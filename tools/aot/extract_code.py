#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract MM3D's executable image from a user's decrypted game-card dump.

Reads the first NCCH partition of a .cci/.3ds, decompresses ExeFS .code,
optionally applies the prepared code.bps / exheader.bin from a DekUI user
folder, and writes:

  code.bin    the flat text+rodata+data image as loaded at its base address
  layout.json segment addresses and sizes from the (patched) exheader

The output is derived from the user's own dump. Keep it out of the repository.
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib

MEDIA_UNIT = 0x200


def u32(data, at):
    return struct.unpack_from("<I", data, at)[0]


def lzss_decompress(compressed):
    """Backward LZSS used by 3DS ExeFS .code (mirrors ncch_container.cpp)."""
    footer = len(compressed) - 8
    top_and_bottom = u32(compressed, footer)
    out_size = len(compressed) + u32(compressed, footer + 4)
    out = bytearray(out_size)
    out[: len(compressed)] = compressed
    index = len(compressed) - ((top_and_bottom >> 24) & 0xFF)
    stop = len(compressed) - (top_and_bottom & 0xFFFFFF)
    pos = out_size
    while index > stop:
        index -= 1
        control = compressed[index]
        for _ in range(8):
            if index <= stop or index <= 0 or pos <= 0:
                break
            if control & 0x80:
                if index < 2:
                    raise ValueError("LZSS: input underrun")
                index -= 2
                segment = compressed[index] | (compressed[index + 1] << 8)
                size = ((segment >> 12) & 15) + 3
                offset = (segment & 0x0FFF) + 2
                if pos < size:
                    raise ValueError("LZSS: output underrun")
                for _ in range(size):
                    if pos + offset >= out_size:
                        raise ValueError("LZSS: reference out of range")
                    pos -= 1
                    out[pos] = out[pos + offset + 1]
            else:
                if pos < 1:
                    raise ValueError("LZSS: output underrun")
                pos -= 1
                index -= 1
                out[pos] = compressed[index]
            control = (control << 1) & 0xFF
    return bytes(out)


def apply_bps(source, patch):
    if patch[:4] != b"BPS1":
        raise ValueError("not a BPS patch")
    at = 4

    def number():
        nonlocal at
        data, shift = 0, 1
        while True:
            byte = patch[at]
            at += 1
            # Citra decodes into u32 and some encoders rely on the wraparound.
            data = (data + (byte & 0x7F) * shift) & 0xFFFFFFFF
            if byte & 0x80:
                return data
            shift = (shift << 7) & 0xFFFFFFFF
            data = (data + shift) & 0xFFFFFFFF

    source_size = number()
    target_size = number()
    metadata_size = number()  # evaluated before `at` is read below
    at += metadata_size
    if source_size != len(source):
        raise ValueError(f"BPS source size {source_size:#x} != {len(source):#x}")
    target = bytearray(target_size)
    # Pad so slice assignments never change the target's length.
    source = bytes(source) + bytes(max(0, target_size - len(source)))
    out = source_rel = target_rel = 0
    end = len(patch) - 12
    while at < end:
        data = number()
        command, length = data & 3, (data >> 2) + 1
        if command == 0:  # SourceRead
            target[out : out + length] = source[out : out + length]
            out += length
        elif command == 1:  # TargetRead
            target[out : out + length] = patch[at : at + length]
            at += length
            out += length
        else:
            delta = number()
            delta = -(delta >> 1) if delta & 1 else delta >> 1
            if command == 2:  # SourceCopy
                source_rel += delta
                target[out : out + length] = source[source_rel : source_rel + length]
                source_rel += length
                out += length
            else:  # TargetCopy, may overlap
                target_rel += delta
                for _ in range(length):
                    target[out] = target[target_rel]
                    out += 1
                    target_rel += 1
    if len(target) != target_size or zlib.crc32(target) != u32(patch, len(patch) - 8):
        raise ValueError("BPS target checksum mismatch")
    return bytes(target)


def read_ncch(cci):
    with open(cci, "rb") as f:
        header = f.read(0x200)
        if header[0x100:0x104] == b"NCSD":
            offset = u32(header, 0x120) * MEDIA_UNIT
        elif header[0x100:0x104] == b"NCCH":
            offset = 0
        else:
            raise ValueError("not an NCSD or NCCH image")
        f.seek(offset)
        ncch = f.read(0x200)
        if ncch[0x100:0x104] != b"NCCH":
            raise ValueError("partition 0 is not NCCH")
        if not ncch[0x18F] & 0x4:
            raise ValueError("NCCH is encrypted; a decrypted dump is required")
        exheader = f.read(0x800)
        exefs_offset = offset + u32(ncch, 0x1A0) * MEDIA_UNIT
        f.seek(exefs_offset)
        exefs_header = f.read(0x200)
        for entry in range(10):
            name = exefs_header[entry * 16 : entry * 16 + 8].rstrip(b"\0")
            if name == b".code":
                file_offset = u32(exefs_header, entry * 16 + 8)
                size = u32(exefs_header, entry * 16 + 12)
                f.seek(exefs_offset + 0x200 + file_offset)
                return exheader, f.read(size)
    raise ValueError("ExeFS has no .code")


def layout(exheader, image_size):
    """Segments as Citra's NCCH loader maps them (see loader/ncch.cpp).

    Citra appends page-aligned .bss before applying code.bps, so a patch may
    place new code past .data; that lands inside the data segment."""
    page = 0x1000
    text, ro, data = (struct.unpack_from("<III", exheader, at) for at in (0x10, 0x20, 0x30))
    bss_pages = (u32(exheader, 0x3C) + page - 1) & ~(page - 1)
    segments, offset = {}, 0
    for name, (address, pages, _), extra in (("text", text, 0), ("rodata", ro, 0),
                                             ("data", data, bss_pages)):
        size = pages * page + extra
        segments[name] = {"address": address, "offset": offset, "size": size}
        offset += size
    return {
        "compressed": bool(exheader[0xD] & 1),
        "entrypoint": text[0],
        "segments": segments,
        "bss_size": u32(exheader, 0x3C),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("cci", type=pathlib.Path)
    parser.add_argument("out", type=pathlib.Path, help="output directory")
    parser.add_argument("--mods", type=pathlib.Path,
                        help="user/load/mods/0004000000125500 with code.bps and exheader.bin")
    args = parser.parse_args()

    exheader, code = read_ncch(args.cci)
    if exheader[0xD] & 1:
        code = lzss_decompress(code)
    original_sha = hashlib.sha256(code).hexdigest()
    if args.mods:
        if (args.mods / "code.bps").exists():
            code = apply_bps(code, (args.mods / "code.bps").read_bytes())
        if (args.mods / "exheader.bin").exists():
            exheader = (args.mods / "exheader.bin").read_bytes()
    info = layout(exheader, len(code))
    info["original_sha256"] = original_sha
    info["code_sha256"] = hashlib.sha256(code).hexdigest()
    info["code_size"] = len(code)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "code.bin").write_bytes(code)
    (args.out / "layout.json").write_text(json.dumps(info, indent=2) + "\n")
    json.dump(info, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
