# Dekui's native visionOS and tvOS apps: SwiftUI on top of an Objective-C++
# bridge to the emulator core. Needs only Qt Core (MM3D presentation code and
# the shared qt-config.ini reader); no Qt GUI modules. The Apple TV app has its
# own SwiftUI screens (TV/) and shares the bridge.

set(DIST_DIR "${PROJECT_SOURCE_DIR}/dist/apple")
set(DEKUI_VISIONOS_APP_SOURCES
    App/ControlsView.swift
    App/DekuiApp.swift
    App/EmulatorModel.swift
    App/GameView.swift
    App/HeadTracking.swift
    App/ImmersiveUpscaler.swift
    App/KeyboardPrompt.swift
    App/SaveStatesView.swift
    App/SettingsView.swift
    App/DimSurroundings.swift
    App/SetupView.swift
    App/StereoScreen.swift
    App/WideHalo.swift
    App/WideHud.swift
    App/WideScreen.swift
)
set(DEKUI_TVOS_APP_SOURCES
    App/KeyboardPrompt.swift
    TV/TVApp.swift
    TV/TVControlsView.swift
    TV/TVGameView.swift
    TV/TVMenuView.swift
    TV/TVModel.swift
    TV/TVSaveStatesView.swift
)
if (CITRA_TVOS)
    set(DEKUI_APP_SOURCES ${DEKUI_TVOS_APP_SOURCES})
else()
    set(DEKUI_APP_SOURCES ${DEKUI_VISIONOS_APP_SOURCES})
