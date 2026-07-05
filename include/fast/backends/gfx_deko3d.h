#pragma once

#if defined(ENABLE_DEKO3D)
#include <map>
#include <utility>

#include "gfx_rendering_api.h"
#include "gfx_deko3d_window.h"
#include "fast/interpreter.h"

struct ImDrawData; // ImGui draw payload; full type pulled into the .cpp only (keeps imgui.h out of this header).

namespace Fast {
struct ShaderProgramDeko3d {
    std::uint64_t ShaderId1 = 0;
    std::uint64_t ShaderId2 = 0;
    CCFeatures Cc = {};
};

/**
 * @brief One sampled texture: the struct own its device-local image and its sampler, keyed by the ID NewTexture()
 *        hands back.  deko3d has no implicit texture cache, so the image memory lives here for the texture's lifetime.
 */
struct TextureDeko3d {
    dk::UniqueMemBlock ImageMemBlock = {};
    dk::Image Image = {};
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    std::uint32_t SamplerIndex = 8; // Slot in the immutable sampler table; 8 == (nearest, Repeat, Repeat), the
                                    // interpreter's cache-insert defaults.
};

/**
 * @brief One render-target framebuffer.  The color surface lives in the rapi's shared texture table (TextureId is its
 *        mTextures slot == its image-descriptor slot), so GetFramebufferTextureId/SelectTextureFb resolve to a
 *        DkResHandle in the already-bound descriptor set -- same handle space as every other sampled texture.
 *        Depth is owned here.  ID 0 is the swap chain/window and owns no image (TextureId == -1); its color/depth come
 *        from the window backend's acquired slot.
 */
struct FramebufferDeko3d {
    std::int32_t TextureId = -1; // Color surface's slot in mTextures; -1 for the window (ID 0)
    dk::UniqueMemBlock DepthMemBlock = {};
    dk::Image DepthImage = {};
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    bool HasDepth = false;
    bool IsYInverted = false; // Carried from the interpreter; unused on deko3d
};

class GfxRenderingApiDeko3d final : public GfxRenderingAPI {
  public:
    explicit GfxRenderingApiDeko3d(GfxWindowBackendDeko3d* windowBackend);
    ~GfxRenderingApiDeko3d() override = default;

    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;

    // ----------------------------------------------------------------------------------------------------------------
    // Shaders
    // ----------------------------------------------------------------------------------------------------------------

    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    void ClearShaderCache() override;
    ShaderProgram* CreateAndLoadNewShader(std::uint64_t shaderId1, std::uint64_t shaderId2) override;
    ShaderProgram* LookupShader(std::uint64_t shaderId1, std::uint64_t shaderId2) override;
    void ShaderGetInfo(ShaderProgram* prg, std::uint8_t* numInputs, bool usedTextures[2]) override;

    // ----------------------------------------------------------------------------------------------------------------
    // Textures
    // ----------------------------------------------------------------------------------------------------------------

