// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <thread>

#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/static_lru_cache.h"
#include "common/zstd_compression.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_disk_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/glsl_shader_gen.h"
#include "video_core/shader/generator/shader_gen.h"
#include "video_core/shader/generator/spv_fs_shader_gen.h"
#ifdef ENABLE_MM3D_PRESENTATION
#include "mm3d/atlas_render.h"
#endif

#define MALFORMED_DISK_CACHE                                                                       \
    do {                                                                                           \
        LOG_ERROR(Render_Vulkan, "Malformed disk shader cache");                                   \
        cleanup_on_error();                                                                        \
        return false;                                                                              \
    } while (0)

// Enable to debug when new cache objects are created.
// #define ENABLE_LOG_NEW_OBJECT

#ifdef ENABLE_LOG_NEW_OBJECT
#define LOG_NEW_OBJECT LOG_DEBUG
#else
#define LOG_NEW_OBJECT(...) ((void)0)
#endif

namespace Vulkan {

using namespace Pica::Shader::Generator;
using namespace Pica::Shader;

static VideoCore::DiskResourceLoadCallback MakeThrottledCallback(
    VideoCore::DiskResourceLoadCallback original) {

    if (!original)
        return nullptr;

    auto last_call = std::chrono::steady_clock::now() - std::chrono::milliseconds(10);

    return [original, last_call](VideoCore::LoadCallbackStage stage, std::size_t current,
                                 std::size_t total, const std::string& name) mutable {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_call >= std::chrono::milliseconds(10)) {
            last_call = now;
            original(stage, current, total, name);
        }
    };
}

void ShaderDiskCache::Init(const std::atomic_bool& stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback) {

    if (!Settings::values.use_disk_shader_cache)
        return;

    auto new_callback = MakeThrottledCallback(callback);

    ImportCaches();

    if (!stop_loading && !InitVSCache(stop_loading, new_callback)) {
        RecreateCache(vs_cache, CacheFileType::VS_CACHE);
    }
    if (!stop_loading && !InitFSCache(stop_loading, new_callback)) {
        RecreateCache(fs_cache, CacheFileType::FS_CACHE);
    }
    if (!stop_loading && !InitGSCache(stop_loading, new_callback)) {
        RecreateCache(gs_cache, CacheFileType::GS_CACHE);
    }
    if (!stop_loading && !InitPLCache(stop_loading, new_callback)) {
        RecreateCache(pl_cache, CacheFileType::PL_CACHE);
    }
}

std::optional<std::pair<u64, Shader* const>> ShaderDiskCache::UseProgrammableVertexShader(
    const Pica::RegsInternal& regs, Pica::ShaderSetup& setup, const VertexLayout& layout) {

    PicaVSConfig config{regs, setup};

    // Transfer vertex attributes to the VS config
    config.state.used_input_vertex_attributes = layout.attribute_count;
    for (u32 i = 0; i < layout.attribute_count; i++) {
        auto& dst = config.state.input_vertex_attributes[i];
        const auto& src = layout.attributes[i];

        dst.location = src.location;
        dst.type = static_cast<u8>(src.type.Value());
        dst.size = src.size;
    }

    const auto config_hash = config.Hash();

    const auto [iter_config, new_config] = programmable_vertex_map.try_emplace(config_hash);
    if (new_config) {

        LOG_NEW_OBJECT(Render_Vulkan, "New VS config {:016X}", config_hash);

        ExtraVSConfig extra_config = parent.CalcExtraConfig(config);

        auto program =
            Common::HashableString(GLSL::GenerateVertexShader(setup, config, extra_config));

        if (program.empty()) {
            LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
            programmable_vertex_map.erase(config_hash);
            return {};
        }

        const u64 spirv_id = program.Hash();

        auto [iter_prog, new_program] =
            programmable_vertex_cache.try_emplace(spirv_id, parent.instance);
        auto& shader = iter_prog->second;

        if (new_program) {
            LOG_NEW_OBJECT(Render_Vulkan, "New VS SPIRV {:016X}", spirv_id);

            shader.program = std::move(program);
            const vk::Device device = parent.instance.GetDevice();
            parent.shader_workers.QueueWork([device, &shader, this, spirv_id] {
                auto spirv = CompileGLSL(shader.program, vk::ShaderStageFlagBits::eVertex);
                AppendVSSPIRV(vs_cache, spirv, spirv_id);
                shader.program.clear();
                shader.module = CompileSPV(spirv, device);
                shader.MarkDone();
            });
        }

        AppendVSConfigProgram(vs_cache, config, setup, config_hash, spirv_id);

        iter_config->second = &shader;
    }

    Shader* const shader{iter_config->second};
    if (!shader) {
        LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
        return {};
    }

    return std::make_pair(config_hash, shader);
}

std::optional<std::pair<u64, Shader* const>> ShaderDiskCache::UseFragmentShader(
    const Pica::RegsInternal& regs, const Pica::Shader::UserConfig& user, bool atlas) {

    const FSConfig fs_config{regs};
    const auto fs_config_hash = atlas ? Common::HashCombine(fs_config.Hash(),u64{0x4d4d334441544c05}) : fs_config.Hash();
    const auto [it, new_shader] = fragment_shaders.try_emplace(fs_config_hash, parent.instance);
    auto& shader = it->second;

    if (new_shader) {
        LOG_NEW_OBJECT(Render_Vulkan, "New FS config {:016X}", fs_config_hash);

        parent.shader_workers.QueueWork([fs_config, user, this, &shader, fs_config_hash, atlas]() {
            std::vector<u32> spirv;
            const bool use_spirv = parent.profile.vk_use_spirv_generator && !atlas;
            if (use_spirv && !fs_config.UsesSpirvIncompatibleConfig()) {
                spirv = SPIRV::GenerateFragmentShader(fs_config, parent.profile);
                shader.module = CompileSPV(spirv, parent.instance.GetDevice());
            } else {
                std::string code =
                    GLSL::GenerateFragmentShader(fs_config, user, parent.profile);
#ifdef ENABLE_MM3D_PRESENTATION
                if(atlas) code=MM3D::Atlas::Shader(std::move(code));
#endif
                spirv = CompileGLSL(code, vk::ShaderStageFlagBits::eFragment);
                shader.module = CompileSPV(spirv, parent.instance.GetDevice());
            }
            shader.MarkDone();

            if (user.IsCacheable() && !atlas) {
                // Only cache to disk if the user config is cacheable
                AppendFSSPIRV(fs_cache, spirv, fs_config_hash);
                FSConfigEntry entry{.version = FSConfigEntry::EXPECTED_VERSION,
                                    .fs_config = fs_config};
                AppendFSConfig(fs_cache, entry, fs_config_hash);
            }
        });
    }

    return std::make_pair(fs_config_hash, &shader);
}

std::optional<std::pair<u64, Shader* const>> ShaderDiskCache::UseFixedGeometryShader(
    const Pica::RegsInternal& regs) {

    const PicaFixedGSConfig gs_config{regs};
    const auto gs_config_hash = gs_config.Hash();

    if (!parent.instance.UseGeometryShaders() ||
        parent.instance.IsFragmentShaderBarycentricSupported()) {
        // Even if we don't support geometry shaders, we still need to cache them
        // so that the shader cache is transferable. There is no need to cache
        // or build the SPIRV.

        if (known_geometry_shaders.emplace(gs_config_hash).second) {
            GSConfigEntry entry{
                .version = GSConfigEntry::EXPECTED_VERSION,
                .gs_config = gs_config,
            };
            AppendGSConfig(gs_cache, entry, gs_config_hash);
        }

        return std::make_pair(gs_config_hash, nullptr);
    } else {
        auto [it, new_shader] = fixed_geometry_shaders.try_emplace(gs_config_hash, parent.instance);
        auto& shader = it->second;

        if (new_shader) {
            LOG_NEW_OBJECT(Render_Vulkan, "New GS config {:016X}", gs_config_hash);

            parent.shader_workers.QueueWork([gs_config, this, &shader, gs_config_hash]() {
                ExtraFixedGSConfig extra;
                extra.use_clip_planes = parent.profile.has_clip_planes;
                extra.separable_shader = true;

                const auto code = GLSL::GenerateFixedGeometryShader(gs_config, extra);
                const auto spirv = CompileGLSL(code, vk::ShaderStageFlagBits::eGeometry);
                shader.module = CompileSPV(spirv, parent.instance.GetDevice());
                shader.MarkDone();

                AppendGSSPIRV(gs_cache, spirv, gs_config_hash);
                GSConfigEntry entry{
                    .version = GSConfigEntry::EXPECTED_VERSION,
                    .gs_config = gs_config,
                };
                AppendGSConfig(gs_cache, entry, gs_config_hash);
            });
        }

        return std::make_pair(gs_config_hash, &shader);
    }
}

