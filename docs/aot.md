# Ahead-of-time translation (AOT)

Vision Pro cannot run Dynarmic without a JIT enabler, so the interpreter
(DynCom) runs the game far too slowly. AOT translates MM3D's guest code to C++
on a desktop, compiles it into the app, and has the interpreter call that code
whenever it reaches a translated block. Signed native code needs no JIT.

The generated code is derived from the user's own game dump. **Never commit
or distribute it**; every user builds it from their dump.

## Pieces

| Path | Role |
| --- | --- |
| `tools/aot/extract_code.py` | Extracts `.code` from a decrypted `.cci`, decompresses it and applies the prepared `code.bps` exactly as Citra does. Writes `code.bin` and `layout.json`. |
| `src/dekui_aot/` | `dekui_aot` host tool: block discovery, IR to C++ emitter, `dump` for one block. Built on desktop with `ENABLE_MM3D_PRESENTATION=ON`. |
| `src/dekui_aot/check.cpp` | `dekui_aot_check`: runs every generated block against Dynarmic from random states and compares the results (see Validation). |
| `src/mm3d/code_substitutions.h` | The MM3D instruction substitutions, shared by the CPU backends and the translator so both see identical guest code. |
| `src/core/arm/aot/abi.h` | Interface between generated code and the emulator. Guest state mirrors Dynarmic's arm64 `A32JitState`. |
| `src/core/arm/aot/block_support.h` | Inline helpers used by generated code. AArch64 only; FP uses the same instructions Dynarmic emits. |
| `src/core/arm/aot/runtime.*` | Loader-time hash check, guest invalidation, block lookup and the per-core runner. |
| `src/core/arm/aot_coverage.*` | `DEKUI_AOT_COVERAGE=<dir>` records every block either CPU backend translates, plus icache invalidations and CRO loads. |

## How translation works

Blocks are translated with Dynarmic's own A32 frontend and optimization
pipeline, configured as Citra configures Dynarmic. Each IR block becomes one C++
function; direct successors are tail calls (`musttail`) while the time slice
lasts. Blocks are keyed by Dynarmic's full location descriptor (PC, Thumb, IT,
FPSCR mode), so a block only runs in the mode it was translated for.

Discovery starts at the entrypoint, coverage files and every aligned word in
the image that points into `.text`, then follows direct branches, call return
addresses and conditional fall-throughs. Restoration places some code past
`.data` (Citra applies `code.bps` after appending `.bss`); that is found through
branches.

Zero words (`.bss`, padding) are never treated as block starts: they decode
as endless `ANDEQ r0,r0,r0`, and `.bss` holds data, not code, at runtime.

Blocks are keyed by FPSCR mode, and MM3D switches to round-to-nearest
(`0x03000000`) right after startup, unlike a fresh thread's round-to-zero
(`0x03C00010`). Code reached through pointers is therefore translated for the
game's mode; the entrypoint chain keeps the thread's initial mode.

On the current USA 1.1 + Restoration image, with one play session's coverage:
413,932 blocks discovered, 413,914 emitted. The 18 left over are NEON or
interpreter-only decodes of data, which ARM11 can never execute. About 415 MB
of C++ in 64 shards; a shard takes roughly 10 to 25 s at `-O2`. Static
discovery alone already contains 73% of the blocks a play session runs; most of
the rest are interpreter resume points after a time slice ends mid-block.

At runtime, DynCom checks at every block dispatch whether the current location
has a translation. If it does, translated code runs until it reaches code with
no translation, the time slice ends, or the emulator requests a reschedule.
SVCs go through the same MM3D hooks and kernel path as the interpreter.
Translated code is enabled only for the MM3D process, and only when the loaded
image's SHA-256 equals the one the code was generated from. A guest
`InvalidateInstructionCacheRange` disables the affected blocks.

## Building

```sh
# 1. Desktop build (any configuration with ENABLE_MM3D_PRESENTATION=ON), then:
cmake --build <desktop-build> --target dekui_aot

# 2. Generate (optionally add --coverage <dir>/blocks.txt from a play session)
DEKUI_AOT=<desktop-build>/bin/Release/dekui_aot \
  tools/aot/build_aot.sh game.cci user/load/mods/0004000000125500 /tmp/mm3d-aot

# 3. Configure the AArch64 build (visionOS, iOS or Apple silicon macOS) with
#    -DDEKUI_AOT_GENERATED_DIR=/tmp/mm3d-aot/generated
```

On desktop, AOT runs only when the CPU JIT is off (Emulation > Configure >
Debug: untick "Enable CPU JIT"), because it hooks into the interpreter. That is the
configuration that shows what a Vision Pro without JIT will get.

## Validation

`dekui_aot_check <work>` links the generated code together with Dynarmic. Each
block runs from the same pseudo-random register, flag, FP and memory state in
both engines; registers, CPSR, FPSCR, every touched memory byte, SVC/exception
events and consumed cycles must match. It needs no game session. Runs that
touch code outside the image or junk coprocessor encodings are skipped.

Result on the current image: all 413,914 blocks, three random states each
(1,241,742 runs), 0 mismatches, 1,929 runs skipped. The check found and fixed
one real divergence: clang vectorised `QADD16`/`QSUB16` into NEON saturating
instructions that set FPSR.QC, which leaked into the guest FPSCR.

## Coverage

Set `DEKUI_AOT_COVERAGE=/some/dir` before launching. `blocks.txt` feeds
`dekui_aot emit --coverage`, and `dekui_aot analyze <work> --coverage ...`
reports how much of what really ran static discovery already found.
`events.txt` shows whether the game invalidates its icache or loads CROs,
which would need fallback handling. A first play session recorded neither.

For repeatable runs, Citra's `--movie-record <file>` / `--movie-play <file>`
record and replay input from boot. Restore the `user` folder snapshot taken
before recording first. Replays may drift across differently generated builds,
because translated code and the interpreter count CPU time differently.

## Known limits

- AArch64 hosts only (the FP helpers are inline AArch64 assembly).
- Hooked into DynCom only; with Dynarmic enabled, translated code is not used.
- An LDREX/STREX pair split between translated code and Dynarmic would
  livelock; with DynCom both sides share DynCom's monitor.
- Duplicated code: a block entered mid-way is translated again from that
  point. Function-level translation would shrink the output a lot.