endif()
set(DEKUI_BRIDGE_SOURCES
    Bridge/Dekui-Bridging-Header.h
    Bridge/DKEmulator.h
    Bridge/DKEmulator.mm
    Bridge/DKEmulator+SaveStates.h
    Bridge/DKEmulator+SaveStates.mm
    Bridge/DKMotion.h
    Bridge/DKMotion.mm
    Bridge/DKSoftwareKeyboard.h
    Bridge/DKSoftwareKeyboard.mm
    Bridge/software_keyboard.h
    Bridge/emu_window_apple.h
    Bridge/emu_window_apple.mm
    Bridge/stereo_frames.h
    Bridge/stereo_frames.mm
    Bridge/icloud_shader_cache.h
    Bridge/icloud_shader_cache.mm
    Bridge/icloud_states.h
    Bridge/icloud_states.mm
    Bridge/cloudkit_states.h
    Bridge/cloudkit_states.mm
)
# The Qt frontend's settings file reader, built against Qt Core only, so both
# apps share user/config/qt-config.ini.
set(DEKUI_CONFIG_SOURCES
    ../citra_qt/configuration/config.cpp
    ../citra_qt/configuration/config.h
    ../citra_qt/uisettings.cpp
    ../citra_qt/uisettings.h
)
set_source_files_properties(${DEKUI_CONFIG_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS DEKUI_CONFIG_QTCORE_ONLY)
if (CITRA_TVOS)
    # Layered (parallax) app icon and Top Shelf images; regenerate with
    # tools/tvos/icon/make_icon.py.
    set(DEKUI_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/TV/Assets.xcassets")
    set(DEKUI_ICON_FILE "")
else()
    set(DEKUI_RESOURCES "${DIST_DIR}/azahar.icns")
    set(DEKUI_ICON_FILE "azahar.icns")
endif()

# The bridge and the emulator sources it includes need src/ on the header
# path, but Swift's Clang importer must not see it: src/network/network.h
# shadows Apple's Network module on a case-insensitive file system.
add_library(dekui_bridge STATIC ${DEKUI_BRIDGE_SOURCES} ${DEKUI_CONFIG_SOURCES})
if (DEKUI_TVOS_ASYNC_MOLTENVK_SUBMITS)
    target_compile_definitions(dekui_bridge PRIVATE
        DEKUI_TVOS_ASYNC_MOLTENVK_SUBMITS=1)
endif()
target_include_directories(dekui_bridge PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)
target_link_libraries(dekui_bridge PRIVATE
    audio_core citra_common citra_core input_common network video_core
    dekui_frontend mm3d_overlay
    Boost::boost fmt Qt6::Core Threads::Threads
)
if (TARGET oaknut)
    target_link_libraries(dekui_bridge PRIVATE oaknut)
endif()
set_target_properties(dekui_bridge PROPERTIES XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)

add_executable(dekui_visionos MACOSX_BUNDLE
    ${DEKUI_APP_SOURCES}
    Bridge/Dekui-Bridging-Header.h
    Bridge/DKEmulator.h
    ${DEKUI_RESOURCES}
)
set_target_properties(dekui_visionos PROPERTIES
    OUTPUT_NAME "Dekui"
    # SwiftUI owns main; Qt's UIKit entry point lives in Qt GUI, which is not linked.
    qt_no_entrypoint TRUE
)
set_property(TARGET dekui_visionos PROPERTY INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/Bridge")
# The project's directory-wide C/C++ warning flags are not Swift options.
set_property(TARGET dekui_visionos PROPERTY COMPILE_OPTIONS "")
target_link_libraries(dekui_visionos PRIVATE dekui_bridge ${PLATFORM_LIBRARIES}
    "-framework Foundation" "-framework UIKit" "-framework SwiftUI"
    "-framework QuartzCore" "-framework Metal" "-framework CoreGraphics"
    "-framework GameController" "-framework AVFoundation" "-framework AudioToolbox"
    "-framework CoreAudio" "-framework CoreHaptics"
    "-framework UniformTypeIdentifiers" "-framework ImageIO" "-framework CloudKit"
)
if (NOT CITRA_TVOS)
    target_link_libraries(dekui_visionos PRIVATE "-framework CoreMotion"
        "-framework RealityKit" "-framework MetalPerformanceShaders"
        "-framework ARKit")
    # Xcode 26 ships MetalFX for visionOS devices but not XRSimulator.
    if (NOT CMAKE_OSX_SYSROOT MATCHES "XRSimulator")
        target_link_libraries(dekui_visionos PRIVATE "-framework MetalFX")
    endif()
endif()

if (CITRA_TVOS)
    set(APPLE_PLATFORM_INFO_PLIST [=[
    <key>CFBundleSupportedPlatforms</key>
    <array>
        <string>AppleTVOS</string>
    </array>
    <key>UIRequiredDeviceCapabilities</key>
    <array>
        <string>arm64</string>
    </array>
    <key>UILaunchScreen</key>
    <dict/>
    <key>UIUserInterfaceStyle</key>
    <string>Dark</string>
    <!-- Extended gamepads navigate Dekui's menus too; while the game view has
         focus, GCEventViewController keeps their presses for the game. -->
    <key>GCSupportsControllerUserInteraction</key>
    <true/>
    <key>GCSupportedGameControllers</key>
    <array>
        <dict>
            <key>ProfileName</key>
            <string>ExtendedGamepad</string>
        </dict>
        <dict>
            <key>ProfileName</key>
            <string>MicroGamepad</string>
        </dict>
    </array>]=])
else()
    set(APPLE_PLATFORM_INFO_PLIST [=[
    <key>CFBundleSupportedPlatforms</key>
    <array>
        <string>XROS</string>
    </array>
    <key>UIApplicationSceneManifest</key>
    <dict>
        <key>UIApplicationSupportsMultipleScenes</key>
        <true/>
        <key>UIApplicationPreferredDefaultSceneSessionRole</key>
        <string>UIWindowSceneSessionRoleApplication</string>
        <key>UISceneConfigurations</key>
        <dict/>
    </dict>
    <key>GCSupportedGameControllers</key>
    <array>
        <dict>
            <key>ProfileName</key>
            <string>ExtendedGamepad</string>
        </dict>
    </array>]=])
endif()
include(GenerateBuildInfo)
set(SRC_DIR "${PROJECT_SOURCE_DIR}")
generate_build_info()
set(CITRA_APPLE_DEVELOPMENT_TEAM "" CACHE STRING
    "Apple development team ID used to sign the visionOS app")
set(CITRA_VISIONOS_JIT26_ARENA_MB "96" CACHE STRING
    "JIT arena size in MiB requested when launched from Xcode")
# A second build (for example one with AOT code) can install beside the main
# app under its own identity. Both share the iCloud container for save states.
set(DEKUI_VISIONOS_BUNDLE_IDENTIFIER "org.azahar-emu.azahar" CACHE STRING
    "Bundle identifier of the visionOS app")
set(DEKUI_VISIONOS_APP_NAME "Dekui" CACHE STRING
    "Name shown for the visionOS app and its Files folder")
set_target_properties(dekui_visionos PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${DIST_DIR}/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "${DEKUI_VISIONOS_APP_NAME}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${DEKUI_VISIONOS_BUNDLE_IDENTIFIER}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${BUILD_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${BUILD_FULLNAME}"
    MACOSX_BUNDLE_LONG_VERSION_STRING "${BUILD_FULLNAME}"
    MACOSX_BUNDLE_ICON_FILE "${DEKUI_ICON_FILE}"
    RESOURCE "${DEKUI_RESOURCES}"
    # Same identifier as the Qt app: installing over it keeps the Documents
    # container with the game and the prepared user folder.
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${DEKUI_VISIONOS_BUNDLE_IDENTIFIER}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER
        "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/Dekui-Bridging-Header.h"
    XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
    XCODE_ATTRIBUTE_SWIFT_OPTIMIZATION_LEVEL "-O"
    XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++20"
    XCODE_GENERATE_SCHEME TRUE
    XCODE_SCHEME_LAUNCH_CONFIGURATION "Release"
)
if (CITRA_TVOS)
    set_target_properties(dekui_visionos PROPERTIES
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "3"
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "App Icon & Top Shelf Image"
        # Increased memory limit and iCloud, like the visionOS app.
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/DekuiTV.entitlements")
else()
    set_target_properties(dekui_visionos PROPERTIES
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "7"
        # Increased memory limit and iCloud (save-state sync). The app ID is
        # UJYC8HY4F2.org.azahar-emu.azahar; see docs/visionos.md.
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/Dekui.entitlements")
endif()
if (CITRA_VISIONOS_JIT26_ARENA_MB AND NOT CITRA_TVOS)
    set_target_properties(dekui_visionos PROPERTIES
        XCODE_SCHEME_ENVIRONMENT "DEKUI_JIT26_ARENA_MB=${CITRA_VISIONOS_JIT26_ARENA_MB}")
endif()
if (CITRA_APPLE_DEVELOPMENT_TEAM)
    set_target_properties(dekui_visionos PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${CITRA_APPLE_DEVELOPMENT_TEAM}")
endif()