GraphicsPipeline* ShaderDiskCache::GetPipeline(const PipelineInfo& info) {

    u64 hash = info.Hash();
    u64 optimized_hash = info.state.OptimizedHash(parent.instance);

    auto [it, new_pipeline] = graphics_pipelines.try_emplace(optimized_hash);
    if (new_pipeline) {
        // The disk cache builds every pipeline it knows at startup, so a pipeline
        // created here has to compile mid-game. The hitch trace shows its cost.
        LOG_INFO(Render_Vulkan, "Hitch trace: building pipeline {:016X} (optimized {:016X}), {}",
                 hash, optimized_hash,
                 known_graphic_pipelines.contains(hash) ? "known to the disk cache but not prebuilt"
                                                        : "never seen before");
        if (!parent.instance.UseGeometryShaders() ||
            parent.instance.IsFragmentShaderBarycentricSupported()) {
            // If we don't need geometry shaders disable
            // them before building the pipeline. It's done here
            // so that the shader ID could be hashed and saved with
            // the pipeline info so that it is transferable.
            parent.UseTrivialGeometryShader();
        }
        it.value() = std::make_unique<GraphicsPipeline>(
            parent.instance, parent.renderpass_cache, info, *parent.driver_pipeline_cache,
            *parent.pipeline_layout, parent.current_shaders, &parent.pipeline_workers);
    }

    if (known_graphic_pipelines.emplace(hash).second) {
        LOG_NEW_OBJECT(Render_Vulkan, "New Pipeline {:016X}", hash);

        PLConfigEntry entry{
            .version = PLConfigEntry::EXPECTED_VERSION,
            .pl_info = info.state,
        };

        AppendPLConfig(pl_cache, entry, hash);
    }

    return it.value().get();
}

ShaderDiskCache::SourceFileCacheVersionHash ShaderDiskCache::GetSourceFileCacheVersionHash() {
    SourceFileCacheVersionHash hash{};
    const std::size_t length = std::min(std::strlen(Common::g_shader_cache_version), hash.size());
    std::memcpy(hash.data(), Common::g_shader_cache_version, length);
    return hash;
}

ShaderDiskCache::CacheEntry ShaderDiskCache::CacheFile::ReadFirst() {
    return ReadAt(0);
}

ShaderDiskCache::CacheEntry ShaderDiskCache::CacheFile::ReadNext(const CacheEntry& previous) {
    if (!previous.valid)
        return CacheEntry();

    return ReadAt(previous.position + previous.header.entry_size);
}

std::pair<size_t, ShaderDiskCache::CacheEntry::CacheEntryHeader>
ShaderDiskCache::CacheFile::ReadNextHeader(
    const ShaderDiskCache::CacheEntry::CacheEntryHeader& previous, size_t previous_position) {

    size_t new_pos = previous_position + previous.entry_size;

    return {new_pos, ReadAtHeader(new_pos)};
}

ShaderDiskCache::CacheEntry::CacheEntryHeader ShaderDiskCache::CacheFile::ReadAtHeader(
    size_t position) {

    CacheEntry::CacheEntryHeader header;

    if (!ReadFromFileCached(&header, position, sizeof(header))) {
        return CacheEntry::CacheEntryHeader();
    }

    return header;
}

ShaderDiskCache::CacheEntry ShaderDiskCache::CacheFile::ReadAt(size_t position) {
    CacheEntry res{};
    res.position = position;

    constexpr u32 headers_size =
        sizeof(CacheEntry::CacheEntryHeader) + sizeof(CacheEntry::CacheEntryFooter);

    res.header = ReadAtHeader(position);

    if (res.header.Valid()) {

        // We have everything validated, read the data.
        u32 payload_size = res.header.entry_size - headers_size;
        std::vector<u8> payload(payload_size);

        if (ReadFromFileCached(payload.data(), (position + sizeof(CacheEntry::CacheEntryHeader)),
                               payload_size)) {
            // Decompress data if needed
            if (res.header.zstd_compressed) {
                if (Common::Compression::GetDecompressedSize(payload) <
                    CacheEntry::MAX_ENTRY_SIZE) {
                    res.data = Common::Compression::DecompressDataZSTD(payload);
                    res.valid = true;
                }
            } else {
                res.data = std::move(payload);
                res.valid = true;
            }
        }
    }
    return res;
}

size_t ShaderDiskCache::CacheFile::GetTotalEntries() {
    if (!file.IsGood()) {
        next_entry_id = SIZE_MAX;
        file_size = 0;
        return next_entry_id;
    }

    if (next_entry_id != SIZE_MAX) {
        return next_entry_id;
    }

    file_size = file.GetSize();
    if (file_size == 0) {
        next_entry_id = 0;
        return next_entry_id;
    }

    CacheEntry::CacheEntryFooter footer{};

    if (file.ReadAtArray(&footer, 1, file_size - sizeof(footer)) == sizeof(footer) &&
        footer.version == CacheEntry::CacheEntryFooter::ENTRY_VERSION) {
        next_entry_id = footer.entry_id + 1;
    } else {
        file_size = 0;
        return SIZE_MAX;
    }

    return next_entry_id;
}

void ShaderDiskCache::CacheFile::Append(CacheEntryType type, u64 id, std::span<const u8> data,
                                        bool compress) {
    if (curr_mode != CacheOpMode::APPEND) {
        return;
    }

    std::vector<u8> copy_data(data.begin(), data.end());

    append_worker.QueueWork([this, type, id, compress, data = std::move(copy_data)]() {
        if (next_entry_id == SIZE_MAX || !file.IsGood()) {
            return;
        }

        std::span<const u8> data_final;
        std::vector<u8> data_compress;

        CacheEntry::CacheEntryHeader header{};
        CacheEntry::CacheEntryFooter footer{};

        constexpr u32 headers_size =
            sizeof(CacheEntry::CacheEntryHeader) + sizeof(CacheEntry::CacheEntryFooter);

        if (compress) {
            data_compress = Common::Compression::CompressDataZSTDDefault(data);
            data_final = data_compress;
            header.zstd_compressed.Assign(true);
        } else {
            data_final = data;
        }
        header.entry_version = CacheEntry::CacheEntryHeader::ENTRY_VERSION;
        footer.version.Assign(CacheEntry::CacheEntryFooter::ENTRY_VERSION);
        header.entry_size = footer.entry_size = data_final.size() + headers_size;
        footer.entry_id.Assign(next_entry_id++);

        header.type = type;
        header.id = id;

        std::vector<u8> out_data(data_final.size() + headers_size);
        memcpy(out_data.data(), &header, sizeof(header));
        memcpy(out_data.data() + sizeof(header), data_final.data(), data_final.size());
        memcpy(out_data.data() + sizeof(header) + data_final.size(), &footer, sizeof(footer));

        file.WriteBytes(out_data.data(), out_data.size());
        if (file.IsGood()) {
            file.Flush();
        }
    });
}

bool ShaderDiskCache::CacheFile::SwitchMode(CacheOpMode mode) {
    if (curr_mode == mode) {
        return true;
    }
    if (curr_mode == CacheOpMode::APPEND) {
        append_worker.WaitForRequests();
    }

    switch (mode) {
    case CacheOpMode::READ: {
        next_entry_id = SIZE_MAX; // Force reading entries again
        cached_file_data_start = 0;
        cached_file_data.clear();
        file = FileUtil::IOFile(filepath, "rb");
        bool is_open = file.IsGood();
        if (is_open) {
            GetTotalEntries();
        }
        curr_mode = mode;
        return is_open;
    }
    case CacheOpMode::APPEND: {
        if (!SwitchMode(CacheOpMode::READ)) {
            curr_mode = mode;
            return false;
        }
        file.Close();
        cached_file_data_start = 0;
        cached_file_data.clear();
        curr_mode = mode;
        if (next_entry_id == SIZE_MAX) {
            // Cannot append if getting total items fails
            return false;
        }

        file = FileUtil::IOFile(filepath, "ab");
        return file.IsGood();
    }
    case CacheOpMode::DELETE: {
        next_entry_id = SIZE_MAX;
        cached_file_data_start = 0;
        cached_file_data.clear();
        file.Close();
        curr_mode = mode;
        return FileUtil::Delete(filepath);
    }
    case CacheOpMode::RECREATE: {
        if (!SwitchMode(CacheOpMode::DELETE)) {
            return false;
        }
        if (!FileUtil::CreateEmptyFile(filepath)) {
            return false;
        }
        return SwitchMode(CacheOpMode::APPEND);
    }
    default:
        UNREACHABLE();
    }
    return false;
}

