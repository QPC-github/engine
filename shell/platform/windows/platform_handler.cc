// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/platform_handler.h"

#include <windows.h>

#include <cstring>
#include <optional>

#include "flutter/fml/logging.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/platform/win/wstring_conversion.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/method_result_functions.h"
#include "flutter/shell/platform/common/json_method_codec.h"
#include "flutter/shell/platform/windows/flutter_windows_view.h"

static constexpr char kChannelName[] = "flutter/platform";

static constexpr char kGetClipboardDataMethod[] = "Clipboard.getData";
static constexpr char kHasStringsClipboardMethod[] = "Clipboard.hasStrings";
static constexpr char kSetClipboardDataMethod[] = "Clipboard.setData";
static constexpr char kExitApplicationMethod[] = "System.exitApplication";
static constexpr char kRequestAppExitMethod[] = "System.requestAppExit";
static constexpr char kPlaySoundMethod[] = "SystemSound.play";

static constexpr char kExitCodeKey[] = "exitCode";

static constexpr char kExitTypeKey[] = "type";
static constexpr char kExitTypeCancelable[] = "cancelable";
static constexpr char kExitTypeRequired[] = "required";

static constexpr char kExitResponseKey[] = "response";
static constexpr char kExitResponseCancel[] = "cancel";
static constexpr char kExitResponseExit[] = "exit";

static constexpr char kTextPlainFormat[] = "text/plain";
static constexpr char kTextKey[] = "text";
static constexpr char kUnknownClipboardFormatMessage[] =
    "Unknown clipboard format";

static constexpr char kValueKey[] = "value";
static constexpr int kAccessDeniedErrorCode = 5;
static constexpr int kErrorSuccess = 0;

namespace flutter {

namespace {

// A scoped wrapper for GlobalAlloc/GlobalFree.
class ScopedGlobalMemory {
 public:
  // Allocates |bytes| bytes of global memory with the given flags.
  ScopedGlobalMemory(unsigned int flags, size_t bytes) {
    memory_ = ::GlobalAlloc(flags, bytes);
    if (!memory_) {
      FML_LOG(ERROR) << "Unable to allocate global memory: "
                     << ::GetLastError();
    }
  }

  ~ScopedGlobalMemory() {
    if (memory_) {
      if (::GlobalFree(memory_) != nullptr) {
        FML_LOG(ERROR) << "Failed to free global allocation: "
                       << ::GetLastError();
      }
    }
  }

  // Returns the memory pointer, which will be nullptr if allocation failed.
  void* get() { return memory_; }

  void* release() {
    void* memory = memory_;
    memory_ = nullptr;
    return memory;
  }

 private:
  HGLOBAL memory_;

  FML_DISALLOW_COPY_AND_ASSIGN(ScopedGlobalMemory);
};

// A scoped wrapper for GlobalLock/GlobalUnlock.
class ScopedGlobalLock {
 public:
  // Attempts to acquire a global lock on |memory| for the life of this object.
  ScopedGlobalLock(HGLOBAL memory) {
    source_ = memory;
    if (memory) {
      locked_memory_ = ::GlobalLock(memory);
      if (!locked_memory_) {
        FML_LOG(ERROR) << "Unable to acquire global lock: " << ::GetLastError();
      }
    }
  }

  ~ScopedGlobalLock() {
    if (locked_memory_) {
      if (!::GlobalUnlock(source_)) {
        DWORD error = ::GetLastError();
        if (error != NO_ERROR) {
          FML_LOG(ERROR) << "Unable to release global lock: "
                         << ::GetLastError();
        }
      }
    }
  }

  // Returns the locked memory pointer, which will be nullptr if acquiring the
  // lock failed.
  void* get() { return locked_memory_; }

 private:
  HGLOBAL source_;
  void* locked_memory_;

  FML_DISALLOW_COPY_AND_ASSIGN(ScopedGlobalLock);
};

// A Clipboard wrapper that automatically closes the clipboard when it goes out
// of scope.
class ScopedClipboard : public ScopedClipboardInterface {
 public:
  ScopedClipboard();
  virtual ~ScopedClipboard();

  int Open(HWND window) override;

  bool HasString() override;

  std::variant<std::wstring, int> GetString() override;

  int SetString(const std::wstring string) override;

 private:
  bool opened_ = false;

