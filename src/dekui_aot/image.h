// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace DekuiAot {

/// The MM3D executable as Citra maps it: code.bin and layout.json written by
/// tools/aot/extract_code.py.
class Image {
public:
    struct Segment {
        std::string name;
        std::uint32_t address = 0;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };

    explicit Image(const std::filesystem::path& work_directory);

    /// Original guest word (before MM3D substitutions). Bytes past code.bin
    /// inside the data segment are .bss and read as zero.
    std::optional<std::uint32_t> Read32(std::uint32_t vaddr) const;

    /// Guest word as the MM3D backends fetch it for execution.
    std::optional<std::uint32_t> ReadCode(std::uint32_t vaddr) const;

    bool Contains(std::uint32_t vaddr) const;
    const Segment* SegmentAt(std::uint32_t vaddr) const;

    const std::vector<Segment>& Segments() const { return segments; }
    std::uint32_t Entrypoint() const { return entrypoint; }
    const std::string& Sha256() const { return sha256; }
    std::size_t CodeSize() const { return code.size(); }

private:
    std::vector<std::uint8_t> code;
    std::vector<Segment> segments;
    std::uint32_t entrypoint = 0;
    std::string sha256;
};

} // namespace DekuiAot