bool ShaderDiskCache::CacheFile::ReadFromFileCached(void* dst, size_t position, size_t size) {
    if (!dst || position + size > file_size) {
        return false;
    }

    size_t offset = position - cached_file_data_start;
    if (position < cached_file_data_start || offset > cached_file_data.size() ||
        size > cached_file_data.size() - offset) {
        if (size > CacheEntry::MAX_ENTRY_SIZE) {
            return false;
        }

        size_t to_read = std::min<size_t>(CacheEntry::MAX_ENTRY_SIZE, file_size - position);

        cached_file_data_start = position;
        cached_file_data.resize(to_read);

        if (file.ReadAtBytes(cached_file_data.data(), to_read, position) != to_read) {
            return false;
        }

        offset = 0;
    }

    std::memcpy(dst, cached_file_data.data() + offset, size);
    return true;
}

std::string ShaderDiskCache::GetVSFile(u64 title_id, bool is_temp) const {
    return parent.GetTransferableDir() + DIR_SEP + fmt::format("{:016X}_vs", title_id) +
           (is_temp ? "_temp" : "") + ".vkch";
}

std::string ShaderDiskCache::GetFSFile(u64 title_id, bool is_temp) const {
    return parent.GetTransferableDir() + DIR_SEP + fmt::format("{:016X}_fs", title_id) +
           (is_temp ? "_temp" : "") + ".vkch";
}

std::string ShaderDiskCache::GetGSFile(u64 title_id, bool is_temp) const {
    return parent.GetTransferableDir() + DIR_SEP + fmt::format("{:016X}_gs", title_id) +
           (is_temp ? "_temp" : "") + ".vkch";
}

std::string ShaderDiskCache::GetPLFile(u64 title_id, bool is_temp) const {
    return parent.GetTransferableDir() + DIR_SEP + fmt::format("{:016X}_pl", title_id) +
           (is_temp ? "_temp" : "") + ".vkch";
}

bool ShaderDiskCache::RecreateCache(CacheFile& file, CacheFileType type) {
    file.SwitchMode(CacheFile::CacheOpMode::RECREATE);

    std::array<char, 0x20> build_name{};
    size_t name_len = std::strlen(Common::g_build_fullname);
    memcpy(build_name.data(), Common::g_build_fullname, std::min(name_len, build_name.size()));

    auto get_hash = [](CacheFileType type) {
        switch (type) {
        case CacheFileType::VS_CACHE:
            return PicaVSConfigState::StructHash();
        case CacheFileType::FS_CACHE:
            return FSConfig::StructHash();
        case CacheFileType::GS_CACHE:
            return PicaGSConfigState::StructHash();
        case CacheFileType::PL_CACHE:
            return StaticPipelineInfo::StructHash();
        default:
            UNREACHABLE();
            return u64{};
        };
    };

    FileInfoEntry entry{
        .cache_magic = FileInfoEntry::CACHE_FILE_MAGIC,
        .file_version = FileInfoEntry::CACHE_FILE_VERSION,
        .config_struct_hash = get_hash(type),
        .file_type = type,
        .source_hash = GetSourceFileCacheVersionHash(),
        .build_name = build_name,
        .profile = parent.profile,
    };

    file.Append(CacheEntryType::FILE_INFO, 0, entry, false);
    return true;
}

void ShaderDiskCache::ImportCaches() {
    const std::string import_dir =
        FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) + "import" + DIR_SEP;
    if (!FileUtil::IsDirectory(import_dir)) {
        return;
    }
    const std::array<std::pair<CacheFileType, std::string>, 4> files{{
        {CacheFileType::VS_CACHE, GetVSFile(title_id, false)},
        {CacheFileType::FS_CACHE, GetFSFile(title_id, false)},
        {CacheFileType::GS_CACHE, GetGSFile(title_id, false)},
        {CacheFileType::PL_CACHE, GetPLFile(title_id, false)},
    }};
    for (const auto& [type, cache_path] : files) {
        const std::string name = cache_path.substr(cache_path.find_last_of("/\\") + 1);
        const std::string import_path = import_dir + name;
        if (!FileUtil::Exists(import_path)) {
            continue;
        }
        if (MergeImported(type, import_path, cache_path)) {
            FileUtil::Delete(import_path);
        }
    }
}

bool ShaderDiskCache::MergeImported(CacheFileType type, const std::string& import_path,
                                    const std::string& cache_path) {
    using Header = CacheEntry::CacheEntryHeader;
    using Footer = CacheEntry::CacheEntryFooter;
    constexpr size_t headers_size = sizeof(Header) + sizeof(Footer);

    struct Entry {
        Header header;
        std::span<const u8> payload; // header and footer excluded
    };
    // Splits a cache file into its entries; empty if the file is not a valid
    // cache of this type and config layout.
    const auto parse = [&](const std::vector<u8>& data) {
        std::vector<Entry> out;
        for (size_t pos = 0; pos + headers_size <= data.size();) {
            Header header;
            std::memcpy(&header, data.data() + pos, sizeof(header));
            if (!header.Valid() || pos + header.entry_size > data.size()) {
                break; // keep the entries before a torn write
            }
            out.push_back({header, std::span<const u8>(data.data() + pos + sizeof(Header),
                                                       header.entry_size - headers_size)});
            pos += header.entry_size;
        }
        if (out.empty() || out[0].header.Type() != CacheEntryType::FILE_INFO ||
            out[0].header.zstd_compressed || out[0].payload.size() != sizeof(FileInfoEntry)) {
            return std::vector<Entry>{};
        }
        const auto& info = *reinterpret_cast<const FileInfoEntry*>(out[0].payload.data());
        const u64 struct_hash = type == CacheFileType::VS_CACHE ? PicaVSConfigState::StructHash()
                                : type == CacheFileType::FS_CACHE ? FSConfig::StructHash()
                                : type == CacheFileType::GS_CACHE
                                    ? PicaGSConfigState::StructHash()
                                    : StaticPipelineInfo::StructHash();
        if (info.cache_magic != FileInfoEntry::CACHE_FILE_MAGIC ||
            info.file_version != FileInfoEntry::CACHE_FILE_VERSION || info.file_type != type ||
            info.config_struct_hash != struct_hash) {
            return std::vector<Entry>{};
        }
        return out;
    };
    const auto read = [](const std::string& path) {
        std::vector<u8> data;
        FileUtil::IOFile file(path, "rb");
        if (file.IsOpen()) {
            data.resize(file.GetSize());
            if (file.ReadBytes(data.data(), data.size()) != data.size()) {
                data.clear();
            }
        }
        return data;
    };

    const std::vector<u8> imported_data = read(import_path);
    const std::vector<u8> own_data = read(cache_path);
    const auto imported = parse(imported_data);
    const auto own = parse(own_data);
    if (imported.empty()) {
        LOG_ERROR(Render_Vulkan, "Shader cache import: {} is not a usable cache file, skipped",
                  import_path);
        return false;
    }

    // Keep this device's entries and add the new ones. The SPIR-V of the two
    // may come from different drivers or builds, so clear the source hash in
    // the header: the next load regenerates all SPIR-V from the configs.
    const auto& info_source = (own.empty() ? imported : own)[0].payload;
    std::vector<u8> info_bytes(info_source.begin(), info_source.end());
    reinterpret_cast<FileInfoEntry*>(info_bytes.data())->source_hash = {};

    std::vector<u8> out;
    u32 next_id = 0;
    const auto append = [&](CacheEntryType entry_type, u64 id, const Header& source,
                            std::span<const u8> payload) {
        Header header = source;
        header.type = entry_type;
        header.id = id;
        header.entry_size = static_cast<u32>(payload.size() + headers_size);
        Footer footer{};
        footer.version.Assign(Footer::ENTRY_VERSION);
        footer.entry_id.Assign(next_id++);
        footer.entry_size = header.entry_size;
        const auto bytes = [&](const auto& object) {
            const auto* p = reinterpret_cast<const u8*>(&object);
            out.insert(out.end(), p, p + sizeof(object));
        };
        bytes(header);
        out.insert(out.end(), payload.begin(), payload.end());
        bytes(footer);
    };
    Header info_header{};
    info_header.entry_version = Header::ENTRY_VERSION;
    append(CacheEntryType::FILE_INFO, 0, info_header, info_bytes);

    std::unordered_set<u64> seen;
    const auto key = [](const Header& h) {
        return Common::HashCombine(static_cast<u64>(h.Type()), h.Id());
    };
    size_t added = 0;
    for (size_t i = 1; i < own.size(); i++) {
        seen.insert(key(own[i].header));
        append(own[i].header.Type(), own[i].header.Id(), own[i].header, own[i].payload);
    }
    for (size_t i = 1; i < imported.size(); i++) {
        if (seen.insert(key(imported[i].header)).second) {
            append(imported[i].header.Type(), imported[i].header.Id(), imported[i].header,
                   imported[i].payload);
            added++;
        }
    }
    if (added == 0 && !own.empty()) {
        // Already merged. The import may stay behind when its folder is not
        // ours to delete (devicectl creates folders owned by root); leave the
        // cache alone so its SPIR-V is not regenerated on every boot.
        return true;
    }

    const std::string temp_path = cache_path + ".import";
    {
        FileUtil::CreateFullPath(temp_path);
        FileUtil::IOFile file(temp_path, "wb");
        if (!file.IsOpen() || file.WriteBytes(out.data(), out.size()) != out.size()) {
            LOG_ERROR(Render_Vulkan, "Shader cache import: cannot write {}", temp_path);
            return false;
        }
    }
    FileUtil::Delete(cache_path);
    if (!FileUtil::Rename(temp_path, cache_path)) {
        LOG_ERROR(Render_Vulkan, "Shader cache import: cannot replace {}", cache_path);
        return false;
    }
    LOG_INFO(Render_Vulkan, "Shader cache import: {} new of {} entries merged into {}", added,
             imported.size() - 1, cache_path);
    return true;
}

