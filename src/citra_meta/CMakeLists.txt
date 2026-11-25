add_executable(citra_meta
    citra.rc
    main.cpp
    precompiled_headers.h
)

set_target_properties(citra_meta PROPERTIES OUTPUT_NAME "azahar")

if (BSD STREQUAL "NetBSD")
    include(DisablePaxMprotect)
    disable_pax_mprotect(citra_meta)
endif()

if (APPLE)
    set(DIST_DIR "../../dist/apple")
    if (CITRA_VISIONOS)
        set(APPLE_PLATFORM_INFO_PLIST [=[
    <key>CFBundleSupportedPlatforms</key>
    <array>
        <string>XROS</string>
    </array>
    <key>UIApplicationSceneManifest</key>
    <dict>
        <key>UIApplicationSupportsMultipleScenes</key>
        <true/>
        <key>UISceneConfigurations</key>
        <dict/>
    </dict>]=])
    elseif (IOS)
        set(APPLE_PLATFORM_INFO_PLIST [=[
    <key>UILaunchStoryboardName</key>
    <string>LaunchScreen</string>]=])
    else()
        set(APPLE_PLATFORM_INFO_PLIST "")
    endif()
    set(APPLE_RESOURCES
        "${DIST_DIR}/azahar.icns"
        "${DIST_DIR}/launch_logo.png"
    )
    if (IOS)
        list(APPEND APPLE_RESOURCES "${DIST_DIR}/LaunchScreen.storyboard")
    endif()
    target_sources(citra_meta PRIVATE ${APPLE_RESOURCES})

    # Define app bundle metadata.
    include(GenerateBuildInfo)
    set(SRC_DIR "${PROJECT_SOURCE_DIR}")
    generate_build_info()
    set(APPLE_BUNDLE_NAME "Azahar")
    if (ENABLE_MM3D_GAME_FOLDER)
        set(APPLE_BUNDLE_NAME "Dekui")
    endif()
    # Lets side-by-side builds (e.g. the AOT build) keep their own name and,
    # on macOS, their own Application Support data folder.
    set(DEKUI_APPLE_APP_NAME "${APPLE_BUNDLE_NAME}" CACHE STRING
        "Bundle name of the Qt app; also names its macOS data folder")
    set(DEKUI_APPLE_BUNDLE_IDENTIFIER "org.azahar-emu.azahar" CACHE STRING
        "Bundle identifier of the Qt app")
    set(APPLE_BUNDLE_NAME "${DEKUI_APPLE_APP_NAME}")
    set_target_properties(citra_meta PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${DIST_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${APPLE_BUNDLE_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${DEKUI_APPLE_BUNDLE_IDENTIFIER}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${BUILD_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${BUILD_FULLNAME}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${BUILD_FULLNAME}"
        MACOSX_BUNDLE_ICON_FILE "azahar.icns"
        RESOURCE "${APPLE_RESOURCES}"
    )

    if (IOS OR CITRA_VISIONOS)
        if (CITRA_VISIONOS)
            set(CITRA_APPLE_DEVELOPMENT_TEAM "" CACHE STRING
                "Apple development team ID used to sign the visionOS app")
            set_target_properties(citra_meta PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
                XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.azahar-emu.azahar"
                XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "7"
                XCODE_GENERATE_SCHEME TRUE
                # Device Qt is a Release-only SDK. Keep Run on that configuration;
                # Xcode still launches it through LLDB for debugger-assisted JIT.
                XCODE_SCHEME_LAUNCH_CONFIGURATION "Release"
            )
            # Xcode's LLDB prepares JIT memory over Wi-Fi (tools/visionos/jit26_lldb.py),
            # so keep the debugger-prepared arena smaller for Build & Run.
            set(CITRA_VISIONOS_JIT26_ARENA_MB "96" CACHE STRING
                "JIT arena size in MiB requested when launched from Xcode")
            if (CITRA_VISIONOS_JIT26_ARENA_MB)
                set_target_properties(citra_meta PROPERTIES
                    XCODE_SCHEME_ENVIRONMENT "DEKUI_JIT26_ARENA_MB=${CITRA_VISIONOS_JIT26_ARENA_MB}"
                )
            endif()
            if (CITRA_APPLE_DEVELOPMENT_TEAM)
                set_target_properties(citra_meta PROPERTIES
                    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${CITRA_APPLE_DEVELOPMENT_TEAM}"
                )
            endif()
        else()
            set_target_properties(citra_meta PROPERTIES
                # iOS may use the dynamic MoltenVK package; the visionOS port links its archive.
                XCODE_EMBED_FRAMEWORKS "${MOLTENVK_LIBRARY}"
                XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY YES
                XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks"
                # Support iPhone and iPad.
                XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            )
        endif()
    endif()
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND MINGW)
    # TODO: Do this for all executables, not just citra_meta
    # TODO: Don't hardcode MXE directory?
    set(dllwalker_command "${CMAKE_SOURCE_DIR}/externals/dllwalker/dllwalker.rb"
                          $<TARGET_FILE:citra_meta>
                          /opt/mxe/usr/x86_64-w64-mingw32.shared/bin/
                          /opt/mxe/usr/x86_64-w64-mingw32.shared/qt6/bin/
                          /opt/mxe/usr/x86_64-w64-mingw32.shared/x86_64-w64-mingw32/bin/)
    set(dll_list_filename "${CMAKE_CURRENT_BINARY_DIR}/required_dlls_list.txt")
    add_custom_command(TARGET citra_meta POST_BUILD
        COMMAND ${dllwalker_command} > ${dll_list_filename}
    )
    add_custom_command(TARGET citra_meta POST_BUILD
        COMMAND cat ${dll_list_filename} | xargs -I {} ${CMAKE_COMMAND} -E copy {} $<TARGET_FILE_DIR:citra_meta>
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:citra_meta>/plugins/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory /opt/mxe/usr/x86_64-w64-mingw32.shared/qt6/plugins/ "$<TARGET_FILE_DIR:citra_meta>/plugins/"
        COMMAND_EXPAND_LISTS
        DEPENDS ${dll_list_filename}
    )
endif()

target_link_libraries(citra_meta PRIVATE citra_cli citra_common fmt)

if (ENABLE_QT)
    target_link_libraries(citra_meta PRIVATE citra_qt)
    target_link_libraries(citra_meta PRIVATE Boost::boost Qt6::Widgets)
endif()

if (ENABLE_ROOM)
    target_link_libraries(citra_meta PRIVATE citra_room)
endif()

if (ENABLE_QT AND UNIX AND NOT APPLE)
    target_link_libraries(citra_meta PRIVATE Qt6::DBus gamemode)
endif()

if (ENABLE_QT AND APPLE)
    find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
    target_link_libraries(citra_meta PRIVATE Qt6::GuiPrivate)
endif()

if (ENABLE_QT AND ENABLE_DISCORD_RPC)
    target_link_libraries(citra_meta PRIVATE discord-rpc)
endif()

if(WIN32)
    # compile as a win32 gui application instead of a console application
    if(MSVC)
        set_target_properties(citra_meta PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS")
    elseif(MINGW)
        set_target_properties(citra_meta PROPERTIES LINK_FLAGS_RELEASE "-mwindows")
    endif()
endif()

if (CITRA_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(citra_meta PRIVATE precompiled_headers.h)
endif()

# Bundle in-place on MSVC so dependencies can be resolved by builds.
if (ENABLE_QT AND MSVC)
    include(BundleTarget)
    qt_bundle_target_in_place(citra_meta)
endif()

if(UNIX AND NOT APPLE)
    install(TARGETS citra_meta RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
endif()
