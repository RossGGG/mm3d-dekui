# Offline ahead-of-time translator for MM3D's guest code. A host tool: it reads
# a user's extracted code.bin and never ships in an application bundle.
add_executable(dekui_aot
    discover.cpp
    discover.h
    emit_cpp.cpp
    emit_cpp.h
    frontend.cpp
    frontend.h
    image.cpp
    image.h
    main.cpp
    ../core/arm/dynarmic/arm_tick_counts.cpp
    ../core/arm/dynarmic/arm_tick_counts.h
)
target_include_directories(dekui_aot PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(dekui_aot PRIVATE dynarmic merry::mcl Boost::boost fmt::fmt json-headers)
