#if defined(ENABLE_DEKO3D)
#include "fast/backends/gfx_deko.h"

namespace Fast {
GfxRenderingAPIDeko::GfxRenderingAPIDeko(GfxWindowBackendDeko* windowBackend) : mWindowBackend(windowBackend) {
}

const char* GfxRenderingAPIDeko::GetName() {
    return "deko3d";
}

int GfxRenderingAPIDeko::GetMaxTextureSize() {
    return 16384; // Maxwell (TX1) max 2D texture dimension.
}

GfxClipParameters GfxRenderingAPIDeko::GetClipParameters() {
    // deko3d uses 0..1 depth instead of -1..1.
    return { /* z_is_from_0_to_1 */ true, /* invertY */ false };
}

// --------------------------------------------------------------------------------------------------------------------
// Shaders
// --------------------------------------------------------------------------------------------------------------------

void GfxRenderingAPIDeko::UnloadShader(ShaderProgram* oldPrg) {
}

void GfxRenderingAPIDeko::LoadShader(ShaderProgram* newPrg) {
}

void GfxRenderingAPIDeko::ClearShaderCache() {
}

ShaderProgram* GfxRenderingAPIDeko::CreateAndLoadNewShader(std::uint64_t shaderId1, std::uint64_t shaderId2) {
    return nullptr;
}

ShaderProgram* GfxRenderingAPIDeko::LookupShader(std::uint64_t shaderId1, std::uint64_t shaderId2) {
    return nullptr;
}

void GfxRenderingAPIDeko::ShaderGetInfo(ShaderProgram* prg, std::uint8_t* numInputs, bool usedTextures[2]) {
    if (numInputs) {
        *numInputs = 0;
    }

    if (usedTextures) {
        usedTextures[0] = false;
        usedTextures[1] = false;
    }
}

// --------------------------------------------------------------------------------------------------------------------
// Textures
// --------------------------------------------------------------------------------------------------------------------

std::uint32_t GfxRenderingAPIDeko::NewTexture() {
    return 0;
}

void GfxRenderingAPIDeko::SelectTexture(int tile, std::uint32_t textureId) {
}

void GfxRenderingAPIDeko::UploadTexture(const std::uint8_t* rgba32Buf, std::uint32_t width, std::uint32_t height) {
}

void GfxRenderingAPIDeko::SetSamplerParameters(int sampler, bool linearFilter, std::uint32_t cms, std::uint32_t cmt) {
}

void GfxRenderingAPIDeko::DeleteTexture(std::uint32_t texId) {
}

void GfxRenderingAPIDeko::SetTextureFilter(FilteringMode mode) {
    mTextureFilter = mode;
}

FilteringMode GfxRenderingAPIDeko::GetTextureFilter() {
    return mTextureFilter;
}

ImTextureID GfxRenderingAPIDeko::GetTextureById(int id) {
    return nullptr;
}

// --------------------------------------------------------------------------------------------------------------------
// Fixed-function state
// --------------------------------------------------------------------------------------------------------------------

void GfxRenderingAPIDeko::SetDepthTestAndMask(bool depthTest, bool zUpd) {
}

void GfxRenderingAPIDeko::SetZmodeDecal(bool decal) {
}

void GfxRenderingAPIDeko::SetViewport(int x, int y, int width, int height) {
}

void GfxRenderingAPIDeko::SetScissor(int x, int y, int width, int height) {
}

void GfxRenderingAPIDeko::SetUseAlpha(bool useAlpha) {
}

void GfxRenderingAPIDeko::SetSrgbMode() {
}

void GfxRenderingAPIDeko::SetCurrentPrimDepth(float depth) {
}

// --------------------------------------------------------------------------------------------------------------------
// Draw
// --------------------------------------------------------------------------------------------------------------------

void GfxRenderingAPIDeko::DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) {
}

// --------------------------------------------------------------------------------------------------------------------
// Frame lifecycle
// --------------------------------------------------------------------------------------------------------------------

void GfxRenderingAPIDeko::Init() {
}

void GfxRenderingAPIDeko::OnResize() {
}

void GfxRenderingAPIDeko::StartFrame() {
}

void GfxRenderingAPIDeko::EndFrame() {
}

void GfxRenderingAPIDeko::FinishRender() {
}

// --------------------------------------------------------------------------------------------------------------------
// Framebuffers
// --------------------------------------------------------------------------------------------------------------------

int GfxRenderingAPIDeko::CreateFramebuffer() {
    return 0;
}

void GfxRenderingAPIDeko::UpdateFramebufferParameters(int fbId, std::uint32_t width, std::uint32_t height,
                                                      std::uint32_t msaaLevel, bool openglInvertY, bool renderTarget,
                                                      bool hasDepthBuffer, bool canExtractDepth) {
}

void GfxRenderingAPIDeko::StartDrawToFramebuffer(int fbId, float noiseScale) {
}

void GfxRenderingAPIDeko::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                          int dstX0, int dstY0, int dstX1, int dstY1) {
}

void GfxRenderingAPIDeko::ClearFramebuffer(bool color, bool depth) {
}

void GfxRenderingAPIDeko::ReadFramebufferToCPU(int fbId, std::uint32_t width, std::uint32_t height,
                                               std::uint16_t* rgba16Buf) {
}

void GfxRenderingAPIDeko::ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) {
}

std::unordered_map<std::pair<float, float>, std::uint16_t, hash_pair_ff>
GfxRenderingAPIDeko::GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) {
    // Real depth readback isn't implemented yet, but Interpreter::GetPixelDepth() does find(coord)->second on this
    // result with no end() check.  An empty map makes that dereference end().  Return a defined placeholder for every
    // requested coordinate so the lookup resolves.  Replaced by a true readback with the real backend.
    std::unordered_map<std::pair<float, float>, std::uint16_t, hash_pair_ff> result;
    for (const auto& coord : coordinates) {
        result.emplace(coord, 0); // 0 reads as near; 0xFFFF reads as empty/far.
    }

    return result;
}

void* GfxRenderingAPIDeko::GetFramebufferTextureId(int fbId) {
    return nullptr;
}

void GfxRenderingAPIDeko::SelectTextureFb(int fbId) {
}
} // namespace Fast
#endif