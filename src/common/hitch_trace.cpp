// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "common/hitch_trace.h"
#include "common/logging/log.h"

namespace Common::HitchTrace {

namespace {

/// Frames slower than this are logged; a 30/60 Hz frame is 33/17 ms.
constexpr auto Threshold = std::chrono::milliseconds(50);

constexpr std::array<const char*, static_cast<std::size_t>(Phase::Count)> Names{
    "cpu", "aot", "aot svc", "draws", "texture uploads", "custom textures", "pipeline builds",
    "inline pipeline builds", "file reads", "gpu wait", "worker wait", "present",
    "present slot", "present fence", "present finish", "pacing", "present submit",
    "present gpu", "deliver", "deliver blit", "swap acquire", "swap present", "gpu"};

struct Counter {
    std::atomic<std::int64_t> nanoseconds{0};
    std::atomic<std::int64_t> max{0};
    std::atomic<std::uint32_t> count{0};
};

std::array<Counter, static_cast<std::size_t>(Phase::Count)> counters;
std::uint64_t frames_since_hitch = 0;

struct Call {
    const char* kind;
    const char* name;
    std::uint32_t id;
    std::uint32_t count;
    std::int64_t nanoseconds;
};
/// Distinct calls this frame. A frame makes a few dozen kinds of call, so a
/// linear search is cheaper than hashing; overflow is counted, not stored.
std::array<Call, 96> calls;
std::size_t call_count = 0;
std::int64_t dropped_call_ns = 0;

std::string place;
std::chrono::steady_clock::time_point place_since;

/// Every frame, slow or not, for the periodic "Frame summary" line: hitch lines
/// alone cannot show whether smooth frames got cheaper.
constexpr auto SummaryInterval = std::chrono::seconds(10);
struct Summary {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::vector<float> frame_ms, gpu_ms;
    std::array<double, static_cast<std::size_t>(Phase::Count)> wait_ms{};
} summary;
constexpr std::array SummaryWaits{Phase::Present, Phase::PresentSlot, Phase::GpuWait,
                                  Phase::PresentGpu, Phase::DeliverBlit};

void Summarize(const FrameTotals& frame) {
    summary.frame_ms.push_back(frame.frame_time.count() / 1e6f);
    summary.gpu_ms.push_back(frame.nanoseconds[static_cast<std::size_t>(Phase::Gpu)] / 1e6f);
    for (const Phase phase : SummaryWaits)
        summary.wait_ms[static_cast<std::size_t>(phase)] +=
            frame.nanoseconds[static_cast<std::size_t>(phase)] / 1e6;
    const auto now = std::chrono::steady_clock::now();
    if (now - summary.start < SummaryInterval)
        return;
    const auto stats = [](std::vector<float>& values) {
        double total = 0;
        for (const float value : values)
            total += value;
        const auto p90 = values.begin() + values.size() * 9 / 10;
        std::nth_element(values.begin(), p90, values.end());
        return fmt::format("avg {:.1f} p90 {:.1f}", total / values.size(), *p90);
    };
    const std::size_t frames = summary.frame_ms.size();
    const std::chrono::duration<double> elapsed = now - summary.start;
    const float max = *std::max_element(summary.frame_ms.begin(), summary.frame_ms.end());
    std::string line = fmt::format("{} frames in {:.1f} s | frame {} max {:.1f} ms | gpu {} ms",
                                   frames, elapsed.count(), stats(summary.frame_ms), max,
                                   stats(summary.gpu_ms));
    for (const Phase phase : SummaryWaits)
        line += fmt::format(" | {} avg {:.1f}", Names[static_cast<std::size_t>(phase)],
                            summary.wait_ms[static_cast<std::size_t>(phase)] / frames);
    if (!place.empty())
        line += fmt::format(" | at {}", place);
    LOG_INFO(Frontend, "Frame summary: {}", line);
    summary = {};
}

std::vector<Reporter> reporters;

std::string CallsLine() {
    std::sort(calls.begin(), calls.begin() + call_count,
              [](const Call& a, const Call& b) { return a.nanoseconds > b.nanoseconds; });
    std::string line;
    const auto separate = [&line] { return line.empty() ? "" : " | "; };
    for (std::size_t i = 0; i < std::min<std::size_t>(call_count, 10); ++i) {
        const Call& call = calls[i];
        if (call.nanoseconds < 100'000)
            break;
        line += call.name ? fmt::format("{}{} {}", separate(), call.kind, call.name)
                          : fmt::format("{}{} {:#x}", separate(), call.kind, call.id);
        line += fmt::format(" {:.1f} ms ({})", call.nanoseconds / 1e6, call.count);
    }
    if (dropped_call_ns)
        line += fmt::format("{}untracked {:.1f} ms", separate(), dropped_call_ns / 1e6);
    return line;
}

} // namespace

void Add(Phase phase, std::chrono::nanoseconds time, std::uint32_t count) {
    auto& counter = counters[static_cast<std::size_t>(phase)];
    const std::int64_t ns = time.count();
    counter.nanoseconds.fetch_add(ns, std::memory_order_relaxed);
    counter.count.fetch_add(count, std::memory_order_relaxed);
    std::int64_t max = counter.max.load(std::memory_order_relaxed);
    while (ns > max && !counter.max.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {
    }
}

void AddCall(const char* kind, const char* name, std::uint32_t id, std::chrono::nanoseconds time) {
    for (std::size_t i = 0; i < call_count; ++i) {
        Call& call = calls[i];
        if (call.name == name && call.id == id && call.kind == kind) {
            call.nanoseconds += time.count();
            ++call.count;
            return;
        }
    }
    if (call_count == calls.size()) {
        dropped_call_ns += time.count();
        return;
    }
    calls[call_count++] = {kind, name, id, 1, time.count()};
}

void SetPlace(std::string place_) {
    place = std::move(place_);
    place_since = std::chrono::steady_clock::now();
}

void AddReporter(Reporter reporter) {
    if (std::find(reporters.begin(), reporters.end(), reporter) == reporters.end())
        reporters.push_back(reporter);
}

void EndFrame(std::chrono::nanoseconds frame_time) {
    FrameTotals frame{frame_time, frame_time >= Threshold, {}};
    std::array<std::pair<std::uint32_t, std::int64_t>, static_cast<std::size_t>(Phase::Count)>
        counts_max;
    for (std::size_t i = 0; i < counters.size(); ++i) {
        frame.nanoseconds[i] = counters[i].nanoseconds.exchange(0, std::memory_order_relaxed);
        counts_max[i] = {counters[i].count.exchange(0, std::memory_order_relaxed),
                         counters[i].max.exchange(0, std::memory_order_relaxed)};
    }
    ++frames_since_hitch;
    Summarize(frame);
    std::vector<std::string> details;
    for (const Reporter reporter : reporters) {
        std::string line = reporter(frame);
        if (frame.slow && !line.empty())
            details.push_back(std::move(line));
    }
    if (!frame.slow) {
        call_count = 0;
        dropped_call_ns = 0;
        return;
    }
    std::string phases;
    for (std::size_t i = 0; i < frame.nanoseconds.size(); ++i) {
        const std::int64_t nanoseconds = frame.nanoseconds[i];
        const auto [count, max] = counts_max[i];
        if (count == 0 && nanoseconds == 0) {
            continue;
        }
        phases += fmt::format(" | {} {:.1f} ms ({})", Names[i], nanoseconds / 1e6, count);
        // The longest single wait tells one stall from many small ones.
        if (count > 1 && max >= 1'000'000)
            phases.insert(phases.size() - 1, fmt::format(", max {:.1f}", max / 1e6));
    }
    if (!place.empty()) {
        const std::chrono::duration<double> since = std::chrono::steady_clock::now() - place_since;
        phases += fmt::format(" | at {} for {:.0f} s", place, since.count());
    }
    LOG_WARNING(Frontend, "Hitch: {:.1f} ms frame after {} smooth frames{}", frame_time.count() / 1e6,
                frames_since_hitch - 1, phases);
    if (const std::string line = CallsLine(); !line.empty())
        LOG_WARNING(Frontend, "Hitch calls: {}", line);
    for (const std::string& line : details)
        LOG_WARNING(Frontend, "{}", line);
    call_count = 0;
    dropped_call_ns = 0;
    frames_since_hitch = 0;
}

} // namespace Common::HitchTrace
