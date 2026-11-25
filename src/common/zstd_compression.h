// Copyright 2020-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <span>
#include <streambuf>
#include <unordered_map>
#include <vector>

#include <boost/serialization/array.hpp>
#include <boost/serialization/unordered_map.hpp>
#include "common/archives.h"
#include "common/common_types.h"

namespace Common::Compression {

/// A stream buffer that compresses everything written to it into one zstd
/// frame, so large data (save states) never has to exist uncompressed in full.
/// The frame has no content size: read it with ZSTDDecompressingBuffer.
class ZSTDCompressingBuffer final : public std::streambuf {
public:
    ZSTDCompressingBuffer();
    ~ZSTDCompressingBuffer() override;
    /// Ends the frame and returns the compressed data.
    std::vector<u8> Finish();

protected:
    int_type overflow(int_type ch) override;

private:
    void Compress(bool end);
    void* context;
    std::vector<char> input;
    std::vector<u8> output;
};

/// Reads zstd-compressed data (frames with or without a content size) through
/// a small window, without holding the decompressed data in full.
class ZSTDDecompressingBuffer final : public std::streambuf {
public:
    explicit ZSTDDecompressingBuffer(std::span<const u8> compressed);
    ~ZSTDDecompressingBuffer() override;

protected:
    int_type underflow() override;

private:
    void* context;
    std::span<const u8> source;
    std::size_t position{};
    std::vector<char> window;
};

/**
 * Compresses a source memory region with Zstandard and returns the compressed data in a vector.
 *
 * @param source the uncompressed source memory region.
 * @param compression_level the used compression level. Should be between 1 and 22.
 *
 * @return the compressed data.
 */
[[nodiscard]] std::vector<u8> CompressDataZSTD(std::span<const u8> source, s32 compression_level);

/**
 * Compresses a source memory region with Zstandard with the default compression level and returns
 * the compressed data in a vector.
 *
 * @param source the uncompressed source memory region.
 *
 * @return the compressed data.
 */
[[nodiscard]] std::vector<u8> CompressDataZSTDDefault(std::span<const u8> source);

/**
 * Gets the decompressed size of the specified Zstandard compressed memory region.
 *
 * @param compressed the compressed source memory region.
 *
 * @return the size of the decompressed data.
 */
[[nodiscard]] std::size_t GetDecompressedSize(std::span<const u8> compressed);

/**
 * Decompresses a source memory region with Zstandard and returns the uncompressed data in a vector.
 *
 * @param compressed the compressed source memory region.
 *
 * @return the decompressed data.
 */
[[nodiscard]] std::vector<u8> DecompressDataZSTD(std::span<const u8> compressed);

} // namespace Common::Compression

namespace FileUtil {

using ProgressCallback = void(std::size_t, std::size_t);

bool CompressZ3DSFile(const std::string& src_file, const std::string& dst_file,
                      const std::array<u8, 4>& underlying_magic, size_t frame_size,
                      std::function<ProgressCallback>&& update_callback = nullptr,
                      std::unordered_map<std::string, std::vector<u8>> metadata = {});

bool DeCompressZ3DSFile(const std::string& src_file, const std::string& dst_file,
                        std::function<ProgressCallback>&& update_callback = nullptr);

} // namespace FileUtil