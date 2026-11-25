// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <json.hpp>
#include "dekui_aot/image.h"
#include "mm3d/code_substitutions.h"

namespace DekuiAot {

Image::Image(const std::filesystem::path& work_directory) {
    std::ifstream layout_file(work_directory / "layout.json");
    if (!layout_file)
        throw std::runtime_error("missing layout.json in " + work_directory.string());
    const auto layout = nlohmann::json::parse(layout_file);

    std::ifstream code_file(work_directory / "code.bin", std::ios::binary);
    if (!code_file)
        throw std::runtime_error("missing code.bin in " + work_directory.string());
    code.assign(std::istreambuf_iterator<char>(code_file), {});

    entrypoint = layout.at("entrypoint").get<std::uint32_t>();
    sha256 = layout.at("code_sha256").get<std::string>();
    for (const char* name : {"text", "rodata", "data"}) {
        const auto& segment = layout.at("segments").at(name);
        segments.push_back({name, segment.at("address").get<std::uint32_t>(),
                            segment.at("offset").get<std::uint32_t>(),
                            segment.at("size").get<std::uint32_t>()});
    }
}

const Image::Segment* Image::SegmentAt(std::uint32_t vaddr) const {
    for (const auto& segment : segments)
        if (vaddr >= segment.address && vaddr - segment.address < segment.size)
            return &segment;
    return nullptr;
}

bool Image::Contains(std::uint32_t vaddr) const {
    return SegmentAt(vaddr) != nullptr;
}

std::optional<std::uint32_t> Image::Read32(std::uint32_t vaddr) const {
    const Segment* segment = SegmentAt(vaddr);
    if (!segment || vaddr - segment->address + 4 > segment->size)
        return std::nullopt;
    const std::size_t at = segment->offset + (vaddr - segment->address);
    std::uint32_t word = 0;
    if (at + 4 <= code.size())
        std::memcpy(&word, code.data() + at, 4);
    return word;
}

std::optional<std::uint32_t> Image::ReadCode(std::uint32_t vaddr) const {
    const auto word = Read32(vaddr);
    if (!word)
        return word;
    struct Reader {
        const Image& image;
        std::optional<std::uint32_t> Read(std::uint32_t at) { return image.Read32(at); }
    } reader{*this};
    return MM3D::SubstituteInstruction(reader, vaddr, *word);
}

} // namespace DekuiAot
