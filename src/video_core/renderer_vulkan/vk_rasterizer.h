// Copyright 2022-2025 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once
#include "mm3d/glare_interpolation.h"
#ifdef ENABLE_MM3D_PRESENTATION
#include <map>
#include "mm3d/profiles.h"
#include "mm3d/atlas_render.h"
#include "mm3d/camera_midpoint.h"
#include "mm3d/link_interpolation.h"
#include "mm3d/ripple_interpolation.h"
#include "mm3d/forest_particle_interpolation.h"
#include "mm3d/splash_interpolation.h"
#include "mm3d/song_effect_interpolation.h"
#include "mm3d/pause_capture.h"
#include "mm3d/visibility_depth.h"
#include "video_core/renderer_vulkan/vk_visibility_depth.h"
#endif

#include "video_core/rasterizer_accelerated.h"
#include "video_core/renderer_vulkan/vk_descriptor_update_queue.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_render_manager.h"
#include "video_core/renderer_vulkan/vk_simulator_workarounds.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"
#include "video_core/renderer_vulkan/vk_texture_runtime.h"

namespace Frontend {
class EmuWindow;
}

namespace MM3D { class PromptLayer; }

namespace VideoCore {
class CustomTexManager;
class RendererBase;
} // namespace VideoCore

namespace Pica {
struct DisplayTransferConfig;
struct MemoryFillConfig;
struct FramebufferConfig;
} // namespace Pica

