// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/filters/inputs/texture_filter_input.h"

#include <utility>

#include "impeller/renderer/formats.h"

namespace impeller {

TextureFilterInput::TextureFilterInput(std::shared_ptr<Texture> texture,
                                       Matrix local_transform)
    : texture_(std::move(texture)), local_transform_(local_transform) {}

TextureFilterInput::~TextureFilterInput() = default;

FilterInput::Variant TextureFilterInput::GetInput() const {
  return texture_;
}

std::optional<Snapshot> TextureFilterInput::GetSnapshot(
    const ContentContext& renderer,
    const Entity& entity) const {
  auto snapshot =
      Snapshot{.texture = texture_, .transform = GetTransform(entity)};
  if (texture_->GetMipCount() > 1) {
    snapshot.sampler_descriptor.label = "TextureFilterInput Trilinear Sampler";
    snapshot.sampler_descriptor.mip_filter = MipFilter::kLinear;
  }
  return snapshot;
}

std::optional<Rect> TextureFilterInput::GetCoverage(
    const Entity& entity) const {
  return Rect::MakeSize(texture_->GetSize())
      .TransformBounds(GetTransform(entity));
}

Matrix TextureFilterInput::GetLocalTransform(const Entity& entity) const {
  return local_transform_;
}

}  // namespace impeller