  FML_DISALLOW_COPY_AND_ASSIGN(ScopedClipboard);
};

ScopedClipboard::ScopedClipboard() {}

ScopedClipboard::~ScopedClipboard() {
  if (opened_) {
    ::CloseClipboard();
  }
}

int ScopedClipboard::Open(HWND window) {
  opened_ = ::OpenClipboard(window);

  if (!opened_) {
    return ::GetLastError();
  }

  return kErrorSuccess;
}

bool ScopedClipboard::HasString() {
  // Allow either plain text format, since getting data will auto-interpolate.
  return ::IsClipboardFormatAvailable(CF_UNICODETEXT) ||
         ::IsClipboardFormatAvailable(CF_TEXT);
}

std::variant<std::wstring, int> ScopedClipboard::GetString() {
  FML_DCHECK(opened_) << "Called GetString when clipboard is closed";

  HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
  if (data == nullptr) {
    return ::GetLastError();
  }
  ScopedGlobalLock locked_data(data);

  if (!locked_data.get()) {
    return ::GetLastError();
  }
  return static_cast<wchar_t*>(locked_data.get());
}

int ScopedClipboard::SetString(const std::wstring string) {
  FML_DCHECK(opened_) << "Called GetString when clipboard is closed";
  if (!::EmptyClipboard()) {
    return ::GetLastError();
  }
  size_t null_terminated_byte_count =
      sizeof(decltype(string)::traits_type::char_type) * (string.size() + 1);
  ScopedGlobalMemory destination_memory(GMEM_MOVEABLE,
                                        null_terminated_byte_count);
  ScopedGlobalLock locked_memory(destination_memory.get());
  if (!locked_memory.get()) {
    return ::GetLastError();
  }
  memcpy(locked_memory.get(), string.c_str(), null_terminated_byte_count);
  if (!::SetClipboardData(CF_UNICODETEXT, locked_memory.get())) {
    return ::GetLastError();
  }
  // The clipboard now owns the global memory.
  destination_memory.release();
  return kErrorSuccess;
}

}  // namespace

PlatformHandler::PlatformHandler(
    BinaryMessenger* messenger,
    FlutterWindowsEngine* engine,
    std::optional<std::function<std::unique_ptr<ScopedClipboardInterface>()>>
        scoped_clipboard_provider)
    : channel_(std::make_unique<MethodChannel<rapidjson::Document>>(
          messenger,
          kChannelName,
          &JsonMethodCodec::GetInstance())),
      engine_(engine) {
  channel_->SetMethodCallHandler(
      [this](const MethodCall<rapidjson::Document>& call,
             std::unique_ptr<MethodResult<rapidjson::Document>> result) {
        HandleMethodCall(call, std::move(result));
      });
  if (scoped_clipboard_provider.has_value()) {
    scoped_clipboard_provider_ = scoped_clipboard_provider.value();
  } else {
    scoped_clipboard_provider_ = []() {
      return std::make_unique<ScopedClipboard>();
    };
  }
}

PlatformHandler::~PlatformHandler() = default;

void PlatformHandler::GetPlainText(
    std::unique_ptr<MethodResult<rapidjson::Document>> result,
    std::string_view key) {
  const FlutterWindowsView* view = engine_->view();
  if (view == nullptr) {
    result->Error(kClipboardError,
                  "Clipboard is not available in Windows headless mode");
    return;
  }

  std::unique_ptr<ScopedClipboardInterface> clipboard =
      scoped_clipboard_provider_();

  int open_result = clipboard->Open(std::get<HWND>(*view->GetRenderTarget()));
  if (open_result != kErrorSuccess) {
    rapidjson::Document error_code;
    error_code.SetInt(open_result);
    result->Error(kClipboardError, "Unable to open clipboard", error_code);
    return;
  }
  if (!clipboard->HasString()) {
    result->Success(rapidjson::Document());
    return;
  }
  std::variant<std::wstring, int> get_string_result = clipboard->GetString();
  if (std::holds_alternative<int>(get_string_result)) {
    rapidjson::Document error_code;
    error_code.SetInt(std::get<int>(get_string_result));
    result->Error(kClipboardError, "Unable to get clipboard data", error_code);
    return;
  }

  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
  document.AddMember(
      rapidjson::Value(key.data(), allocator),
      rapidjson::Value(
          fml::WideStringToUtf8(std::get<std::wstring>(get_string_result)),
          allocator),
      allocator);
  result->Success(document);
}

void PlatformHandler::GetHasStrings(
    std::unique_ptr<MethodResult<rapidjson::Document>> result) {
  const FlutterWindowsView* view = engine_->view();
  if (view == nullptr) {
    result->Error(kClipboardError,
                  "Clipboard is not available in Windows headless mode");
    return;
  }

  std::unique_ptr<ScopedClipboardInterface> clipboard =
      scoped_clipboard_provider_();

  bool hasStrings;
  int open_result = clipboard->Open(std::get<HWND>(*view->GetRenderTarget()));
  if (open_result != kErrorSuccess) {
    // Swallow errors of type ERROR_ACCESS_DENIED. These happen when the app is
    // not in the foreground and GetHasStrings is irrelevant.
    // See https://github.com/flutter/flutter/issues/95817.
    if (open_result != kAccessDeniedErrorCode) {
      rapidjson::Document error_code;
      error_code.SetInt(open_result);
      result->Error(kClipboardError, "Unable to open clipboard", error_code);
      return;
    }
    hasStrings = false;
  } else {
    hasStrings = clipboard->HasString();
  }

  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
  document.AddMember(rapidjson::Value(kValueKey, allocator),
                     rapidjson::Value(hasStrings), allocator);
  result->Success(document);
}

void PlatformHandler::SetPlainText(
    const std::string& text,
    std::unique_ptr<MethodResult<rapidjson::Document>> result) {
  const FlutterWindowsView* view = engine_->view();
  if (view == nullptr) {
    result->Error(kClipboardError,
                  "Clipboard is not available in Windows headless mode");
    return;
  }

  std::unique_ptr<ScopedClipboardInterface> clipboard =
      scoped_clipboard_provider_();

  int open_result = clipboard->Open(std::get<HWND>(*view->GetRenderTarget()));
  if (open_result != kErrorSuccess) {
    rapidjson::Document error_code;
    error_code.SetInt(open_result);
    result->Error(kClipboardError, "Unable to open clipboard", error_code);
    return;
  }
  int set_result = clipboard->SetString(fml::Utf8ToWideString(text));
  if (set_result != kErrorSuccess) {
    rapidjson::Document error_code;
    error_code.SetInt(set_result);
    result->Error(kClipboardError, "Unable to set clipboard data", error_code);
    return;
  }
  result->Success();
}

void PlatformHandler::SystemSoundPlay(
    const std::string& sound_type,
    std::unique_ptr<MethodResult<rapidjson::Document>> result) {
  if (sound_type.compare(kSoundTypeAlert) == 0) {
    MessageBeep(MB_OK);
    result->Success();
  } else {
    result->NotImplemented();
  }
}

void PlatformHandler::SystemExitApplication(
    const std::string& exit_type,
    int64_t exit_code,
    std::unique_ptr<MethodResult<rapidjson::Document>> result) {
  rapidjson::Document result_doc;
  result_doc.SetObject();
  if (exit_type.compare(kExitTypeRequired) == 0) {
    QuitApplication(exit_code);
    result_doc.GetObjectW().AddMember(kExitResponseKey, kExitResponseExit,
                                      result_doc.GetAllocator());
    result->Success(result_doc);
  } else {
    RequestAppExit(exit_type, exit_code);
    result_doc.GetObjectW().AddMember(kExitResponseKey, kExitResponseCancel,
                                      result_doc.GetAllocator());
    result->Success(result_doc);
  }
}

void PlatformHandler::RequestAppExit(const std::string& exit_type,
                                     int64_t exit_code) {
  auto callback = std::make_unique<MethodResultFunctions<rapidjson::Document>>(
      [this, exit_code](const rapidjson::Document* response) {
        RequestAppExitSuccess(response, exit_code);
      },
      nullptr, nullptr);
  auto args = std::make_unique<rapidjson::Document>();
  args->SetObject();
  args->GetObjectW().AddMember(kExitTypeKey, exit_type, args->GetAllocator());
  channel_->InvokeMethod(kRequestAppExitMethod, std::move(args),
                         std::move(callback));
}

void PlatformHandler::RequestAppExitSuccess(const rapidjson::Document* result,
                                            int64_t exit_code) {
  const std::string& exit_type = result[0][kExitResponseKey].GetString();
  if (exit_type.compare(kExitResponseExit) == 0) {
    QuitApplication(exit_code);
  }
}

void PlatformHandler::QuitApplication(int64_t exit_code) {
  PostQuitMessage(exit_code);
}

void PlatformHandler::HandleMethodCall(
    const MethodCall<rapidjson::Document>& method_call,
    std::unique_ptr<MethodResult<rapidjson::Document>> result) {
  const std::string& method = method_call.method_name();
  if (method.compare(kExitApplicationMethod) == 0) {
    const rapidjson::Value& arguments = method_call.arguments()[0];
    const std::string& exit_type = arguments[kExitTypeKey].GetString();
    int64_t exit_code = arguments[kExitCodeKey].GetInt64();
    SystemExitApplication(exit_type, exit_code, std::move(result));
  } else if (method.compare(kGetClipboardDataMethod) == 0) {
    // Only one string argument is expected.
    const rapidjson::Value& format = method_call.arguments()[0];

    if (strcmp(format.GetString(), kTextPlainFormat) != 0) {
      result->Error(kClipboardError, kUnknownClipboardFormatMessage);
      return;
    }
    GetPlainText(std::move(result), kTextKey);
  } else if (method.compare(kHasStringsClipboardMethod) == 0) {
    // Only one string argument is expected.
    const rapidjson::Value& format = method_call.arguments()[0];

    if (strcmp(format.GetString(), kTextPlainFormat) != 0) {
      result->Error(kClipboardError, kUnknownClipboardFormatMessage);
      return;
    }
    GetHasStrings(std::move(result));
  } else if (method.compare(kSetClipboardDataMethod) == 0) {
    const rapidjson::Value& document = *method_call.arguments();
    rapidjson::Value::ConstMemberIterator itr = document.FindMember(kTextKey);
    if (itr == document.MemberEnd()) {
      result->Error(kClipboardError, kUnknownClipboardFormatMessage);
      return;
    }
    if (!itr->value.IsString()) {
      result->Error(kClipboardError, kUnknownClipboardFormatMessage);
      return;
    }
    SetPlainText(itr->value.GetString(), std::move(result));
  } else if (method.compare(kPlaySoundMethod) == 0) {
    // Only one string argument is expected.
    const rapidjson::Value& sound_type = method_call.arguments()[0];

    SystemSoundPlay(sound_type.GetString(), std::move(result));
  } else {
    result->NotImplemented();
  }
}

}  // namespace flutter
