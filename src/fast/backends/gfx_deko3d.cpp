#if defined(ENABLE_DEKO3D)
#include "fast/backends/gfx_deko3d.h"
#include "fast/interpreter.h"

namespace {
constexpr std::uint32_t gVtxRingSize = 0x800000; // 8 MiB per slot

constexpr std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
    return value + alignment - 1 & ~(alignment - 1);
}

// -----------

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t gDrawNs = 0;
std::int64_t gMemCpyNs = 0;
std::int32_t gDrawCalls = 0;
std::int32_t gDrawFrames = 0;
} // namespace

namespace Fast {
GfxRenderingApiDeko3d::GfxRenderingApiDeko3d(GfxWindowBackendDeko3d* windowBackend) : mWindowBackend(windowBackend) {
}

const char* GfxRenderingApiDeko3d::GetName() {
    return "deko3d";
}

int GfxRenderingApiDeko3d::GetMaxTextureSize() {
    return 16384; // Maxwell (TX1) max 2D texture dimension.
}

GfxClipParameters GfxRenderingApiDeko3d::GetClipParameters() {
    // deko3d uses 0..1 depth instead of -1..1.
    return { /* z_is_from_0_to_1 */ true, /* invertY */ false };
}

// --------------------------------------------------------------------------------------------------------------------
// Shaders
// --------------------------------------------------------------------------------------------------------------------

void GfxRenderingApiDeko3d::UnloadShader(ShaderProgram* oldPrg) {
}

void GfxRenderingApiDeko3d::LoadShader(ShaderProgram* newPrg) {
}

void GfxRenderingApiDeko3d::ClearShaderCache() {
}

ShaderProgram* GfxRenderingApiDeko3d::CreateAndLoadNewShader(std::uint64_t shaderId1, std::uint64_t shaderId2) {
    mProgram.ShaderId1 = shaderId1;
    mProgram.ShaderId2 = shaderId2;
    return reinterpret_cast<ShaderProgram*>(&mProgram);
}

ShaderProgram* GfxRenderingApiDeko3d::LookupShader(std::uint64_t shaderId1, std::uint64_t shaderId2) {
#if defined(DEKO3D_VARIANT_SURVEY)
    CCFeatures cc = {};
    gfx_cc_get_features(shaderId1, shaderId2, &cc);

    char axes[96];
    std::snprintf(axes, sizeof(axes), "tex=%d%d alpha=%d 2cyc=%d fog=%d gray=%d inputs=%d clamp=%d%d%d%d",
                  cc.usedTextures[0], cc.usedTextures[1], cc.opt_alpha, cc.opt_2cyc, cc.opt_fog, cc.opt_grayscale,
                  cc.numInputs, cc.clamp[0][0], cc.clamp[0][1], cc.clamp[1][0], cc.clamp[1][1]);

    if (mSeenVariants.insert(axes).second) { // First sighting of this layout variant
        char line[192];
        std::snprintf(line, sizeof(line), "[deko-variant #%zu] %s firstId=%016llx_%016llx", mSeenVariants.size(), axes,
                      static_cast<unsigned long long>(shaderId1), static_cast<unsigned long long>(shaderId2));
        mWindowBackend->Trace(line); // Crash-sync sink -> logs/deko3d_trace.log
    }
#endif

    return reinterpret_cast<ShaderProgram*>(&mProgram); // Single variant -> always found
}

void GfxRenderingApiDeko3d::ShaderGetInfo(ShaderProgram* prg, std::uint8_t* numInputs, bool usedTextures[2]) {
    if (numInputs) {
        *numInputs = 1; // Pack exactly one input...
    }

    if (usedTextures) {
        usedTextures[0] = false; // ...and no texcoords, so the input lands at stride-(3 + alpha).
        usedTextures[1] = false;
    }
}

// --------------------------------------------------------------------------------------------------------------------
// Textures
// --------------------------------------------------------------------------------------------------------------------

std::uint32_t GfxRenderingApiDeko3d::NewTexture() {
    return 0;
}

void GfxRenderingApiDeko3d::SelectTexture(int tile, std::uint32_t textureId) {
}

void GfxRenderingApiDeko3d::UploadTexture(const std::uint8_t* rgba32Buf, std::uint32_t width, std::uint32_t height) {
}

void GfxRenderingApiDeko3d::SetSamplerParameters(int sampler, bool linearFilter, std::uint32_t cms, std::uint32_t cmt) {
}

void GfxRenderingApiDeko3d::DeleteTexture(std::uint32_t texId) {
}

void GfxRenderingApiDeko3d::SetTextureFilter(FilteringMode mode) {
    mTextureFilter = mode;
}

FilteringMode GfxRenderingApiDeko3d::GetTextureFilter() {
    return mTextureFilter;
}

ImTextureID GfxRenderingApiDeko3d::GetTextureById(int id) {
    return nullptr;
}

// --------------------------------------------------------------------------------------------------------------------
// Fixed-function state
// --------------------------------------------------------------------------------------------------------------------

void GfxRenderingApiDeko3d::SetDepthTestAndMask(bool depthTest, bool zUpd) {
    mDepthTest = depthTest;
    mDepthMask = zUpd;
}

void GfxRenderingApiDeko3d::SetZmodeDecal(bool decal) {
    mDecal = decal;
}

void GfxRenderingApiDeko3d::SetViewport(int x, int y, int width, int height) {
    std::uint32_t fbHeight = 0;
    mWindowBackend->GetDimensions(nullptr, &fbHeight, nullptr, nullptr);

    auto cb = mFrameCmdBuf;
    cb.setViewports(0, { { static_cast<float>(x),
                           static_cast<float>(static_cast<int>(fbHeight) - y - height), // Top-left origin flip
                           static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f } });
}

void GfxRenderingApiDeko3d::SetScissor(int x, int y, int width, int height) {
    std::uint32_t fbWidth = 0;
    std::uint32_t fbHeight = 0;
    mWindowBackend->GetDimensions(&fbWidth, &fbHeight, nullptr, nullptr);

    const auto w = static_cast<int>(fbWidth);
    const auto h = static_cast<int>(fbHeight);
    const auto flippedY = h - y - height;

    // deko3d rejects scissors outside the render target; clamp like Metal does.
    const auto scissorX = static_cast<std::uint32_t>(std::clamp(x, 0, w));
    const auto scissorY = static_cast<std::uint32_t>(std::clamp(flippedY, 0, h));
    const auto scissorW = static_cast<std::uint32_t>(std::clamp(width, 0, w));
    const auto scissorH = static_cast<std::uint32_t>(std::clamp(height, 0, h));

    auto cb = mFrameCmdBuf;
    cb.setScissors(0, { { scissorX, scissorY, scissorW, scissorH } });
}

void GfxRenderingApiDeko3d::SetUseAlpha(bool useAlpha) {
    mUseAlpha = useAlpha;
}

void GfxRenderingApiDeko3d::SetSrgbMode() {
}

void GfxRenderingApiDeko3d::SetCurrentPrimDepth(float depth) {
}

// --------------------------------------------------------------------------------------------------------------------
// Draw
// --------------------------------------------------------------------------------------------------------------------

void GfxRenderingApiDeko3d::DrawTriangles(float bufVbo[], std::size_t bufVboLen, std::size_t bufVboNumTris) {
    if (bufVboNumTris == 0) {
        return;
    }

    const auto drawT0 = NowNs();

    const std::uint32_t vertexCount = static_cast<std::uint32_t>(3 * bufVboNumTris);
    const std::uint32_t strideFloats = static_cast<std::uint32_t>(bufVboLen / vertexCount);
    const std::uint32_t strideBytes = strideFloats * static_cast<std::uint32_t>(sizeof(float));
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(bufVboLen * sizeof(float));

    const auto offset = AlignUp(mVtxRingOffset[mRing], 16);
    if (offset + dataBytes > gVtxRingSize) {
        mWindowBackend->Trace("DrawTriangles: vertex ring overflow (bump gVtxRingSize)");
        return; // Drop the batch rather than overrun
    }

    const auto cpyT0 = NowNs();
    std::memcpy(mVtxRingCpu[mRing] + offset, bufVbo, dataBytes);
    gMemCpyNs += NowNs() - cpyT0;

    const DkGpuAddr vtxAddr = mVtxRingGpu[mRing] + offset;
    mVtxRingOffset[mRing] = offset + dataBytes;

    // Inputs are packed last; the single forced input is the final (3 + alpha) floats.  Read its RGB.
    const std::uint32_t inputFloats = 3u + (mUseAlpha ? 1u : 0u);
    const std::uint32_t inputOffsetBytes = (strideFloats - inputFloats) * static_cast<std::uint32_t>(sizeof(float));

    auto cb = mFrameCmdBuf;

    const bool isDepthEnabled = mDepthTest || mDepthMask;
    cb.bindDepthStencilState(
        dk::DepthStencilState{}
            .setDepthTestEnable(isDepthEnabled)
            .setDepthWriteEnable(mDepthMask)
            .setDepthCompareOp(mDepthTest ? (mDecal ? DkCompareOp_Lequal : DkCompareOp_Less) : DkCompareOp_Always));

    cb.bindVtxAttribState({
        { 0, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0 },                // aVtxPos @ 0
        { 0, 0, inputOffsetBytes, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 }, // aInput1 @ adaptive
    });

    cb.bindVtxBufferState({ { strideBytes, 0 } });
    cb.bindVtxBuffer(0, vtxAddr, dataBytes);
    cb.draw(DkPrimitive_Triangles, vertexCount, 1, 0, 0);

    ++gDrawCalls;
    gDrawNs += NowNs() - drawT0;
}

// --------------------------------------------------------------------------------------------------------------------
// Frame lifecycle
// --------------------------------------------------------------------------------------------------------------------

void GfxRenderingApiDeko3d::Init() {
    const auto device = mWindowBackend->GetDevice();

    for (std::uint8_t i = 0; i < sVtxRing; ++i) {
        mVtxRingMemBlock[i] = dk::MemBlockMaker{ device, AlignUp(gVtxRingSize, DK_MEMBLOCK_ALIGNMENT) }
                                  .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                  .create();

        mVtxRingGpu[i] = mVtxRingMemBlock[i].getGpuAddr();
        mVtxRingCpu[i] = static_cast<std::uint8_t*>(mVtxRingMemBlock[i].getCpuAddr());

        mVtxRingOffset[i] = 0;
    }
}

void GfxRenderingApiDeko3d::OnResize() {
}

void GfxRenderingApiDeko3d::StartFrame() {
    mFrameCmdBuf = mWindowBackend->BeginFrameRecording();
    mRing = mWindowBackend->GetRecordingRing();
    mVtxRingOffset[mRing] = 0; // BeginFrameRecording already fence-waited this slot, so its memory is free

    auto cb = mFrameCmdBuf;
    cb.bindShaders(DkStageFlag_GraphicsMask, { &mWindowBackend->GetColorVsh(), &mWindowBackend->GetColorFsh() });
    cb.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
    cb.bindColorState(dk::ColorState{});
    cb.bindColorWriteState(dk::ColorWriteState{});
}

void GfxRenderingApiDeko3d::EndFrame() {
    mWindowBackend->EndFrameRecording(mFrameCmdBuf.finishList());

    if (++gDrawFrames >= 60) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "[deko-draw] avg us/60f: draw=%lld memcpy=%lld calls=%d", gDrawNs / 60 / 1000,
                      gMemCpyNs / 60 / 1000, gDrawCalls / 60);
        mWindowBackend->Trace(buf);

