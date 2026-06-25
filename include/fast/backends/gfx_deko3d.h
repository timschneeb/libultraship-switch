#pragma once

#if defined(ENABLE_DEKO3D)
#include "gfx_rendering_api.h"
#include "gfx_deko3d_window.h"

#include <map>
#include <utility>

namespace Fast {
struct ShaderProgramDeko3d {
    std::uint64_t ShaderId1 = 0;
    std::uint64_t ShaderId2 = 0;

    // Decoded packing-affecting flags (from gfx_cc_get_features), used to locate per-vertex attributes in mBufVbo in
    // the interpreter's packing order.  Mirrors what GfxRenderingAPIOGL bakes into its per-program attrib table.
    bool OptFog = false;
    bool OptGrayscale = false;
    bool OptAlpha = false;
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
    GfxWindowBackendDeko3d* mWindowBackend = nullptr;
    FilteringMode mTextureFilter = FILTER_THREE_POINT;

    static constexpr std::uint8_t sVtxRing = GfxWindowBackendDeko3d::sFramebuffers; // Must be same swap chain depth
    // Per-(id0,id1) program pool, mirroring GfxRenderingAPIOGL's contract: LookupShader misses return nullptr so the
    // interpreter calls CreateAndLoadNewShader, and we decode CCFeatures once.std::map avoids pulling in a pair
    // hasher; lookups are already cached in comb->prg[tm].
    std::map<std::pair<std::uint64_t, std::uint64_t>, ShaderProgramDeko3d> mShaderProgramPool = {};
    ShaderProgramDeko3d* mCurrentProgram = nullptr; // Set by LoadShader; the batch being drawn
    dk::CmdBuf mFrameCmdBuf = {};                   // Borrowed frame cmdbuf (set in StartFrame)
    std::uint32_t mRing = 0;                        // Current ring slot

    bool mUseAlpha = false; // From SetUseAlpha: selects the vec3/vec4 input stride
    bool mDepthTest = false;
    bool mDepthMask = false;
    bool mDecal = false;

    std::array<dk::UniqueMemBlock, sVtxRing> mVtxRingMemBlock = {};
    std::array<DkGpuAddr, sVtxRing> mVtxRingGpu = {};
    std::array<std::uint8_t*, sVtxRing> mVtxRingCpu = {};
    std::array<std::uint32_t, sVtxRing> mVtxRingOffset = {};

#if defined(DEKO3D_VARIANT_SURVEY)
    std::set<std::string> mSeenVariants = {};
#endif
};
} // namespace Fast
#endif