bool ShaderDiskCache::InitVSCache(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) {
    std::vector<size_t> pending_configs;
    std::unordered_map<u64, size_t> pending_programs;
    std::unique_ptr<Pica::ShaderSetup> shader_setup;
    std::unique_ptr<CacheFile> regenerate_file;
    // Reuse summary, logged at the end: shows whether a cache from another device was usable.
    size_t spirv_loaded = 0, configs_linked = 0, configs_rebuilt = 0, configs_rejected = 0;

    auto cleanup_on_error = [&]() {
        programmable_vertex_cache.clear();
        programmable_vertex_map.clear();
        known_vertex_programs.clear();
        if (regenerate_file) {
            regenerate_file->SwitchMode(CacheFile::CacheOpMode::DELETE);
        }
    };

    LOG_INFO(Render_Vulkan, "Loading VS disk shader cache for title {:016X}", title_id);

    vs_cache.SetFilePath(GetVSFile(title_id, false));

    if (!vs_cache.SwitchMode(CacheFile::CacheOpMode::READ)) {
        LOG_INFO(Render_Vulkan, "Missing VS disk shader cache for title {:016X}", title_id);
        cleanup_on_error();
        return false;
    }

    u32 tot_entries = vs_cache.GetTotalEntries();
    auto curr = vs_cache.ReadFirst();
    if (!curr.Valid() || curr.Type() != CacheEntryType::FILE_INFO) {
        MALFORMED_DISK_CACHE;
    }

    const FileInfoEntry* file_info = curr.Payload<FileInfoEntry>();
    if (!file_info || file_info->cache_magic != FileInfoEntry::CACHE_FILE_MAGIC ||
        file_info->file_version != FileInfoEntry::CACHE_FILE_VERSION ||
        file_info->file_type != CacheFileType::VS_CACHE) {
        MALFORMED_DISK_CACHE;
    }

    if (file_info->config_struct_hash != PicaVSConfigState::StructHash()) {
        LOG_ERROR(Render_Vulkan,
                  "Cache was created for a different PicaVSConfigState, resetting...");
        cleanup_on_error();
        return false;
    }

    if (file_info->source_hash != GetSourceFileCacheVersionHash()) {
        LOG_INFO(Render_Vulkan, "Cache contains old vertex program, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetVSFile(title_id, true));
    }

    if (file_info->profile != parent.profile && !regenerate_file) {
        LOG_INFO(Render_Vulkan,
                 "Cache has driver and user settings mismatch, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetVSFile(title_id, true));
    }

    const bool regenerate_file_used = regenerate_file != nullptr;
    if (regenerate_file) {
        RecreateCache(*regenerate_file, CacheFileType::VS_CACHE);
    }

    CacheEntry::CacheEntryHeader curr_header = curr.Header();
    size_t curr_offset = curr.Position();

    size_t current_callback_index = 0;
    size_t tot_callback_index = tot_entries - 1;

    // Scan the entire file first, while keeping track of configs and programs.
    // SPIRV can be compiled directly and will be linked to the proper config entries
    // later.
    for (int i = 1; i < tot_entries; i++) {

        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        std::tie(curr_offset, curr_header) = vs_cache.ReadNextHeader(curr_header, curr_offset);

        if (!curr_header.Valid()) {
            MALFORMED_DISK_CACHE;
        }

        LOG_DEBUG(Render_Vulkan, "Processing ID: {:016X} (type {})", curr_header.Id(),
                  curr_header.Type());

        if (curr_header.Type() == CacheEntryType::VS_CONFIG) {
            pending_configs.push_back(curr_offset);
        } else if (curr_header.Type() == CacheEntryType::VS_PROGRAM) {
            pending_programs.try_emplace(curr_header.Id(), curr_offset);

            // We won't use this entry sequentially again, so report progress.
            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                         tot_callback_index, "Vertex Shader");
            }
        } else if (curr_header.Type() == CacheEntryType::VS_SPIRV) {

            // Only use SPIRV entries if we are not regenerating the cache, as the driver or
            // user settings do not match, which could lead to different SPIRV.
            // These will be re-created from the cached config and programs later.
            if (!regenerate_file) {
                LOG_DEBUG(Render_Vulkan, "    processing SPIRV.");

                curr = vs_cache.ReadAt(curr_offset);
                if (!curr.Valid() || curr.Type() != CacheEntryType::VS_SPIRV) {
                    MALFORMED_DISK_CACHE;
                }

                const u8* spirv_data = curr.Data().data();
                const size_t spirv_size = curr.Data().size();

                auto [iter_prog, new_program] =
                    programmable_vertex_cache.try_emplace(curr.Id(), parent.instance);
                if (new_program) {
                    LOG_DEBUG(Render_Vulkan, "    compiling SPIRV.");

                    const auto spirv = std::span<const u32>(
                        reinterpret_cast<const u32*>(spirv_data), spirv_size / sizeof(u32));

                    iter_prog->second.module = CompileSPV(spirv, parent.instance.GetDevice());
                    iter_prog->second.MarkDone();

                    if (!iter_prog->second.module) {
                        // Compilation failed for some reason, remove from cache to let it
                        // be re-generated at runtime or during config and program processing.
                        LOG_ERROR(Render_Vulkan, "Unexpected program compilation failure");
                        programmable_vertex_cache.erase(iter_prog);
                    } else {
                        spirv_loaded++;
                    }
                }
            }

            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                         tot_callback_index, "Vertex Shader");
            }
        } else {
            MALFORMED_DISK_CACHE;
        }
    }

    // Once we have all the shader instances created from SPIRV, we can link them to the VS configs.
    LOG_DEBUG(Render_Vulkan, "Linking with config entries.");

    // Mmultiple config entries may point to the same program entry. We could load all program
    // entries to memory to prevent having to read them from disk on every config entry, but program
    // entries are pretty big (around 50KB each). A LRU cache is a middle point between disk access
    // and memory usage.
    std::unique_ptr<Common::StaticLRUCache<u64, VSProgramEntry, 10>> program_lru =
        std::make_unique<Common::StaticLRUCache<u64, VSProgramEntry, 10>>();

    for (auto& offset : pending_configs) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        if (callback) {
            callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                     tot_callback_index, "Vertex Shader");
        }

        curr = vs_cache.ReadAt(offset);
        const VSConfigEntry* entry;

        if (!curr.Valid() || curr.Type() != CacheEntryType::VS_CONFIG ||
            !(entry = curr.Payload<VSConfigEntry>()) ||
            entry->version != VSConfigEntry::EXPECTED_VERSION) {
            MALFORMED_DISK_CACHE;
        }

        if (curr.Id() != entry->vs_config.Hash()) {
            LOG_ERROR(Render_Vulkan, "Unexpected PicaVSConfig hash mismatch");
            configs_rejected++;
            continue;
        }

        LOG_DEBUG(Render_Vulkan, "Linking {:016X}.", curr.Id());

        auto [iter_config, new_config] = programmable_vertex_map.try_emplace(curr.Id());
        if (new_config) {
            // New config entry, usually always taken unless there is duplicate entries on the cache
            // for some reason.

            // We cannot trust the SPIRV entry ID anymore if we are regenerating.
            auto shader_it = regenerate_file
                                 ? programmable_vertex_cache.end()
                                 : programmable_vertex_cache.find(entry->spirv_entry_id);
            if (shader_it != programmable_vertex_cache.end()) {
                // The config entry uses a SPIRV entry that was already compiled (this is the usual
                // path when the cache doesn't need to be re-generated).

                LOG_DEBUG(Render_Vulkan, "    linked with existing SPIRV {:016X}.",
                          entry->spirv_entry_id);

                iter_config->second = &shader_it->second;
                known_vertex_programs.emplace(entry->program_entry_id).second;
                configs_linked++;
            } else {
                // Cached SPIRV not found, need to recompile.

                // Search program entry in a LRU first, to prevent having to read from the cache
                // file on each separate config entry.
                auto [found, program_lru_entry] = program_lru->request(entry->program_entry_id);
                if (!found) {
                    LOG_DEBUG(Render_Vulkan, "    reading program {:016X}.",
                              entry->program_entry_id);

                    // Program not on the LRU, need to read it from cache file
                    auto program_it = pending_programs.find(entry->program_entry_id);
                    if (program_it == pending_programs.end()) {
                        // Program code not in disk cache, should never happen.
                        LOG_ERROR(Render_Vulkan, "Missing program code for config entry");
                        programmable_vertex_map.erase(iter_config);
                        configs_rejected++;
                        continue;
                    }

                    auto program_cache_entry = vs_cache.ReadAt(program_it->second);
                    const VSProgramEntry* program_entry;

                    if (!program_cache_entry.Valid() ||
                        program_cache_entry.Type() != CacheEntryType::VS_PROGRAM ||
                        !(program_entry = program_cache_entry.Payload<VSProgramEntry>()) ||
                        program_entry->version != VSProgramEntry::EXPECTED_VERSION) {
                        MALFORMED_DISK_CACHE;
                    }

                    program_lru_entry = *program_entry;

                    bool new_program =
                        known_vertex_programs.emplace(entry->program_entry_id).second;

                    if (new_program && regenerate_file) {
                        // When regenerating, only append if it's a new program entry not seen
                        // before.
                        AppendVSProgram(*regenerate_file, program_lru_entry,
                                        entry->program_entry_id);
                    }
                }

                // Recompile SPIRV from config and program now.
                LOG_DEBUG(Render_Vulkan, "    using program {:016X}.", entry->program_entry_id);

                shader_setup = std::make_unique<Pica::ShaderSetup>();
                shader_setup->UpdateProgramCode(program_lru_entry.program_code,
                                                program_lru_entry.program_len);
                shader_setup->UpdateSwizzleData(program_lru_entry.swizzle_code,
                                                program_lru_entry.swizzle_len);
                shader_setup->DoProgramCodeFixup();

                if (entry->vs_config.state.program_hash != shader_setup->GetProgramCodeHash() ||
                    entry->vs_config.state.swizzle_hash != shader_setup->GetSwizzleDataHash()) {
                    LOG_ERROR(Render_Vulkan, "Unexpected ShaderSetup hash mismatch");
                    programmable_vertex_map.erase(iter_config);
                    configs_rejected++;
                    continue;
                }

                ExtraVSConfig extra_config = parent.CalcExtraConfig(entry->vs_config);

                auto program_glsl = Common::HashableString(
                    GLSL::GenerateVertexShader(*shader_setup, entry->vs_config, extra_config));
                if (program_glsl.empty()) {
                    LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
                    programmable_vertex_map.erase(iter_config);
                    configs_rejected++;
                    continue;
                }

                const u64 spirv_id = program_glsl.Hash();

                auto [iter_prog, new_spirv] =
                    programmable_vertex_cache.try_emplace(spirv_id, parent.instance);

                LOG_DEBUG(Render_Vulkan, "    processing SPIRV.");

                if (new_spirv) {
                    LOG_DEBUG(Render_Vulkan, "    compiling SPIRV.");

                    auto spirv = CompileGLSL(program_glsl, vk::ShaderStageFlagBits::eVertex);

                    iter_prog->second.module = CompileSPV(spirv, parent.instance.GetDevice());
                    iter_prog->second.MarkDone();

                    if (regenerate_file) {
                        // If we are regenerating, save the new spirv to disk.
                        AppendVSSPIRV(*regenerate_file, spirv, spirv_id);
                    }
                }

                if (regenerate_file) {
                    // If we are regenerating, save the config entry to the cache. We need to make a
                    // copy first because it's possible the SPIRV id has changed and we need to
                    // adjust it.
                    std::unique_ptr<VSConfigEntry> entry_copy =
                        std::make_unique<VSConfigEntry>(*entry);
                    entry_copy->spirv_entry_id = spirv_id;
                    AppendVSConfig(*regenerate_file, *entry_copy, curr.Id());
                }

                // Asign the SPIRV shader to the config
                iter_config->second = &iter_prog->second;
                configs_rebuilt++;

                LOG_DEBUG(Render_Vulkan, "    linked with SPIRV {:016X}.", entry->spirv_entry_id);
            }
        }
    }

    if (regenerate_file) {
        // If we are regenerating, replace the old file with the new one.
        vs_cache.SwitchMode(CacheFile::CacheOpMode::DELETE);
        regenerate_file.reset();
        FileUtil::Rename(GetVSFile(title_id, true), GetVSFile(title_id, false));
    }

    LOG_INFO(Render_Vulkan,
             "VS disk cache: {} SPIR-V loaded, {} configs linked to it, {} rebuilt from "
             "config, {} rejected{}",
             spirv_loaded, configs_linked, configs_rebuilt, configs_rejected,
             regenerate_file_used ? " (regenerated)" : "");

    // Switch to append mode to receive new entries.
    return vs_cache.SwitchMode(CacheFile::CacheOpMode::APPEND);
}

