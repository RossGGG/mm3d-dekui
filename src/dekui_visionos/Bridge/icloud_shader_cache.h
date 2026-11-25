// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// Shader caches built on a Mac (tools/mm3d/shader_tour.py) reach the headset
// through the app's iCloud Drive folder, "Dekui/Shader Cache". New versions
// are copied to user/shaders/import/, which the Vulkan shader disk cache merges
// into this device's cache when the game boots.
namespace Dekui::CloudShaderCache {

/// Copies new cache files from iCloud Drive to the import folder. Files not yet
/// downloaded start downloading and are picked up on a later launch.
void Fetch();

} // namespace Dekui::CloudShaderCache
