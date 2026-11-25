// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

namespace Frontend {
class SoftwareKeyboard;
}

/// The software keyboard applet for the visionOS app (DKSoftwareKeyboard.mm).
std::shared_ptr<Frontend::SoftwareKeyboard> DekuiMakeSoftwareKeyboard();