bool ShaderDiskCache::InitFSCache(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) {
    std::vector<std::pair<u64, size_t>> pending_configs;
    std::unique_ptr<CacheFile> regenerate_file;
    size_t spirv_loaded = 0, configs_rebuilt = 0, configs_rejected = 0;

    auto cleanup_on_error = [&]() {
        fragment_shaders.clear();
        if (regenerate_file) {
            regenerate_file->SwitchMode(CacheFile::CacheOpMode::DELETE);
        }
    };

    LOG_INFO(Render_Vulkan, "Loading FS disk shader cache for title {:016X}", title_id);

    fs_cache.SetFilePath(GetFSFile(title_id, false));

    if (!fs_cache.SwitchMode(CacheFile::CacheOpMode::READ)) {
        LOG_INFO(Render_Vulkan, "Missing FS disk shader cache for title {:016X}", title_id);
        cleanup_on_error();
        return false;
    }

    u32 tot_entries = fs_cache.GetTotalEntries();
    auto curr = fs_cache.ReadFirst();
    if (!curr.Valid() || curr.Type() != CacheEntryType::FILE_INFO) {
        MALFORMED_DISK_CACHE;
    }

    const FileInfoEntry* file_info = curr.Payload<FileInfoEntry>();
    if (!file_info || file_info->cache_magic != FileInfoEntry::CACHE_FILE_MAGIC ||
        file_info->file_version != FileInfoEntry::CACHE_FILE_VERSION ||
        file_info->file_type != CacheFileType::FS_CACHE) {
        MALFORMED_DISK_CACHE;
    }

    if (file_info->config_struct_hash != FSConfig::StructHash()) {
        LOG_ERROR(Render_Vulkan, "Cache was created for a different FSConfig, resetting...");
        cleanup_on_error();
        return false;
    }

    if (file_info->source_hash != GetSourceFileCacheVersionHash()) {
        LOG_INFO(Render_Vulkan, "Cache contains old fragment program, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetFSFile(title_id, true));
    }

    if (file_info->profile != parent.profile && !regenerate_file) {
        LOG_INFO(Render_Vulkan,
                 "Cache has driver and user settings mismatch, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetFSFile(title_id, true));
    }

    const bool regenerate_file_used = regenerate_file != nullptr;
    if (regenerate_file) {
        RecreateCache(*regenerate_file, CacheFileType::FS_CACHE);
    }

    CacheEntry::CacheEntryHeader curr_header = curr.Header();
    size_t curr_offset = curr.Position();

    size_t current_callback_index = 0;
    size_t tot_callback_index = tot_entries - 1;

    // Scan the entire file first, while keeping track of configs.
    // SPIRV can be compiled directly, if a config has a missing
    // SPIRV entry it can be regenerated later.
    for (int i = 1; i < tot_entries; i++) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        std::tie(curr_offset, curr_header) = fs_cache.ReadNextHeader(curr_header, curr_offset);

        if (!curr_header.Valid()) {
            MALFORMED_DISK_CACHE;
        }

        LOG_DEBUG(Render_Vulkan, "Processing ID: {:016X} (type {})", curr_header.Id(),
                  curr_header.Type());

        if (curr_header.Type() == CacheEntryType::FS_CONFIG) {
            pending_configs.push_back({curr_header.Id(), curr_offset});
        } else if (curr_header.Type() == CacheEntryType::FS_SPIRV) {

            // Only use SPIRV entries if we are not regenerating the cache, as the driver or
            // user settings do not match, which could lead to different SPIRV.
            // These will be regenerated from the cached config later.
            if (!regenerate_file) {
                LOG_DEBUG(Render_Vulkan, "    processing SPIRV.");

                curr = fs_cache.ReadAt(curr_offset);
                if (!curr.Valid() || curr.Type() != CacheEntryType::FS_SPIRV) {
                    MALFORMED_DISK_CACHE;
                }

                const u8* spirv_data = curr.Data().data();
                const size_t spirv_size = curr.Data().size();

                auto [iter_spirv, new_program] =
                    fragment_shaders.try_emplace(curr.Id(), parent.instance);
                if (new_program) {
                    LOG_DEBUG(Render_Vulkan, "    compiling SPIRV.");

                    const auto spirv = std::span<const u32>(
                        reinterpret_cast<const u32*>(spirv_data), spirv_size / sizeof(u32));

                    iter_spirv->second.module = CompileSPV(spirv, parent.instance.GetDevice());
                    iter_spirv->second.MarkDone();

                    if (!iter_spirv->second.module) {
                        // Compilation failed for some reason, remove from cache to let it
                        // be regenerated at runtime or during config processing.
                        LOG_ERROR(Render_Vulkan, "Unexpected program compilation failure");
                        fragment_shaders.erase(iter_spirv);
                    } else {
                        spirv_loaded++;
                    }
                }
            }

            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                         tot_callback_index, "Fragment Shader");
            }
        } else {
            MALFORMED_DISK_CACHE;
        }
    }

    // Once we have all the shader instances created from SPIRV, we can link them to the FS configs.
    LOG_DEBUG(Render_Vulkan, "Linking with config entries.");

    for (auto& offset : pending_configs) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        if (callback) {
            callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                     tot_callback_index, "Fragment Shader");
        }

        LOG_DEBUG(Render_Vulkan, "Linking {:016X}.", offset.first);

        if (fragment_shaders.find(offset.first) != fragment_shaders.end()) {
            // SPIRV of config was already compiled, no need to regenerate
            // it from the cache. This can only happen if we are not regenerating
            // the cache.
            LOG_DEBUG(Render_Vulkan, "    linked with existing SPIRV.");
            continue;
        }

        // Cached SPIRV not found, need to recompile. Should only happen if
        // we are regenerating the cache.

        curr = fs_cache.ReadAt(offset.second);
        const FSConfigEntry* entry;

        if (!curr.Valid() || curr.Type() != CacheEntryType::FS_CONFIG ||
            !(entry = curr.Payload<FSConfigEntry>()) ||
            entry->version != FSConfigEntry::EXPECTED_VERSION) {
            MALFORMED_DISK_CACHE;
        }

        const auto fs_config_hash = entry->fs_config.Hash();
        if (curr.Id() != fs_config_hash) {
            LOG_ERROR(Render_Vulkan, "Unexpected FSConfig hash mismatch");
            configs_rejected++;
            continue;
        }

        const auto [it, new_shader] = fragment_shaders.try_emplace(fs_config_hash, parent.instance);
        auto& shader = it->second;

        std::vector<u32> spirv;
        if (parent.profile.vk_use_spirv_generator &&
            !entry->fs_config.UsesSpirvIncompatibleConfig()) {
            // Use SPIRV generator directly

            spirv = SPIRV::GenerateFragmentShader(entry->fs_config, parent.profile);
            shader.module = CompileSPV(spirv, parent.instance.GetDevice());
        } else {
            // Use GLSL generator then convert to SPIRV

            UserConfig user{};
            const std::string code_glsl =
                GLSL::GenerateFragmentShader(entry->fs_config, user, parent.profile);

            if (code_glsl.empty()) {
                LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
                fragment_shaders.erase(it);
                configs_rejected++;
                continue;
            }

            spirv = CompileGLSL(code_glsl, vk::ShaderStageFlagBits::eFragment);
            shader.module = CompileSPV(spirv, parent.instance.GetDevice());
        }
        shader.MarkDone();
        configs_rebuilt++;

        if (regenerate_file) {
            // Append the config and SPIRV to the new file.
            AppendFSSPIRV(*regenerate_file, spirv, fs_config_hash);
            AppendFSConfig(*regenerate_file, *entry, fs_config_hash);
        }

        LOG_DEBUG(Render_Vulkan, "    linked with new SPIRV.");
    }

    if (regenerate_file) {
        // If we are regenerating, replace the old file with the new one.
        fs_cache.SwitchMode(CacheFile::CacheOpMode::DELETE);
        regenerate_file.reset();
        FileUtil::Rename(GetFSFile(title_id, true), GetFSFile(title_id, false));
    }

    LOG_INFO(Render_Vulkan,
             "FS disk cache: {} configs, {} SPIR-V loaded, {} rebuilt from config, {} "
             "rejected{}",
             pending_configs.size(), spirv_loaded, configs_rebuilt, configs_rejected,
             regenerate_file_used ? " (regenerated)" : "");

    // Switch to append mode to receive new entries.
    return fs_cache.SwitchMode(CacheFile::CacheOpMode::APPEND);
}

