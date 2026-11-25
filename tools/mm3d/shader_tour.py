#!/usr/bin/env python3
"""Build Dekui's shader cache for Apple Vision Pro and Apple TV on a Mac.

The Mac app plays a shader tour (src/mm3d/shader_tour.h): Link warps to every
scene entrance, then Deku, Goron, Zora and Fierce Deity Link visit the main
areas while walking, attacking, rolling, charging and turning. Every shader and
pipeline drawn is recorded in the disk shader cache. The headset and the Apple
TV merge that cache at boot and build those pipelines before the game starts,
so first visits to an area do not pop in.

The tour runs on copies: the app, a link to your game dump and a copy of your
user folder without save states or shader caches go into a work folder. Your
own saves, save states and settings are never opened for writing.

The visionOS cache is copied to iCloud Drive ("Dekui/Shader Cache"), where the
headset app picks it up on its next launch. For the Apple TV, copy the tvOS
cache into the app's user/shaders/import/ folder (the command is printed).

usage: shader_tour.py [--app APP] [--root GAME_FOLDER] [--targets visionos,tvos]
                      [--seconds 12] [--form-seconds 10] [--work DIR] [--no-icloud]
"""
from __future__ import annotations

import argparse
import json
import os
import plistlib
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

TITLE = "0004000000125500"
KINDS = ("vs", "fs", "gs", "pl")
ENTRY_TYPES = {1: "VS config", 2: "VS program", 3: "VS SPIR-V", 4: "FS config",
               5: "FS SPIR-V", 6: "GS config", 7: "GS SPIR-V", 8: "pipeline"}
# Hardware vertex shaders. 60 fps interpolation moves vertices on the CPU when
# the JIT is available (Apple Vision Pro with JIT), which turns hardware vertex
# shaders off; without the JIT (Apple TV, and Vision Pro running Dekui AOT) it
# runs on the GPU with hardware vertex shaders on.
TARGETS = {"visionos": False, "tvos": True}
ICLOUD = Path.home() / "Library/Mobile Documents/iCloud~org~azahar-emu~azahar/Documents/Shader Cache"
SKIP_USER = ("states", "shaders", "log", "screenshots", "aot-coverage", "dump")
STALL_SECONDS = 120


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def default_app() -> Path:
    for name in ("Dekui.app", "Dekui AOT.app"):
        if (Path("/Applications") / name).is_dir():
            return Path("/Applications") / name
    raise SystemExit("Dekui app not found in /Applications; pass --app")


def default_root(app: Path) -> Path:
    """The game folder the app uses: beside the app if portable, else Application Support."""
    beside = app.parent
    if any(beside.glob("*.cci")) or (beside / "user").is_dir():
        return beside
    with open(app / "Contents/Info.plist", "rb") as f:
        name = plistlib.load(f).get("CFBundleName", "Dekui")
    return Path.home() / "Library/Application Support" / name


def find_rom(root: Path) -> Path:
    for pattern in ("*.cci", "*.3ds", "*.cxi"):
        for rom in sorted(root.glob(pattern)):
            return rom
    raise SystemExit(f"No game dump (.cci/.3ds/.cxi) in {root}")


def set_config(ini: Path, values: dict[str, str]) -> None:
    """Set Qt config keys; '\\default=false' too, or Qt keeps the default."""
    text = ini.read_text(encoding="utf-8")
    for key, value in values.items():
        text, n = re.subn(rf"^{re.escape(key)}=.*$", f"{key}={value}", text, flags=re.M)
        if not n:
            raise SystemExit(f"{key} missing from {ini}")
        text = re.sub(rf"^{re.escape(key)}\\default=.*$", f"{key}\\\\default=false", text, flags=re.M)
    ini.write_text(text, encoding="utf-8")


def prepare(app: Path, root: Path, work: Path) -> Path:
    """Work folder: app copy (the app runs portable from it), ROM link, user copy."""
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    copy = work / app.name
    # APFS clone: fast and takes no space.
    if subprocess.run(["cp", "-cR", str(app), str(copy)]).returncode:
        shutil.copytree(app, copy, symlinks=True)
    rom = find_rom(root)
    (work / rom.name).symlink_to(rom)
    user = root / "user"
    if not (user / "config/qt-config.ini").is_file():
        raise SystemExit(f"{user} has no config; start the game once with this app first")
    shutil.copytree(user, work / "user", symlinks=True,
                    ignore=lambda folder, names: [n for n in names
                                                  if Path(folder) == user and n in SKIP_USER])
    return copy


def processes(app: Path) -> list[int]:
    out = subprocess.run(["pgrep", "-f", str(app / "Contents/MacOS")], capture_output=True, text=True)
    return [int(p) for p in out.stdout.split()]


