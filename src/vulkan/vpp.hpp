// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
#include "../overlay-params.h"

namespace irisva {
struct Surface;
struct VppPicture;
class VulkanVpp {
    struct Impl;
    std::shared_ptr<Impl> impl_;

  public:
    explicit VulkanVpp(int render_fd);
    void process(const VppPicture &picture, Surface &target);
    void transform(const VppPicture &picture, Surface &target);
    void overlay(Surface &base, Surface &rgba, Surface &target, const OverlayParams &params);
};
} // namespace irisva
