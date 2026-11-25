# Frontend-neutral Dekui behaviour shared by the Qt frontend and the native
# visionOS app. QtCore only (through the MM3D game-folder helpers); no QtGui.
add_library(dekui_frontend STATIC
    bootstrap.cpp
    bootstrap.h
    emu_thread.cpp
    emu_thread.h
    input.cpp
    input.h
    motion.cpp
    motion.h
    poller.cpp
    poller.h
)
target_include_directories(dekui_frontend PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(dekui_frontend PUBLIC citra_common
    PRIVATE citra_core input_common mm3d_overlay Qt6::Core Boost::boost)
if (TARGET oaknut)
    target_link_libraries(dekui_frontend PRIVATE oaknut)
endif()
