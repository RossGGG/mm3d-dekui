// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <unistd.h>
#include <TargetConditionals.h>

namespace AppleUtils {

bool IsRunningFromTerminal() {
#if TARGET_OS_OSX
    return (getppid() != 1);
#else
    return false;
#endif
}

} // namespace AppleUtils