namespace Vulkan {

struct ScreenInfo;

class Instance;
class Scheduler;
class RenderManager;

class RasterizerVulkan : public VideoCore::RasterizerAccelerated
#ifdef ENABLE_MM3D_PRESENTATION
    , public MM3D::PauseCapture::Backend, public MM3D::VisibilityDepth::Backend
#endif
{
public:
    explicit RasterizerVulkan(Memory::MemorySystem& memory, Pica::PicaCore& pica,
                              VideoCore::CustomTexManager& custom_tex_manager,
                              VideoCore::RendererBase& renderer, Frontend::EmuWindow& emu_window,
                              const Instance& instance, Scheduler& scheduler,
                              RenderManager& renderpass_cache, DescriptorUpdateQueue& update_queue,
                              u32 image_count);
    ~RasterizerVulkan() override;

    void TickFrame();
#ifdef ENABLE_MM3D_PRESENTATION
    void EndGuestFrame() override;
    const auto& PromptOverlays() const { return prompt_layers; }
    MM3D::CameraMidpoint::PresentationStatus MidpointStatus(u64 completed) const {
        using namespace MM3D::CameraMidpoint;
        const auto status=EyeStatus(completed,midpoint_target_frame,midpoint_frame_interpolated,
            midpoint_hud_frame,midpoint_hud_failed,midpoint_scene_clear);
        // A right eye captured in the same frame must be ready too.
        if(status!=PresentationStatus::Active || !RightEyeCaptured())return status;
        const auto& r=midpoint_other_eye;
        return EyeStatus(completed,r.target_frame,r.frame_interpolated,r.hud_frame,r.hud_failed,
            r.scene_clear);
    }
    /// The right eye's midpoint layer ("@camera_midpoint" or "@camera_midpoint_hud")
    /// when the right eye was captured in the same frame as the left; else null.
    const MM3D::PromptLayer* MidpointRightEyeLayer(std::string_view id) const;
    std::optional<u32> ReadVisibilityDepth(u32 address, const MM3D::VisibilityDepth::Context& context) override;
    void ResetVisibilityDepth() override;
    void CapturePause(u32 source,unsigned eye) override;
    void BindPause(u32 texture,unsigned eye,const MM3D::PauseCapture::Probes& probes) override;
    void ResetPause() override;
#endif
    void LoadDefaultDiskResources(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) override;

    void DrawTriangles() override;
    void FlushAll() override;
    void FlushRegion(PAddr addr, u32 size) override;
    void InvalidateRegion(PAddr addr, u32 size) override;
    void FlushAndInvalidateRegion(PAddr addr, u32 size) override;
    void ClearAll(bool flush) override;
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateFill(const Pica::MemoryFillConfig& config) override;
    bool AccelerateDisplay(const Pica::FramebufferConfig& config, PAddr framebuffer_addr,
                           u32 pixel_stride, ScreenInfo& screen_info);
    bool AccelerateDrawBatch(bool is_indexed) override;

    /// Switches the disk resources to the specified title
    void SwitchDiskResources(u64 title_id) override;
    void SaveDiskResources() override;

private:
#ifdef ENABLE_MM3D_PRESENTATION
    std::map<std::string,std::unique_ptr<MM3D::PromptLayer>> prompt_layers;
    std::vector<MM3D::Profiles::Profile> mm3d_draw_profiles;
    bool panel_excluded = false;
    std::vector<MM3D::Profiles::Rule> panel_draw_identities;
    MM3D::Atlas::Uniforms atlas_uniforms{};
    MM3D::CameraMidpoint::History midpoint_history;
    // CPU replay uploads into the stream buffer before deferred commands are
    // recorded; those commands never retain pointers into this scratch storage.
    std::vector<HardwareVertex> midpoint_cpu_vertices;
    MM3D::LinkInterpolation::History link_history;
    MM3D::SongEffectInterpolation::History song_history;
    std::optional<MM3D::SongEffectInterpolation::Values> song_overrides;
    MM3D::LinkInterpolation::Overrides link_overrides{};
    std::vector<MM3D::LinkPoseCapture::LivePose> link_poses;
    std::optional<u64> link_frame;
    unsigned link_generation{};
    std::optional<MM3D::CameraMidpoint::Matrix> midpoint_projection;
    unsigned midpoint_generation{};
    unsigned midpoint_width{}, midpoint_height{};
    unsigned midpoint_copies{}, midpoint_rejected{}, midpoint_fallback{};
    std::optional<u64> midpoint_target_frame, midpoint_fade_frame, midpoint_hud_frame, midpoint_title_depth_frame;
    std::optional<u64> midpoint_world_frame;
    std::optional<u64> midpoint_opaque_world_frame;
    std::optional<u64> midpoint_stencil_frame;
    bool midpoint_hud_failed{}, midpoint_frame_interpolated{};
    std::array<u64,6> midpoint_rejection_logged{};
    std::array<u64,2> midpoint_transfer_logged{};
    u64 glare_trace_session{}, glare_trace_frame{};
    unsigned glare_trace_samples{};
    bool visibility_depth_enabled{};
    bool visibility_depth_refresh_enabled{};
    std::unique_ptr<VisibilityDepthCache> visibility_depth;
    MM3D::GlareInterpolation glare_history;
    MM3D::RippleInterpolation::History ripple_history;
    MM3D::ForestParticleInterpolation::History forest_particle_history;
    MM3D::SplashInterpolation::History splash_history;
    std::optional<std::array<u32,3>> splash_offsets;
    bool splash_blend{};
    std::optional<std::array<u32,2>> ripple_offsets;
    std::optional<u32> glare_vertex_offset;
    MM3D::CameraMidpoint::SceneCopies midpoint_scene_copies;
    MM3D::CameraMidpoint::SceneClearPair midpoint_scene_clear;
    s64 midpoint_last_draw_ms{};

    // Side-by-side stereo: the game draws the left eye, copies it to the left
    // display buffer, then draws the right eye into the same target. The
    // midpoint members above belong to the eye being drawn; the other eye's
    // are kept here and swapped in when the eye changes (SetMidpointEye).
    static constexpr std::array<const char*,3> MidpointLayerIds{
        "@camera_midpoint","@camera_midpoint_fade","@camera_midpoint_hud"};
    struct MidpointEyeState {
        MM3D::CameraMidpoint::History history;
        MM3D::ForestParticleInterpolation::History forest_particle_history;
        MM3D::LinkInterpolation::History link_history;
        MM3D::SongEffectInterpolation::History song_history;
        std::optional<MM3D::SongEffectInterpolation::Values> song_overrides;
        MM3D::LinkInterpolation::Overrides link_overrides{};
        std::vector<MM3D::LinkPoseCapture::LivePose> link_poses;
        std::optional<u64> link_frame;
        unsigned link_generation{};
        unsigned generation{};
        unsigned width{}, height{};
        std::optional<u64> target_frame, fade_frame, hud_frame, title_depth_frame;
        std::optional<u64> world_frame, opaque_world_frame, stencil_frame;
        bool hud_failed{}, frame_interpolated{};
        MM3D::GlareInterpolation glare_history;
        MM3D::RippleInterpolation::History ripple_history;
        MM3D::SplashInterpolation::History splash_history;
        MM3D::CameraMidpoint::SceneClearPair scene_clear;
        s64 last_draw_ms{};
        std::array<std::unique_ptr<MM3D::PromptLayer>,MidpointLayerIds.size()> layers;
    };
    MidpointEyeState midpoint_other_eye;
    unsigned midpoint_eye{};
    void SetMidpointEye(unsigned eye);
    void SwapMidpointEye();
    /// Clears the inactive eye's midpoint state, keeping its layers hidden.
    void ResetMidpointOtherEye();
    bool RightEyeCaptured() const {
        return midpoint_eye==0 && midpoint_other_eye.target_frame &&
            midpoint_other_eye.target_frame==midpoint_target_frame;
    }
    static MM3D::CameraMidpoint::PresentationStatus EyeStatus(u64 completed,
        const std::optional<u64>& target_frame, bool frame_interpolated,
        const std::optional<u64>& hud_frame, bool hud_failed,
        const MM3D::CameraMidpoint::SceneClearPair& scene_clear) {
        using namespace MM3D::CameraMidpoint;
        if(!target_frame)return PresentationStatus::WaitingScene;
        if(!IsCompletedCapture(target_frame,completed))return PresentationStatus::FrameMismatch;
        if(!frame_interpolated)return PresentationStatus::NoMidpoint;
        if(hud_frame!=target_frame)return PresentationStatus::NoHud;
        if(hud_failed || scene_clear.Pending(*target_frame))
            return PresentationStatus::UnsupportedHud;
        return PresentationStatus::Active;
    }
#endif
    /// Syncs pipeline state from PICA registers
    void SyncDrawState();

    /// Syncs and uploads the lighting, fog and proctex LUTs
    void SyncAndUploadLUTs();
    void SyncAndUploadLUTsLF();

    /// Syncs all enabled PICA texture units
    void SyncTextureUnits(const Framebuffer* framebuffer);

    /// Syncs all utility textures in the fragment shader.
    void SyncUtilityTextures(const Framebuffer* framebuffer);

    /// Binds the PICA shadow cube required for shadow mapping
    void BindShadowCube(const Pica::TexturingRegs::FullTextureConfig& texture,
                        vk::DescriptorSet texture_set);

    /// Binds a texture cube to texture unit 0
    void BindTextureCube(const Pica::TexturingRegs::FullTextureConfig& texture,
                         vk::DescriptorSet texture_set);

    /// Upload the uniform blocks to the uniform buffer object
    void UploadUniforms(bool accelerate_draw);

    /// Generic draw function for DrawTriangles and AccelerateDrawBatch
    bool Draw(bool accelerate, bool is_indexed);

    /// Internal implementation for AccelerateDrawBatch
    bool AccelerateDrawBatchInternal(bool is_indexed);

    /// Setup index array for AccelerateDrawBatch
    void SetupIndexArray();

    /// Setup vertex array for AccelerateDrawBatch
    void SetupVertexArray();

    /// Setup the fixed attribute emulation in vulkan
    void SetupFixedAttribs();

    /// Setup vertex shader for AccelerateDrawBatch
    bool SetupVertexShader();

    /// Setup geometry shader for AccelerateDrawBatch
    bool SetupGeometryShader();

    /// Creates the vertex layout struct used for software shader pipelines
    void MakeSoftwareVertexLayout();

private:
    const Instance& instance;
    Scheduler& scheduler;
    RenderManager& renderpass_cache;
    DescriptorUpdateQueue& update_queue;
    PipelineCache pipeline_cache;
    TextureRuntime runtime;
    RasterizerCache res_cache;
#ifdef ENABLE_MM3D_PRESENTATION
    std::array<std::unique_ptr<Surface>,2> pause_images;
    std::array<u32,2> pause_textures{};
    std::array<unsigned,2> pause_orientation{};
    std::array<int,4> pause_draw{};
#endif

    VertexLayout software_layout;
    std::array<u32, 16> binding_offsets{};
    std::array<bool, 16> enable_attributes{};
    std::array<vk::Buffer, 16> vertex_buffers;
    u32 replay_index_offset{};
    vk::IndexType replay_index_type{};
    VertexArrayInfo vertex_info;
    PipelineInfo pipeline_info{};

    StreamBuffer stream_buffer;     ///< Vertex+Index buffer
    StreamBuffer uniform_buffer;    ///< Uniform buffer
    StreamBuffer texture_buffer;    ///< Texture buffer
    StreamBuffer texture_lf_buffer; ///< Texture Light-Fog buffer
#ifdef CITRA_PRIVATE_TEXEL_BUFFERS
    PrivateTexelBuffer texture_private;    ///< What draws read in the simulator
    PrivateTexelBuffer texture_lf_private; ///< What draws read in the simulator
#endif
    vk::UniqueBufferView texture_lf_view;
    vk::UniqueBufferView texture_rg_view;
    vk::UniqueBufferView texture_rgba_view;
    vk::DeviceSize uniform_buffer_alignment;
    u32 uniform_size_aligned_vs_pica;
    u32 uniform_size_aligned_vs;
    u32 uniform_size_aligned_fs;
    bool async_shaders{false};
};

} // namespace Vulkan
