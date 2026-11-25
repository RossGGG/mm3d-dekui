// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Motion controls: chooses what the emulated 3DS gyroscope and accelerometer
// read (Settings::values.motion_source). Registers the "dekui_motion" input
// engine, which HID uses in place of the input profile's motion device and
// which wraps that device for the Controller source when no connected
// controller has a gyroscope.
//
// 3DS axes (core/frontend/input.h): x+ left, y+ out of the touch screen, z+
// towards the top of the D-pad. A controller, or the wearer's head, counts as
// a 3DS lying flat in front of the player: turning left is a turn about y.

#pragma once

namespace Dekui::Motion {

/// Registers the input engine. Call once after InputCommon::Init().
void Init();
void Shutdown();

/// UI thread, every frame: applies requests from the settings UIs
/// (MM3D::PlayerSettings::set_motion_*) and follows controller hotplug.
/// Returns true when a setting changed and the configuration should be saved.
bool Tick();

/// The wearer's head pose from the frontend's tracking, as a unit quaternion
/// rotating head-local vectors into a gravity-aligned world (y+ up). Head
/// axes: x+ right, y+ up, z+ backwards (ARKit's device anchor). `seconds` is
/// the pose's timestamp. Any thread.
void SetHeadPose(float x, float y, float z, float w, double seconds);
/// Tracking stopped; the console lies still until the next pose.
void ClearHeadPose();
/// Takes the current head pose as the console's resting, level orientation.
void RecenterHead();

} // namespace Dekui::Motion
