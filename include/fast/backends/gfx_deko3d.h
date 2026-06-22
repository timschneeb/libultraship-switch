#pragma once

#if defined(ENABLE_DEKO3D)
#include "gfx_rendering_api.h"
#include "gfx_deko3d_window.h"

namespace Fast {
struct ShaderProgramDeko3d {
    std::uint64_t ShaderId1 = 0;
    std::uint64_t ShaderId2 = 0;
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
};
} // namespace Fast
#endif