// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

/// Explains slow frames. Hot paths add the time they take to a phase; at the
/// end of each emulated frame, a frame slower than the threshold is logged
/// with the time and count of every phase, then all counters reset. Costs two
/// clock reads per scope, so scopes go around whole operations (a draw call,
/// a texture upload), not per-vertex work. docs/hitch-trace.md explains the
/// log lines.
namespace Common::HitchTrace {

enum class Phase : std::uint8_t {
    Cpu,           ///< Guest CPU slices (interpreter or JIT), including AOT and HLE work.
    Aot,           ///< Ahead-of-time translated guest code, including SVCs it calls.
    AotSvc,        ///< SVCs (kernel, services, MM3D hooks) called from AOT code.
    Draw,          ///< PICA draw calls: vertex loading, CPU vertex shading, submission.
    TextureUpload, ///< Decoding and uploading guest textures.
    CustomTexture, ///< Looking up and loading replacement textures.
    PipelineBuild, ///< Building Vulkan pipelines on a worker thread.
    PipelineInline, ///< Cache-only pipeline builds tried on the draw thread.
    FileRead,      ///< Game file reads through the FS service.
    GpuWait,       ///< Emulation thread waiting for the GPU (scheduler fences, readbacks).
    WorkerWait,    ///< Emulation thread waiting for the Vulkan worker to record.
    Present,       ///< Presenting the frame (the emulation thread's whole swap).
    PresentSlot,   ///< Swap: waiting for a free frame from the presentation thread.
    PresentFence,  ///< Swap: waiting for that frame's previous presentation to finish.
    PresentFinish, ///< Swap: draining the GPU when nothing was presented.
    Pacing,        ///< Presentation thread: midpoint pacing sleep.
    PresentSubmit, ///< Presentation thread: queue lock and composition submit.
    PresentGpu,    ///< Presentation thread: waiting for the GPU to finish the frame.
    Deliver,       ///< Presentation thread: handing the frame to the external presenter.
    DeliverBlit,   ///< Deliver: waiting for the Metal copy into the RealityKit ring.
    SwapAcquire,   ///< Presentation thread: acquiring a swapchain image.
    SwapPresent,   ///< Presentation thread: queue present.
    Gpu,           ///< GPU time of command buffers that completed (timestamp queries).
    Count,
};

void Add(Phase phase, std::chrono::nanoseconds time, std::uint32_t count = 1);

/// Time per HLE call: kernel SVCs, service commands and MM3D hook SVCs, so a
/// slow frame shows which ones ran long. Calls nest (a service command runs
/// inside SendSyncRequest). Emulation thread only; `kind` and `name` must stay
/// valid until the next EndFrame. `name` may be nullptr, then `id` is shown.
void AddCall(const char* kind, const char* name, std::uint32_t id, std::chrono::nanoseconds time);

/// Where the game is (scene, entrance), shown on Hitch lines. Emulation
/// thread only; call it when the place changes.
void SetPlace(std::string place);

struct FrameTotals {
    std::chrono::nanoseconds frame_time;
    bool slow;
    std::array<std::int64_t, static_cast<std::size_t>(Phase::Count)> nanoseconds;
};
/// Adds detail to slow frames. Called on the emulation thread at the end of
/// every frame: a reporter resets its per-frame state and, for a slow frame,
/// returns a line to log after the Hitch line (empty for none).
using Reporter = std::string (*)(const FrameTotals& frame);
void AddReporter(Reporter reporter);

/// Called once per emulated frame with its wall-clock duration.
void EndFrame(std::chrono::nanoseconds frame_time);

class Scope {
public:
    explicit Scope(Phase phase_) : phase{phase_}, start{std::chrono::steady_clock::now()} {}
    ~Scope() {
        Add(phase, std::chrono::steady_clock::now() - start);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Phase phase;
    std::chrono::steady_clock::time_point start;
};

class CallScope {
public:
    CallScope(const char* kind_, const char* name_, std::uint32_t id_ = 0)
        : kind{kind_}, name{name_}, id{id_}, start{std::chrono::steady_clock::now()} {}
    ~CallScope() {
        AddCall(kind, name, id, std::chrono::steady_clock::now() - start);
    }
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;

private:
    const char* kind;
    const char* name;
    std::uint32_t id;
    std::chrono::steady_clock::time_point start;
};

} // namespace Common::HitchTrace