bool ShaderDiskCache::InitGSCache(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) {
    std::vector<std::pair<u64, size_t>> pending_configs;
    std::unique_ptr<CacheFile> regenerate_file;

    auto cleanup_on_error = [&]() {
        fixed_geometry_shaders.clear();
        if (regenerate_file) {
            regenerate_file->SwitchMode(CacheFile::CacheOpMode::DELETE);
        }
    };

    LOG_INFO(Render_Vulkan, "Loading GS disk shader cache for title {:016X}", title_id);

    gs_cache.SetFilePath(GetGSFile(title_id, false));

    if (!gs_cache.SwitchMode(CacheFile::CacheOpMode::READ)) {
        LOG_INFO(Render_Vulkan, "Missing GS disk shader cache for title {:016X}", title_id);
        cleanup_on_error();
        return false;
    }

    u32 tot_entries = gs_cache.GetTotalEntries();
    auto curr = gs_cache.ReadFirst();
    if (!curr.Valid() || curr.Type() != CacheEntryType::FILE_INFO) {
        MALFORMED_DISK_CACHE;
    }

    const FileInfoEntry* file_info = curr.Payload<FileInfoEntry>();
    if (!file_info || file_info->cache_magic != FileInfoEntry::CACHE_FILE_MAGIC ||
        file_info->file_version != FileInfoEntry::CACHE_FILE_VERSION ||
        file_info->file_type != CacheFileType::GS_CACHE) {
        MALFORMED_DISK_CACHE;
    }

    if (file_info->config_struct_hash != PicaGSConfigState::StructHash()) {
        LOG_ERROR(Render_Vulkan,
                  "Cache was created for a different PicaGSConfigState, resetting...");
        cleanup_on_error();
        return false;
    }

    // There is no need to load geometry shaders if we don't support them.
    // We can just load the known IDs and skip SPIRV and cache regeneration.
    const auto geo_shaders_needed = parent.instance.UseGeometryShaders() &&
                                    !parent.instance.IsFragmentShaderBarycentricSupported();

    if (geo_shaders_needed && file_info->source_hash != GetSourceFileCacheVersionHash()) {
        LOG_INFO(Render_Vulkan, "Cache contains old fragment program, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetGSFile(title_id, true));
    }

    if (geo_shaders_needed && file_info->profile != parent.profile && !regenerate_file) {
        LOG_INFO(Render_Vulkan,
                 "Cache has driver and user settings mismatch, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetGSFile(title_id, true));
    }

    if (regenerate_file) {
        RecreateCache(*regenerate_file, CacheFileType::GS_CACHE);
    }

    CacheEntry::CacheEntryHeader curr_header = curr.Header();
    size_t curr_offset = curr.Position();

    size_t current_callback_index = 0;
    size_t tot_callback_index = tot_entries - 1;

    // Scan the entire file first, while keeping track of configs.
    // SPIRV can be compiled directly, if a config has a missing
    // SPIRV entry it can be regenerated later.
    for (int i = 1; i < tot_entries; i++) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        std::tie(curr_offset, curr_header) = gs_cache.ReadNextHeader(curr_header, curr_offset);

        if (!curr_header.Valid()) {
            MALFORMED_DISK_CACHE;
        }

        LOG_DEBUG(Render_Vulkan, "Processing ID: {:016X} (type {})", curr_header.Id(),
                  curr_header.Type());

        if (curr_header.Type() == CacheEntryType::GS_CONFIG) {
            if (geo_shaders_needed) {
                pending_configs.push_back({curr_header.Id(), curr_offset});
            } else {
                known_geometry_shaders.emplace(curr_header.Id());

                if (callback) {
                    callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                             tot_callback_index, "Geometry Shader");
                }
            }
        } else if (curr_header.Type() == CacheEntryType::GS_SPIRV) {

            // Only use SPIRV entries if we are not regenerating the cache, as the driver or
            // user settings do not match, which could lead to different SPIRV.
            // These will be regenerated from the cached config later.
            // Also, only use SPIRV entries if we support geometry shaders on this device.
            if (geo_shaders_needed && !regenerate_file) {
                LOG_DEBUG(Render_Vulkan, "    processing SPIRV.");

                curr = gs_cache.ReadAt(curr_offset);
                if (!curr.Valid() || curr.Type() != CacheEntryType::GS_SPIRV) {
                    MALFORMED_DISK_CACHE;
                }

                const u8* spirv_data = curr.Data().data();
                const size_t spirv_size = curr.Data().size();

                auto [iter_spirv, new_program] =
                    fixed_geometry_shaders.try_emplace(curr.Id(), parent.instance);
                if (new_program) {
                    LOG_DEBUG(Render_Vulkan, "    compiling SPIRV.");

                    const auto spirv = std::span<const u32>(
                        reinterpret_cast<const u32*>(spirv_data), spirv_size / sizeof(u32));

                    iter_spirv->second.module = CompileSPV(spirv, parent.instance.GetDevice());
                    iter_spirv->second.MarkDone();

                    if (!iter_spirv->second.module) {
                        // Compilation failed for some reason, remove from cache to let it
                        // be regenerated at runtime or during config processing.
                        LOG_ERROR(Render_Vulkan, "Unexpected program compilation failure");
                        fixed_geometry_shaders.erase(iter_spirv);
                    }
                }
            }

            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                         tot_callback_index, "Geometry Shader");
            }
        } else {
            MALFORMED_DISK_CACHE;
        }
    }

    // Once we have all the shader instances created from SPIRV, we can link them to the FS configs.
    LOG_DEBUG(Render_Vulkan, "Linking with config entries.");

    for (auto& offset : pending_configs) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        if (callback) {
            callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                     tot_callback_index, "Geometry Shader");
        }

        LOG_DEBUG(Render_Vulkan, "Linking {:016X}.", offset.first);

        if (fixed_geometry_shaders.find(offset.first) != fixed_geometry_shaders.end()) {
            // SPIRV of config was already compiled, no need to regenerate
            // it from the cache. This can only happen if we are not regenerating
            // the cache.
            LOG_DEBUG(Render_Vulkan, "    linked with existing SPIRV.");
            continue;
        }

        // Cached SPIRV not found, need to recompile. Should only happen if
        // we are regenerating the cache.

        curr = gs_cache.ReadAt(offset.second);
        const GSConfigEntry* entry;

        if (!curr.Valid() || curr.Type() != CacheEntryType::GS_CONFIG ||
            !(entry = curr.Payload<GSConfigEntry>()) ||
            entry->version != GSConfigEntry::EXPECTED_VERSION) {
            MALFORMED_DISK_CACHE;
        }

        const auto gs_config_hash = entry->gs_config.Hash();
        if (curr.Id() != gs_config_hash) {
            LOG_ERROR(Render_Vulkan, "Unexpected PicaGSConfigState hash mismatch");
            continue;
        }

        const auto [it, new_shader] =
            fixed_geometry_shaders.try_emplace(gs_config_hash, parent.instance);
        auto& shader = it->second;

        std::vector<u32> spirv;
        ExtraFixedGSConfig extra;
        extra.use_clip_planes = parent.profile.has_clip_planes;
        extra.separable_shader = true;

        const auto code_glsl = GLSL::GenerateFixedGeometryShader(entry->gs_config, extra);

        if (code_glsl.empty()) {
            LOG_ERROR(Render_Vulkan, "Failed to retrieve fixed geometry shader");
            fixed_geometry_shaders.erase(it);
            continue;
        }

        spirv = CompileGLSL(code_glsl, vk::ShaderStageFlagBits::eGeometry);
        shader.module = CompileSPV(spirv, parent.instance.GetDevice());
        shader.MarkDone();

        if (regenerate_file) {
            // Append the config and SPIRV to the new file.
            AppendGSSPIRV(*regenerate_file, spirv, gs_config_hash);
            AppendGSConfig(*regenerate_file, *entry, gs_config_hash);
        }

        LOG_DEBUG(Render_Vulkan, "    linked with new SPIRV.");
    }

    if (regenerate_file) {
        // If we are regenerating, replace the old file with the new one.
        gs_cache.SwitchMode(CacheFile::CacheOpMode::DELETE);
        regenerate_file.reset();
        FileUtil::Rename(GetGSFile(title_id, true), GetGSFile(title_id, false));
    }

    LOG_INFO(Render_Vulkan, "GS disk cache: {} configs known, {} shaders built",
             known_geometry_shaders.size() + pending_configs.size(),
             fixed_geometry_shaders.size());

    // Switch to append mode to receive new entries.
    return gs_cache.SwitchMode(CacheFile::CacheOpMode::APPEND);
}

