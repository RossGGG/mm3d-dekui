// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/param_package.h"
#include "common/quaternion.h"
#include "common/settings.h"
#include "core/frontend/input.h"
#include "dekui_frontend/motion.h"
#include "input_common/main.h"
#include "mm3d/controller_autoconfig.h"
#include "mm3d/player_settings.h"
#include "mm3d/wide_view.h"

namespace Dekui::Motion {

namespace {

using Vec3 = Common::Vec3<float>;
using Quat = Common::Quaternion<float>;
using Status = std::tuple<Vec3, Vec3>;
using Clock = std::chrono::steady_clock;

constexpr char engine[] = "dekui_motion";
/// A console lying still, face up: the accelerometer reads gravity along -y.
const Vec3 rest_accel{0.0f, -1.0f, 0.0f};
/// Head poses older than this mean tracking stopped.
constexpr auto head_timeout = std::chrono::milliseconds(250);
/// Smooths pose-to-pose differences without adding noticeable lag.
constexpr double head_smoothing_seconds = 0.02;

std::atomic<Settings::MotionSource> source{Settings::MotionSource::Controller};
std::atomic<float> sensitivity{1.0f};

/// The controller whose sensors drive the console, chosen on the UI thread.
struct Controller {
    std::string key;
    std::shared_ptr<Input::MotionDevice> device;
    bool has_accel = false;
};
std::mutex controller_mutex;
Controller controller;

struct Head {
    bool tracking = false;
    Quat pose{};
    Quat reference{{0.0f, 0.0f, 0.0f}, 1.0f};
    double seconds = 0;
    Clock::time_point received{};
    Vec3 rate_deg{}; // head axes, degrees per second
};
std::mutex head_mutex;
Head head;

/// Head (x+ right, y+ up, z+ back) and SDL controller axes to 3DS axes. The
/// same mapping as InputCommon's SDL gyroscope.
Vec3 ToConsoleRate(const Vec3& head_rate) {
    return {-head_rate.x, head_rate.y, -head_rate.z};
}

Status HeadStatus() {
    std::lock_guard lock{head_mutex};
    if (!head.tracking || Clock::now() - head.received > head_timeout) {
        return {rest_accel, {}};
    }
    // World up in the head's frame, relative to the recentred orientation.
    const Quat relative = head.reference.Inverse() * head.pose;
    const Vec3 up = Common::QuaternionRotate(relative.Inverse(), Vec3{0.0f, 1.0f, 0.0f});
    // Accelerometers read the reaction to gravity; InputCommon's SDL mapping
    // flips y for the 3DS, which reports gravity itself along -y at rest.
    const Vec3 accel{up.x, -up.y, up.z};
    return {accel, ToConsoleRate(head.rate_deg) * sensitivity.load()};
}

Status ControllerStatus(const Input::MotionDevice& fallback) {
    std::shared_ptr<Input::MotionDevice> device;
    bool has_accel = false;
    {
        std::lock_guard lock{controller_mutex};
        device = controller.device;
        has_accel = controller.has_accel;
    }
    if (!device) {
        return fallback.GetStatus();
    }
    auto [accel, gyro] = device->GetStatus();
    return {has_accel ? accel : rest_accel, gyro * sensitivity.load()};
}

class MotionDevice final : public Input::MotionDevice {
public:
    explicit MotionDevice(std::unique_ptr<Input::MotionDevice> profile_)
        : profile{std::move(profile_)} {}

    Status GetStatus() const override {
        switch (source.load()) {
        case Settings::MotionSource::Off:
            return {rest_accel, {}};
        case Settings::MotionSource::HeadTracking:
            return HeadStatus();
        case Settings::MotionSource::Controller:
        default:
            return ControllerStatus(*profile);
        }
    }

private:
    std::unique_ptr<Input::MotionDevice> profile;
};

class Factory final : public Input::Factory<Input::MotionDevice> {
public:
    /// "profile": the input profile's own motion device.
    std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<MotionDevice>(
            Input::CreateDevice<Input::MotionDevice>(params.Get("profile", "")));
    }
};

bool registered = false;
unsigned follow_ticks = 0;

void Publish() {
    using namespace MM3D::PlayerSettings;
    source.store(Settings::values.motion_source.GetValue());
    // The immersive view uses a fixed sensitivity that matches its screen.
    sensitivity.store(MM3D::WideView::enabled.load()
                          ? MM3D::WideView::motion_sensitivity
                          : Settings::values.motion_sensitivity.GetValue() / 100.0f);
    motion_source.store(int(Settings::values.motion_source.GetValue()));
    motion_sensitivity.store(int(Settings::values.motion_sensitivity.GetValue()));
}

} // namespace