def stop(app: Path) -> None:
    # The emulator ignores SIGTERM while running; the cache appends are flushed
    # entry by entry, so a hard stop loses nothing.
    for pid in processes(app):
        os.kill(pid, signal.SIGKILL)
    for _ in range(50):
        if not processes(app):
            return
        time.sleep(0.1)


def run_tour(app: Path, work: Path, seconds: float, form_seconds: float) -> tuple[int, int, int]:
    """Runs the tour to the end, restarting after a crash or a stall. Returns
    (steps, scenes visited, scenes failed)."""
    log_path = work / "user/log/azahar_log.txt"
    start, total, visited, failed = 0, 0, 0, 0
    step = re.compile(r"Shader tour: \[(\d+)/(\d+)\] (.*?) entrance (0x[0-9a-f]+) (->.*|did not load)")
    while True:
        stop(app)
        if log_path.exists():
            log_path.unlink()
        env = {"DEKUI_SHADER_TOUR": "1", "DEKUI_SHADER_TOUR_START": str(start),
               "DEKUI_SHADER_TOUR_SECONDS": str(seconds),
               "DEKUI_SHADER_TOUR_FORM_SECONDS": str(form_seconds)}
        command = ["open", "-n", "-g"]
        for key, value in env.items():
            command += ["--env", f"{key}={value}"]
        subprocess.run(command + [str(app)], check=True)
        log(f"Game started at step {start + 1}")
        offset, last_progress, finished, stuck = 0, time.monotonic(), False, False
        while True:
            time.sleep(2)
            if log_path.exists():
                with open(log_path, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(offset)
                    chunk = f.read()
                    offset = f.tell()
                for line in chunk.splitlines():
                    if m := step.search(line):
                        index, total = int(m.group(1)), int(m.group(2))
                        start = index
                        last_progress = time.monotonic()
                        if m.group(5).startswith("->"):
                            visited += 1
                        else:
                            failed += 1
                        log(f"  {index}/{total} {m.group(3)} {m.group(4)} {m.group(5)}")
                    elif "Shader tour: done" in line:
                        finished = True
                    elif "Shader tour: stuck" in line:
                        stuck = True
            if stuck:
                log("  stuck outside gameplay; restarting at the next step")
                start += 1
                failed += 1
                break
            if finished:
                time.sleep(10)  # let queued shader compiles finish their cache appends
                stop(app)
                return total, visited, failed
            if not processes(app):
                log("  the game closed; restarting at the next step")
                start += 1
                failed += 1
                break
            if time.monotonic() - last_progress > STALL_SECONDS:
                log("  no progress; restarting at the next step")
                start += 1
                failed += 1
                break


def count(folder: Path) -> dict[str, int]:
    """Unique entries per type in the four cache files."""
    found: dict[str, set[int]] = {}
    for kind in KINDS:
        path = folder / f"{TITLE}_{kind}.vkch"
        data = path.read_bytes() if path.is_file() else b""
        pos = 0
        while pos + 32 <= len(data):
            version, _, kind_id, size, entry_id = struct.unpack_from("<BBHIQ", data, pos)
            if version != 0x42 or size < 32 or pos + size > len(data):
                break
            if kind_id in ENTRY_TYPES:
                found.setdefault(ENTRY_TYPES[kind_id], set()).add(entry_id)
            pos += size
    return {name: len(ids) for name, ids in found.items()}


def entries(data: bytes) -> list[tuple[int, int, bytes]]:
    """(type, id, entry bytes without footer) of a cache file, up to a torn write."""
    out, pos = [], 0
    while pos + 32 <= len(data):
        version, _, kind_id, size, entry_id = struct.unpack_from("<BBHIQ", data, pos)
        if version != 0x42 or size < 32 or pos + size > len(data):
            break
        out.append((kind_id, entry_id, data[pos:pos + size - 16]))
        pos += size
    return out


def merge(tour: bytes, extra: bytes) -> tuple[bytes, int]:
    """Adds the entries of `extra` (same cache type) missing from `tour`, as the
    device does at import (ShaderDiskCache::MergeImported)."""
    own, more = entries(tour), entries(extra)
    # FILE_INFO: same magic, version, config layout hash and file type.
    if not own or not more or own[0][2][16:32] != more[0][2][16:32]:
        return tour, 0
    seen = {(t, i) for t, i, _ in own}
    added = [e for e in more[1:] if (e[0], e[1]) not in seen and not seen.add((e[0], e[1]))]
    if not added:
        return tour, 0
    info = bytearray(own[0][2])
    info[16 + 20:16 + 84] = bytes(64)  # source hash: the device regenerates SPIR-V
    out = bytearray()
    for n, body in enumerate([bytes(info)] + [b for _, _, b in own[1:]] + [b for _, _, b in added]):
        out += body + struct.pack("<IIQ", (n << 8) | 0x24, len(body) + 16, 0)
    return bytes(out), len(added)


def export(work: Path, out: Path, play: Path | None, play_kinds=KINDS) -> dict[str, int]:
    """Copies the tour's cache, plus what the game folder's own cache (your
    play on this Mac) has that the tour did not draw."""
    source = work / "user/shaders/vulkan/transferable"
    out.mkdir(parents=True, exist_ok=True)
    added = 0
    for kind in KINDS:
        name = f"{TITLE}_{kind}.vkch"
        if not (source / name).is_file():
            continue
        data = (source / name).read_bytes()
        if play and kind in play_kinds and (play / name).is_file():
            data, n = merge(data, (play / name).read_bytes())
            added += n
        (out / name).write_bytes(data)
    if added:
        log(f"  added {added} entries from {play}")
    return count(out)


def main() -> int:
    if sys.platform != "darwin":
        print("The shader tour runs on macOS.")
        return 1
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--app", type=Path, help="Dekui app with shader tour support")
    parser.add_argument("--root", type=Path, help="game folder holding the dump and user/")
    parser.add_argument("--targets", default="visionos,tvos", help="visionos, tvos or both")
    parser.add_argument("--seconds", type=float, default=12, help="seconds in each scene")
    parser.add_argument("--form-seconds", type=float, default=10, help="seconds per scene and form")
    parser.add_argument("--work", type=Path,
                        default=Path.home() / "Library/Application Support/Dekui Shader Tour")
    parser.add_argument("--out", type=Path, help="output folder (default: <work>/Shader Cache)")
    parser.add_argument("--no-icloud", action="store_true", help="do not copy to iCloud Drive")
    parser.add_argument("--icloud-target", default="visionos", choices=sorted(TARGETS),
                        help="cache sent to the headset: visionos (JIT) or tvos (Dekui AOT, no JIT)")
    parser.add_argument("--no-play-cache", action="store_true",
                        help="do not add the game folder's own shader cache to the export")
    args = parser.parse_args()

    app = (args.app or default_app()).resolve()
    root = (args.root or default_root(app)).resolve()
    targets = [t.strip() for t in args.targets.split(",") if t.strip()]
    if any(t not in TARGETS for t in targets):
        parser.error(f"targets: {', '.join(TARGETS)}")
    work = args.work.resolve()
    if work == root or root in work.parents or work in root.parents:
        parser.error("the work folder must be separate from the game folder")
    out_root = (args.out or work.parent / "Dekui Shader Cache").resolve()

    for target in targets:
        log(f"Shader tour for {target} (game folder {root})")
        game = prepare(app, root, work / target)
        set_config(work / target / "user/config/qt-config.ini", {
            "use_disk_shader_cache": "true",
            "async_shader_compilation": "true",
            "use_hw_shader": "true" if TARGETS[target] else "false",
            "use_cpu_jit": "true",
            # As on the headset and the Apple TV: stereo presentation and HD
            # textures add draws of their own.
            "render_3d": "2",
            "custom_textures": "true",
            # The game window opens in the background instead of covering the screen.
            "fullscreen": "false",
        })
        presentation = work / target / "user/config/mm3d-local/presentation.json"
        if presentation.is_file():
            settings = json.loads(presentation.read_text(encoding="utf-8"))
            settings["interpolation_cpu_vertices"] = not TARGETS[target]
            presentation.write_text(json.dumps(settings, indent=2), encoding="utf-8")
        started = time.monotonic()
        total, visited, failed = run_tour(game, work / target, args.seconds, args.form_seconds)
        play = None if args.no_play_cache else root / "user/shaders/vulkan/transferable"
        # Without hardware vertex shaders the headset has no use for VS configs;
        # pipelines that need them are skipped at load.
        kinds = KINDS if TARGETS[target] else ("fs", "gs", "pl")
        counts = export(work / target, out_root / target, play, kinds)
        log(f"{target}: {visited} of {total} steps visited, {failed} failed, "
            f"{(time.monotonic() - started) / 60:.0f} min")
        log("  cache entries: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
        log(f"  saved in {out_root / target}")
        if target == args.icloud_target and not args.no_icloud:
            if ICLOUD.parent.is_dir():
                ICLOUD.mkdir(exist_ok=True)
                for f in (out_root / target).glob("*.vkch"):
                    shutil.copy2(f, ICLOUD / f.name)
                log(f"  copied to iCloud Drive ({ICLOUD}); the headset merges it on its next launch")
            else:
                log("  iCloud Drive folder for Dekui not found; copy the files into the headset "
                    "app's Documents/user/shaders/import/ instead")
        if target == "tvos":
            log("  Apple TV: copy the files into the app's user/shaders/import/, for example:")
            log("    xcrun devicectl device copy to --device <id> --domain-type appDataContainer "
                "--domain-identifier org.azahar-emu.azahar.aot "
                f"--source '{out_root / target}/.' --destination Library/Caches/Dekui/user/shaders/import")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
