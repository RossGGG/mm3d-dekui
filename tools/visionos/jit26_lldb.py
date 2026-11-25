"""LLDB handler for the JIT26 breakpoint protocol (Xcode launches on Vision Pro).

Dekui asks its debugger for executable memory by executing `brk #0xf00d`
with a command in x16 (see externals/oaknut/include/oaknut/apple_jit26.hpp).
StikDebug and Apparition answer it on-device. This script lets Xcode's LLDB
answer it instead, so Build & Run from Xcode gets a working JIT.

Install once by adding this line to ~/.lldbinit-Xcode:

    command script import /path/to/repo/tools/visionos/jit26_lldb.py

Every 16 KiB page of the arena must be written by the debugger. Over Xcode's
Wi-Fi connection this is slow, so the generated Xcode scheme sets
DEKUI_JIT26_ARENA_MB (CMake option CITRA_VISIONOS_JIT26_ARENA_MB) to keep the
arena smaller than the on-device default.
"""

import sys
import time

import lldb

BRK_F00D = 0xD43E01A0  # brk #0xf00d
PAGE_SIZE = 16 * 1024
CHUNK_SIZE = 1024 * 1024
CMD_DETACH = 0
CMD_PREPARE_REGION = 1


def log(message):
    # Stop-hook stream output only appears once the hook returns; print so
    # progress shows in Xcode's console while pages are being prepared.
    print("[JIT26] " + message)
    sys.stdout.flush()


class Jit26StopHook:
    def __init__(self, target, extra_args, internal_dict):
        self.target = target

    def handle_stop(self, exe_ctx, stream):
        process = exe_ctx.GetProcess()
        thread = exe_ctx.GetThread()
        if not process.IsValid() or not thread.IsValid():
            return True
        frame = thread.GetFrameAtIndex(0)
        pc = frame.GetPC()

        error = lldb.SBError()
        instruction = process.ReadUnsignedFromMemory(pc, 4, error)
        if error.Fail() or instruction != BRK_F00D:
            return True  # Not ours; stop normally.

        command = frame.FindRegister("x16").GetValueAsUnsigned()
        if command == CMD_PREPARE_REGION:
            if not self._prepare_region(process, frame, stream):
                return True  # Leave the process stopped so the failure is visible.
        elif command != CMD_DETACH:
            log("unknown command %d" % command)
            return True
        # Detach is a no-op: Xcode stays attached for the whole session.

        frame.SetPC(pc + 4)
        return False  # Resume without reporting the stop.

    def _prepare_region(self, process, frame, stream):
        address = frame.FindRegister("x0").GetValueAsUnsigned()
        length = frame.FindRegister("x1").GetValueAsUnsigned()
        length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)
        error = lldb.SBError()

        fresh = address == 0
        if fresh:
            # debugserver allocates out of process, which is what lets TXM
            # accept the mapping as executable.
            address = process.AllocateMemory(
                length, lldb.ePermissionsReadable | lldb.ePermissionsExecutable, error)
            if error.Fail() or address == lldb.LLDB_INVALID_ADDRESS:
                log("AllocateMemory(%d) failed: %s" % (length, error))
                return False

        log("preparing %d MiB at 0x%x..." % (length >> 20, address))
        start = time.time()
        if fresh:
            ok = self._zero_fill(process, address, length, start)
        else:
            ok = self._touch_pages(process, address, length, start)
        if not ok:
            return False

        frame.FindRegister("x0").SetValueFromCString(hex(address), error)
        if error.Fail():
            log("could not set x0: %s" % error)
            return False
        log("region ready after %.0fs" % (time.time() - start))
        return True

    def _zero_fill(self, process, address, length, start):
        # A fresh allocation is all zeros, so rewriting it with zeros is
        # harmless. Large writes cover many pages per round trip, which is far
        # faster over Wi-Fi than one packet per page.
        zeros = b"\x00" * CHUNK_SIZE
        error = lldb.SBError()
        for offset in range(0, length, CHUNK_SIZE):
            chunk = min(CHUNK_SIZE, length - offset)
            written = process.WriteMemory(address + offset, zeros[:chunk], error)
            if error.Fail() or written != chunk:
                log("bulk write at 0x%x failed (%s); falling back to per-page writes" %
                    (address + offset, error))
                return self._touch_pages(process, address + offset, length - offset, start)
            done = offset + chunk
            if done % (16 * CHUNK_SIZE) == 0 or done == length:
                self._progress(done, length, start)
        return True

    def _touch_pages(self, process, address, length, start):
        error = lldb.SBError()
        for offset in range(0, length, PAGE_SIZE):
            data = process.ReadMemory(address + offset, 1, error)
            if error.Fail():
                log("read at 0x%x failed: %s" % (address + offset, error))
                return False
            process.WriteMemory(address + offset, data, error)
            if error.Fail():
                log("write at 0x%x failed: %s" % (address + offset, error))
                return False
            if offset and offset % (256 * PAGE_SIZE) == 0:
                self._progress(offset, length, start)
        return True

    @staticmethod
    def _progress(done, length, start):
        elapsed = time.time() - start
        log("  %d/%d MiB, %.0fs elapsed, ~%.0fs left" %
            (done >> 20, length >> 20, elapsed, elapsed / done * (length - done)))


def __lldb_init_module(debugger, internal_dict):
    # Added before a target exists, the hook lands on LLDB's dummy target and
    # is inherited by the app target Xcode creates.
    debugger.HandleCommand("target stop-hook add -P %s.Jit26StopHook" % __name__)