bool ShaderDiskCache::InitPLCache(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) {

    auto cleanup_on_error = [&]() {
        // Workers may still be building queued pipelines.
        parent.pipeline_workers.WaitForRequests();
        graphics_pipelines.clear();
    };

    LOG_INFO(Render_Vulkan, "Loading PL disk shader cache for title {:016X}", title_id);

    pl_cache.SetFilePath(GetPLFile(title_id, false));

    if (!pl_cache.SwitchMode(CacheFile::CacheOpMode::READ)) {
        LOG_INFO(Render_Vulkan, "Missing PL disk shader cache for title {:016X}", title_id);
        cleanup_on_error();
        return false;
    }

    u32 tot_entries = pl_cache.GetTotalEntries();
    auto curr = pl_cache.ReadFirst();
    if (!curr.Valid() || curr.Type() != CacheEntryType::FILE_INFO) {
        MALFORMED_DISK_CACHE;
    }

    const FileInfoEntry* file_info = curr.Payload<FileInfoEntry>();
    if (!file_info || file_info->cache_magic != FileInfoEntry::CACHE_FILE_MAGIC ||
        file_info->file_version != FileInfoEntry::CACHE_FILE_VERSION ||
        file_info->file_type != CacheFileType::PL_CACHE) {
        MALFORMED_DISK_CACHE;
    }

    if (file_info->config_struct_hash != StaticPipelineInfo::StructHash()) {
        LOG_ERROR(Render_Vulkan,
                  "Cache was created for a different StaticPipelineInfo, resetting...");
        cleanup_on_error();
        return false;
    }

    size_t current_callback_index = 0;
    size_t tot_callback_index = tot_entries - 1;
    size_t pipelines_queued = 0, pipelines_duplicate = 0, pipelines_rejected = 0;

    // There is only one entry type in the pipeline info cache,
    // no need to keep track of anything.
    for (int i = 1; i < tot_entries; i++) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        curr = pl_cache.ReadNext(curr);

        if (!curr.Valid()) {
            MALFORMED_DISK_CACHE;
        }

        LOG_DEBUG(Render_Vulkan, "Processing ID: {:016X} (type {})", curr.Id(), curr.Type());

        if (curr.Type() == CacheEntryType::PL_CONFIG) {

            const PLConfigEntry* entry;

            if (!(entry = curr.Payload<PLConfigEntry>()) ||
                entry->version != PLConfigEntry::EXPECTED_VERSION) {
                MALFORMED_DISK_CACHE;
            }

            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                         tot_callback_index, "Pipeline list");
            }

            known_graphic_pipelines.emplace(curr.Id());

            // Older entries hold stale data in unused binding slots, which made
            // copies of the same pipeline look different. Clear it so they dedupe.
            StaticPipelineInfo pl_info = entry->pl_info;
            for (u32 b = pl_info.vertex_layout.binding_count; b < MAX_VERTEX_BINDINGS; b++) {
                pl_info.vertex_layout.bindings[b] = {};
            }
            auto pl_hash_opt = pl_info.OptimizedHash(parent.instance);

            if (graphics_pipelines.find(pl_hash_opt) != graphics_pipelines.end()) {
                // Multiple entries can have the same optimized hash. Skip if that's the case.
                pipelines_duplicate++;
                LOG_DEBUG(Render_Vulkan, "    skipping.", curr.Id());
                continue;
            }

            // Fetch all the shaders used in the pipeline,
            // if any is missing we cannot build it.
            std::array<Shader*, MAX_SHADER_STAGES> shaders;

            LOG_DEBUG(Render_Vulkan, "    uses VS: {:016X}, FS: {:016X}, GS: {:016X}",
                      pl_info.shader_ids[ProgramType::VS],
                      pl_info.shader_ids[ProgramType::FS],
                      pl_info.shader_ids[ProgramType::GS]);

            if (pl_info.shader_ids[ProgramType::VS]) {
                auto it_vs =
                    programmable_vertex_map.find(pl_info.shader_ids[ProgramType::VS]);
                if (it_vs == programmable_vertex_map.end()) {
                    LOG_ERROR(Render_Vulkan, "Missing vertex shader {:016X} for pipeline {:016X}",
                              pl_info.shader_ids[ProgramType::VS], curr.Id());
                    pipelines_rejected++;
                    continue;
                }
                shaders[ProgramType::VS] = it_vs->second;
            } else {
                shaders[ProgramType::VS] = &parent.trivial_vertex_shader;
            }

            auto it_fs = fragment_shaders.find(pl_info.shader_ids[ProgramType::FS]);
            if (it_fs == fragment_shaders.end()) {
                LOG_ERROR(Render_Vulkan, "Missing fragment shader {:016X} for pipeline {:016X}",
                          pl_info.shader_ids[ProgramType::FS], curr.Id());
                pipelines_rejected++;
                continue;
            }
            shaders[ProgramType::FS] = &it_fs->second;

            if (parent.instance.UseGeometryShaders() &&
                !parent.instance.IsFragmentShaderBarycentricSupported() &&
                pl_info.shader_ids[ProgramType::GS]) {
                auto it_gs =
                    fixed_geometry_shaders.find(pl_info.shader_ids[ProgramType::GS]);
                if (it_gs == fixed_geometry_shaders.end()) {
                    LOG_ERROR(Render_Vulkan, "Missing geometry shader {:016X} for pipeline {:016X}",
                              pl_info.shader_ids[ProgramType::GS], curr.Id());
                    pipelines_rejected++;
                    continue;
                }
                shaders[ProgramType::GS] = &it_gs->second;
            } else {
                shaders[ProgramType::GS] = nullptr;
            }

            // Build the pipeline using the cached pipeline info.
            // The dynamic state can be left default initialized.
            PipelineInfo info{};
            info.state = pl_info;

            auto [it_pl, _] = graphics_pipelines.try_emplace(pl_hash_opt);
            it_pl.value() = std::make_unique<GraphicsPipeline>(
                parent.instance, parent.renderpass_cache, info, *parent.driver_pipeline_cache,
                *parent.pipeline_layout, shaders, &parent.pipeline_workers);

            // Build on the worker pool instead of this thread: the driver cache makes
            // each build short, but there are thousands, and they run again after
            // every save state load because loading recreates the renderer.
            it_pl.value()->TryBuild(false, false);

            LOG_DEBUG(Render_Vulkan, "    queued.");
            pipelines_queued++;

        } else {
            MALFORMED_DISK_CACHE;
        }
    }

    // The game starts with every known pipeline ready, as when they were built inline.
    // With a large cache and nothing in the driver's shader cache yet (a first
    // start, or a cache from another device) this takes minutes, so report the
    // builds as they finish.
    if (callback && !graphics_pipelines.empty()) {
        std::vector<GraphicsPipeline*> queued;
        queued.reserve(graphics_pipelines.size());
        for (const auto& [hash, pipeline] : graphics_pipelines) {
            queued.push_back(pipeline.get());
        }
        size_t last_done = SIZE_MAX;
        auto last_change = std::chrono::steady_clock::now();
        while (!stop_loading) {
            const size_t done = std::count_if(queued.begin(), queued.end(),
                                              [](GraphicsPipeline* p) { return p->IsDone(); });
            callback(VideoCore::LoadCallbackStage::Build, done, queued.size(), "Pipeline");
            const auto now = std::chrono::steady_clock::now();
            if (done != last_done) {
                last_done = done;
                last_change = now;
            }
            // Pipelines whose shaders were not ready are not queued and never
            // finish here; WaitForRequests below is the real barrier.
            if (done == queued.size() || now - last_change > std::chrono::seconds(10)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    parent.pipeline_workers.WaitForRequests();

    LOG_INFO(Render_Vulkan,
             "PL disk cache: {} entries, {} pipelines built, {} duplicates, {} rejected "
             "(missing shader)",
             tot_entries - 1, pipelines_queued, pipelines_duplicate, pipelines_rejected);

    // Switch to append mode to receive new entries.
    return pl_cache.SwitchMode(CacheFile::CacheOpMode::APPEND);
}

void ShaderDiskCache::AppendVSConfigProgram(CacheFile& file,
                                            const Pica::Shader::Generator::PicaVSConfig& config,
                                            const Pica::ShaderSetup& setup, u64 config_id,
                                            u64 spirv_id) {

    VSConfigEntry entry;
    entry.version = VSConfigEntry::EXPECTED_VERSION;
    entry.vs_config = config;
    entry.spirv_entry_id = spirv_id;
    entry.program_entry_id =
        Common::HashCombine(config.state.program_hash, config.state.swizzle_hash);

    bool new_entry = known_vertex_programs.emplace(entry.program_entry_id).second;
    if (new_entry) {
        std::unique_ptr<VSProgramEntry> prog_entry = std::make_unique<VSProgramEntry>();
        prog_entry->version = VSProgramEntry::EXPECTED_VERSION;
        prog_entry->program_len = setup.GetBiggestProgramSize();
        prog_entry->program_code = setup.GetProgramCode();
        prog_entry->swizzle_len = setup.GetBiggestSwizzleSize();
        prog_entry->swizzle_code = setup.GetSwizzleData();

        AppendVSProgram(file, *prog_entry, entry.program_entry_id);
    }

    AppendVSConfig(file, entry, config_id);
}

void ShaderDiskCache::AppendVSProgram(CacheFile& file, const VSProgramEntry& entry,
                                      u64 program_id) {
    file.Append(CacheEntryType::VS_PROGRAM, program_id, entry, true);
}

void ShaderDiskCache::AppendVSConfig(CacheFile& file, const VSConfigEntry& entry, u64 config_id) {
    file.Append(CacheEntryType::VS_CONFIG, config_id, entry, true);
}

void ShaderDiskCache::AppendVSSPIRV(CacheFile& file, std::span<const u32> program, u64 program_id) {
    file.Append(CacheEntryType::VS_SPIRV, program_id,
                {reinterpret_cast<const u8*>(program.data()), program.size() * sizeof(u32)}, true);
}

void ShaderDiskCache::AppendFSConfig(CacheFile& file, const FSConfigEntry& entry, u64 config_id) {
    file.Append(CacheEntryType::FS_CONFIG, config_id, entry, true);
}

void ShaderDiskCache::AppendFSSPIRV(CacheFile& file, std::span<const u32> program, u64 program_id) {
    file.Append(CacheEntryType::FS_SPIRV, program_id,
                {reinterpret_cast<const u8*>(program.data()), program.size() * sizeof(u32)}, true);
}

void ShaderDiskCache::AppendGSConfig(CacheFile& file, const GSConfigEntry& entry, u64 config_id) {
    file.Append(CacheEntryType::GS_CONFIG, config_id, entry, true);
}

void ShaderDiskCache::AppendGSSPIRV(CacheFile& file, std::span<const u32> program, u64 program_id) {
    file.Append(CacheEntryType::GS_SPIRV, program_id,
                {reinterpret_cast<const u8*>(program.data()), program.size() * sizeof(u32)}, true);
}

void ShaderDiskCache::AppendPLConfig(CacheFile& file, const PLConfigEntry& entry, u64 config_id) {
    file.Append(CacheEntryType::PL_CONFIG, config_id, entry, true);
}

} // namespace Vulkan