        gDrawNs = gMemCpyNs = 0;
        gDrawCalls = gDrawFrames = 0;
    }
}

void GfxRenderingApiDeko3d::FinishRender() {
}

// --------------------------------------------------------------------------------------------------------------------
// Framebuffers
// --------------------------------------------------------------------------------------------------------------------

int GfxRenderingApiDeko3d::CreateFramebuffer() {
    return 0;
}

void GfxRenderingApiDeko3d::UpdateFramebufferParameters(int fbId, std::uint32_t width, std::uint32_t height,
                                                        std::uint32_t msaaLevel, bool openglInvertY, bool renderTarget,
                                                        bool hasDepthBuffer, bool canExtractDepth) {
}

void GfxRenderingApiDeko3d::StartDrawToFramebuffer(int fbId, float noiseScale) {
}

void GfxRenderingApiDeko3d::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                            int dstX0, int dstY0, int dstX1, int dstY1) {
}

void GfxRenderingApiDeko3d::ClearFramebuffer(bool color, bool depth) {
}

void GfxRenderingApiDeko3d::ReadFramebufferToCPU(int fbId, std::uint32_t width, std::uint32_t height,
                                                 std::uint16_t* rgba16Buf) {
}

void GfxRenderingApiDeko3d::ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) {
}

std::unordered_map<std::pair<float, float>, std::uint16_t, hash_pair_ff>
GfxRenderingApiDeko3d::GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) {
    // Real depth readback isn't implemented yet, but Interpreter::GetPixelDepth() does find(coord)->second on this
    // result with no end() check.  An empty map makes that dereference end().  Return a defined placeholder for every
    // requested coordinate so the lookup resolves.  Replaced by a true readback with the real backend.
    std::unordered_map<std::pair<float, float>, std::uint16_t, hash_pair_ff> result;
    for (const auto& coord : coordinates) {
        result.emplace(coord, 0); // 0 reads as near; 0xFFFF reads as empty/far.
    }

    return result;
}

void* GfxRenderingApiDeko3d::GetFramebufferTextureId(int fbId) {
    return nullptr;
}

void GfxRenderingApiDeko3d::SelectTextureFb(int fbId) {
}
} // namespace Fast
#endif