void Init() {
    if (!registered) {
        Input::RegisterFactory<Input::MotionDevice>(engine, std::make_shared<Factory>());
        registered = true;
    }
    Publish();
}

void Shutdown() {
    if (registered) {
        Input::UnregisterFactory<Input::MotionDevice>(engine);
        registered = false;
    }
    std::lock_guard lock{controller_mutex};
    controller = {};
}

bool Tick() {
    using namespace MM3D::PlayerSettings;
    bool changed = false;
    if (const int value = set_motion_source.exchange(-1);
        value >= int(Settings::MotionSource::Off) &&
        value <= int(Settings::MotionSource::HeadTracking)) {
        Settings::values.motion_source.SetValue(Settings::MotionSource(value));
        changed = true;
    }
    if (const int value = set_motion_sensitivity.exchange(-1); value >= 0) {
        Settings::values.motion_sensitivity.SetValue(u32(std::clamp(value, 25, 400)));
        changed = true;
    }
    Publish();

    // Hotplug and profile changes; GetConnectedControllers takes SDL's lock.
    if (follow_ticks++ % 15 == 0) {
        const auto chosen = MM3D::ControllerAutoConfig::MotionController(
            Settings::values.current_input_profile, InputCommon::GetConnectedControllers());
        std::string key;
        if (chosen) {
            key = chosen->Get("guid", "") + "/" + std::to_string(chosen->Get("port", 0));
        }
        std::lock_guard lock{controller_mutex};
        if (key != controller.key) {
            controller = {};
            controller.key = key;
            if (chosen) {
                const Common::ParamPackage params{{"engine", "sdl"},
                                                  {"guid", chosen->Get("guid", "")},
                                                  {"port", chosen->Get("port", "0")}};
                controller.device = Input::CreateDevice<Input::MotionDevice>(params.Serialize());
                controller.has_accel = chosen->Get("accel", 0) != 0;
                LOG_INFO(Input, "Motion controls follow controller {}", key);
            }
        }
    }
    return changed;
}

void SetHeadPose(float x, float y, float z, float w, double seconds) {
    const Quat pose = Quat{{x, y, z}, w}.Normalized();
    std::lock_guard lock{head_mutex};
    const double dt = seconds - head.seconds;
    if (head.tracking && dt > 0 && dt < 0.25) {
        // Rotation since the last pose, in the head's own frame.
        Quat delta = head.pose.Inverse() * pose;
        if (delta.w < 0) {
            delta = {-delta.xyz, -delta.w};
        }
        const float s = delta.xyz.Length();
        const float angle = 2.0f * std::atan2(s, delta.w);
        const Vec3 rate = s > 1e-7f ? delta.xyz * (angle / s / float(dt)) : delta.xyz * (2.0f / float(dt));
        const Vec3 rate_deg = rate * (180.0f / Common::PI);
        const float blend = float(1.0 - std::exp(-dt / head_smoothing_seconds));
        head.rate_deg = head.rate_deg + (rate_deg - head.rate_deg) * blend;
    } else if (!head.tracking || dt >= 0.25) {
        head.rate_deg = {};
    }
    if (dt > 0 || !head.tracking) {
        head.pose = pose;
        head.seconds = seconds;
    }
    head.tracking = true;
    head.received = Clock::now();
}

void ClearHeadPose() {
    std::lock_guard lock{head_mutex};
    head.tracking = false;
    head.rate_deg = {};
}

void RecenterHead() {
    std::lock_guard lock{head_mutex};
    if (head.tracking) {
        head.reference = head.pose;
    }
}

} // namespace Dekui::Motion