    std::uint32_t NewTexture() override;
    void SelectTexture(int tile, std::uint32_t textureId) override;
    void UploadTexture(const std::uint8_t* rgba32Buf, std::uint32_t width, std::uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linearFilter, std::uint32_t cms, std::uint32_t cmt) override;
    void DeleteTexture(std::uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    ImTextureID GetTextureById(int id) override;

    // ----------------------------------------------------------------------------------------------------------------
    // Fixed-function state
    // ----------------------------------------------------------------------------------------------------------------

    void SetDepthTestAndMask(bool depthTest, bool zUpd) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void SetSrgbMode() override;
    void SetCurrentPrimDepth(float depth) override;

    // ----------------------------------------------------------------------------------------------------------------
    // Draw
    // ----------------------------------------------------------------------------------------------------------------

    void DrawTriangles(float bufVbo[], std::size_t bufVboLen, std::size_t bufVboNumTris) override;

    // ----------------------------------------------------------------------------------------------------------------
    // ImGui renderer
    //
    // Not part of GfxRenderingAPI; called via a concrete cast from Fast3dGui.  Draws are appended to the same frame
    // command buffer as the game, after game draws, targeting fb 0 -- so ImGui composes the final backbuffer.
    // ----------------------------------------------------------------------------------------------------------------

    void RenderDrawData(ImDrawData* drawData);
    // (Re)builds the ImGui font atlas into a reserved rapi texture slot.  Deferred to NewFrame (never Init): the
    // Fast3dGui::Init handshake fires inside the window backend's Init, before rapi Init(), so the descriptor sets this
    // touches don't exist yet at that point.  Idempotent -- re-uploads only when SoH has dirtied the atlas by adding
    // fonts after Gui::Init().
    void EnsureImGuiFontsUploaded();

    // ----------------------------------------------------------------------------------------------------------------
    // Frame lifecycle
    // ----------------------------------------------------------------------------------------------------------------

    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override;

    // ----------------------------------------------------------------------------------------------------------------
    // Framebuffers
    // ----------------------------------------------------------------------------------------------------------------

    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fbId, std::uint32_t width, std::uint32_t height, std::uint32_t msaaLevel,
                                     bool openglInvertY, bool renderTarget, bool hasDepthBuffer,
                                     bool canExtractDepth) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ReadFramebufferToCPU(int fbId, std::uint32_t width, std::uint32_t height, std::uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, std::uint16_t, hash_pair_ff>
    GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;

  private:
    static constexpr std::uint8_t sVtxRing = GfxWindowBackendDeko3d::sFramebuffers;
    static constexpr std::uint32_t sUniformSlotSize = DK_UNIFORM_BUF_ALIGNMENT; // 0x100
    static constexpr std::uint32_t sUniformRingSize = 0x40000;
    static constexpr std::uint32_t sUploadCmdMemSize = 0x1000;
    static constexpr std::uint32_t sMaxTextures = 1024;
    static constexpr std::uint32_t sSamplerCount = 18; // 2 filters x 3 wrapS x 3 wrapT immutable samplers

    GfxWindowBackendDeko3d* mWindowBackend = nullptr;
    FilteringMode mTextureFilter = FILTER_THREE_POINT;

    // Texture table, indexed by the ID NewTexture() returns.  SelectTexture only records which ID is bound to which
    // tile; the GPU image is created in UploadTexture and referenced at draw time.
    std::vector<TextureDeko3d> mTextures = {};
    std::int32_t mCurrentTile = 0;
    std::array<std::int32_t, SHADER_MAX_TEXTURES> mCurrentTextureIds = {};

    // Transient upload path.  Record a single copyBufferToImage, submit on the borrowed queue, block on waitIdle.
    // Reused across uploads (clear() before each record), so we don't allocate command memory per texture.
    dk::CmdBuf mUploadCmdBuf = {};
    dk::UniqueMemBlock mUploadCmdMemBlock = {};

    // Texture descriptor sets.  deko3d references textures through descriptor tables, not a stateful bind point: each
    // texture's image + sampler descriptor live at slot == texture ID, combined per draw into a handle via
    // dkMakeTextureHandle.  The interpreter recycles texture IDs within TEXTURE_CACHE_MAX_SIZE (1024), so a fixed
    // 1024-slot pool covers every live texture.
    dk::UniqueMemBlock mImageDescMemBlock = {};
    dk::UniqueMemBlock mSamplerDescMemBlock = {};
    dk::ImageDescriptor* mImageDescriptors = nullptr;     // CPU view into the image descriptor set
    dk::SamplerDescriptor* mSamplerDescriptors = nullptr; // CPU view into the sampler descriptor set
    bool mIsDescriptorsDirty = false;                     // A descriptor changed since the last bindTextures
    std::int32_t mCurrentShaderTextured = -1; // Bound shader variant this frame: -1 none, 0 untextured, 1 textured

    // Per-(id0,id1) program pool: LookupShader misses return nullptr so the interpreter calls CreateAndLoadNewShader,
    // and we decode CCFeatures once.  std::map avoids pulling in a pair hasher; lookups are already cached in
    // comb->prg[tm].
    std::map<std::pair<std::uint64_t, std::uint64_t>, ShaderProgramDeko3d> mShaderProgramPool = {};
    ShaderProgramDeko3d* mCurrentProgram = nullptr;
    dk::CmdBuf mFrameCmdBuf = {};
    std::uint32_t mRing = 0;

    std::array<dk::UniqueMemBlock, sVtxRing> mUboRingMemBlock = {};
    std::array<DkGpuAddr, sVtxRing> mUboRingGpu = {};
    std::array<std::uint8_t*, sVtxRing> mUboRingCpu = {};
    std::array<std::uint32_t, sVtxRing> mUboRingOffset = {};

    bool mUseAlpha = false;
    bool mDepthTest = false;
    bool mDepthMask = false;
    bool mDecal = false;

    std::array<dk::UniqueMemBlock, sVtxRing> mVtxRingMemBlock = {};
    std::array<DkGpuAddr, sVtxRing> mVtxRingGpu = {};
    std::array<std::uint8_t*, sVtxRing> mVtxRingCpu = {};
    std::array<std::uint32_t, sVtxRing> mVtxRingOffset = {};

    // ImGui vertex/index streaming, per ring.  Indexed by mRing, whose command memory is gated by the frame fence in
    // BeginFrameRecording, so overwriting slot mRing here can't race an in-flight frame.  Grow-only; created lazily on
    // first use and resized (never shrunk) when a frame's draw data exceeds the current capacity.  The ortho UBO
    // reuses the fragment CombinerUbo ring.
    std::array<dk::UniqueMemBlock, sVtxRing> mImguiVtxMemBlock = {};
    std::array<dk::UniqueMemBlock, sVtxRing> mImguiIdxMemBlock = {};
    std::int32_t mImguiFontTexId = -1; // Reserved slot in mTextures for the font atlas (== its descriptor slot)

    // Framebuffer table.  Index == interpreter framebuffer ID; index 0 is the window/swap chain (no owned image).
    // CreateFramebuffer appends starting at ID 1.  Color surfaces live in mTextures (FramebufferDeko3d::TextureId).
    std::vector<FramebufferDeko3d> mFramebuffers = {};
    std::int32_t mCurrentFb = 0;

    // Dimensions of the render target bound by the last StartDrawToFramebuffer.  SetViewport/SetScissor flip Y and
    // clamp against these, not the window.
    std::uint32_t mRenderTargetWidth = 0;
    std::uint32_t mRenderTargetHeight = 0;
};
} // namespace Fast
#endif