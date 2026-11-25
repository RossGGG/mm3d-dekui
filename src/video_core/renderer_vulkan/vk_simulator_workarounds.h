// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// Apple's simulator GPUs lack features every Apple device GPU has. These
// workarounds are compiled only for simulator builds.
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
// Metal there creates textures over buffers only in private storage, so
// host-visible texel buffers abort. Draw from a GPU-private copy instead.
#define CITRA_PRIVATE_TEXEL_BUFFERS 1
// Indexed draws cannot use a non-zero base vertex. Rebase the indices while
// copying them instead.
#define CITRA_REBASE_INDICES 1
#endif

#ifdef CITRA_PRIVATE_TEXEL_BUFFERS

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

class Instance;
class Scheduler;

class PrivateTexelBuffer {
public:
    PrivateTexelBuffer(const Instance& instance, u64 size);
    ~PrivateTexelBuffer();

    PrivateTexelBuffer(const PrivateTexelBuffer&) = delete;
    PrivateTexelBuffer& operator=(const PrivateTexelBuffer&) = delete;

    vk::Buffer Handle() const noexcept {
        return buffer;
    }

    /// Copies [offset, offset + size) from `source`. Record outside a render pass.
    void Upload(Scheduler& scheduler, vk::Buffer source, u64 offset, u64 size);

private:
    const Instance& instance;
    vk::Buffer buffer;
    VmaAllocation allocation{};
};

} // namespace Vulkan

#endif
