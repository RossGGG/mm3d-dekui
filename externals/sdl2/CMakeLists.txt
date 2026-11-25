# Configure static library build
set(SDL_SHARED OFF CACHE BOOL "")
set(SDL_STATIC ON CACHE BOOL "")

# Subsystems
set(SDL_ATOMIC ON CACHE BOOL "")
set(SDL_AUDIO ON CACHE BOOL "")
if(CITRA_VISIONOS OR CITRA_TVOS)
    # The app (SwiftUI or Qt) owns the application window on visionOS and
    # tvOS. SDL's UIKit video backend depends on UIScreen and OpenGL ES APIs
    # that are unavailable on visionOS, and would take over UIApplication;
    # controller input only needs the event and joystick subsystems.
    set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
    # SDL's iOS URL launcher calls UIApplication's legacy openURL: API,
    # which is unavailable on visionOS. DekUI uses Qt for URL handling.
    set(SDL_MISC OFF CACHE BOOL "" FORCE)
    # Raw HID transport is not available in the visionOS SDK. SDL's
    # GameController-based joystick support remains enabled.
    set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
else()
    set(SDL_VIDEO ON CACHE BOOL "")
endif()
set(SDL_RENDER OFF CACHE BOOL "")
set(SDL_EVENTS ON CACHE BOOL "")
set(SDL_JOYSTICK ON CACHE BOOL "")
set(SDL_HAPTIC OFF CACHE BOOL "")
set(SDL_HIDAPI ON CACHE BOOL "")
set(SDL_POWER OFF CACHE BOOL "")
set(SDL_THREADS ON CACHE BOOL "")
set(SDL_TIMERS ON CACHE BOOL "")
set(SDL_FILE ON CACHE BOOL "")
set(SDL_LOADSO ON CACHE BOOL "")
set(SDL_CPUINFO ON CACHE BOOL "")
set(SDL_FILESYSTEM OFF CACHE BOOL "")
set(SDL_DLOPEN ON CACHE BOOL "")
set(SDL_SENSOR OFF CACHE BOOL "")
set(SDL_LOCALE OFF CACHE BOOL "")

add_subdirectory(SDL)
