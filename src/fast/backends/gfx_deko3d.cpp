#if defined(ENABLE_DEKO3D)
#include "fast/backends/gfx_deko3d.h"
#include "fast/interpreter.h"

namespace {
constexpr std::uint32_t gVtxRingSize = 0x800000; // 8 MiB per slot

constexpr std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
    return value + alignment - 1 & ~(alignment - 1);
}

// std140 layout consumed by color.frag.glsl's CombinerUbo block. Four contiguous ivec4s (color0, alpha0, color1,
// alpha1) match CCFeatures::c[2][2][4] byte-for-byte, then five ints.
struct CombinerUbo {
    std::int32_t C[2][2][4]; // [cycle][color/alpha][a,b,c,d]
    std::int32_t NumInputs;
    std::int32_t Do2Cyc;
    std::int32_t OptAlpha;
    std::int32_t OptFog;
    std::int32_t OptGrayscale;
    std::int32_t Pad[3]; // Pad to 16-byte multiple (std140 block size)
};

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
    mCurrentProgram = reinterpret_cast<ShaderProgramDeko3d*>(newPrg);
}

void GfxRenderingApiDeko3d::ClearShaderCache() {
    mShaderProgramPool.clear();
    mCurrentProgram = nullptr;
}

ShaderProgram* GfxRenderingApiDeko3d::CreateAndLoadNewShader(std::uint64_t shaderId1, std::uint64_t shaderId2) {
    ShaderProgramDeko3d& prg = mShaderProgramPool[{ shaderId1, shaderId2 }];
    prg.ShaderId1 = shaderId1;
    prg.ShaderId2 = shaderId2;

    gfx_cc_get_features(shaderId1, shaderId2, &prg.Cc);
    LoadShader(reinterpret_cast<ShaderProgram*>(&prg));

    return reinterpret_cast<ShaderProgram*>(&prg);
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

    const auto i = mShaderProgramPool.find({ shaderId1, shaderId2 });
    if (i == mShaderProgramPool.end()) {
        return nullptr; // Miss -> interpreter calls CreateAndLoadNewShader, matching the OGL contract
    }

    return reinterpret_cast<ShaderProgram*>(&i->second);
}

void GfxRenderingApiDeko3d::ShaderGetInfo(ShaderProgram* prg, std::uint8_t* numInputs, bool usedTextures[2]) {
    const auto cc = reinterpret_cast<ShaderProgramDeko3d*>(prg)->Cc;
    const bool isUntextured = !cc.usedTextures[0] && !cc.usedTextures[1];

    if (isUntextured) {
        if (numInputs) {
            *numInputs = static_cast<std::uint8_t>(cc.numInputs);
        }

        if (usedTextures) {
            usedTextures[0] = false;
            usedTextures[1] = false;
        }
    } else {
        // Textured draws need samplers.  Force the flat single-input packing so the interpreter lays out exactly what
        // the passthrough path reads.
        if (numInputs) {
            *numInputs = 1;
        }

        if (usedTextures) {
            usedTextures[0] = false;
            usedTextures[1] = false;
        }
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

    const auto& cc = mCurrentProgram->Cc;
    const bool isUntextured = !cc.usedTextures[0] && !cc.usedTextures[1];

    // Per-vertex attribute layout in the interpreter's packing order, forward-computed.
    // usedTextures is reported false to the interpreter, so there are no texcoord/clamp floats.
    const std::uint32_t inputWidth = cc.opt_alpha ? 4u : 3u; // floats per combiner input
    const std::uint32_t numInputs = isUntextured ? static_cast<std::uint32_t>(cc.numInputs) : 1u;
    const std::uint32_t fogFloats = cc.opt_fog ? 4u : 0u;
    const std::uint32_t grayFloats = cc.opt_grayscale ? 4u : 0u;
    const std::uint32_t inputsBase = 4u + fogFloats + grayFloats; // First input's float offset

    auto cb = mFrameCmdBuf;

    const bool isDepthEnabled = mDepthTest || mDepthMask;
    cb.bindDepthStencilState(
        dk::DepthStencilState{}
            .setDepthTestEnable(isDepthEnabled)
            .setDepthWriteEnable(mDepthMask)
            .setDepthCompareOp(mDepthTest ? (mDecal ? DkCompareOp_Lequal : DkCompareOp_Less) : DkCompareOp_Always));

    // ----------------------------------------------------------------------------------------------------------------
    // Combiner uniform
    // ----------------------------------------------------------------------------------------------------------------

    CombinerUbo ubo = {};
    if (isUntextured) {
        std::memcpy(ubo.C, cc.c, sizeof(ubo.C)); // int[2][2][4] -> int32[2][2][4], same layout
        ubo.NumInputs = cc.numInputs;
        ubo.Do2Cyc = cc.opt_2cyc ? 1 : 0;
        ubo.OptAlpha = cc.opt_alpha ? 1 : 0;
        ubo.OptFog = cc.opt_fog ? 1 : 0;
        ubo.OptGrayscale = cc.opt_grayscale ? 1 : 0;
    } else {
        // Passthrough: (input1 - 0) * 1 + 0 = input1, single cycle, no extras.  Keeps textured geometry exactly as the
        // pre-combiner build.
        ubo.C[0][0][0] = SHADER_INPUT_1;
        ubo.C[0][0][1] = SHADER_0;
        ubo.C[0][0][2] = SHADER_1;
        ubo.C[0][0][3] = SHADER_0;
        ubo.NumInputs = 1;
    }

    const auto uboOff = AlignUp(mUboRingOffset[mRing], DK_UNIFORM_BUF_ALIGNMENT);
    if (uboOff + sUniformSlotSize > sUniformRingSize) {
        mWindowBackend->Trace("DrawTriangles: UBO ring overflow (bump sUniformRingSize)");
        return;
    }

    std::memcpy(mUboRingCpu[mRing] + uboOff, &ubo, sizeof(ubo));
    const DkGpuAddr uboAddr = mUboRingGpu[mRing] + uboOff;

    mUboRingOffset[mRing] = uboOff + sUniformSlotSize;
    cb.bindUniformBuffer(DkStage_Fragment, 0, uboAddr, sUniformSlotSize);

    // ----------------------------------------------------------------------------------------------------------------
    // Vertex attributes: pos + N inputs
    // ----------------------------------------------------------------------------------------------------------------

    std::array<DkVtxAttribState, 1 + 7> attribs = {};
    std::uint32_t attribCount = 0;
    attribs[attribCount++] = { 0, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0 }; // aVtxPos @ 0

    for (std::uint32_t i = 0; i < numInputs; ++i) {
        const std::uint32_t offFloats = inputsBase + i * inputWidth;
        attribs[attribCount++] = {
            0, 0, offFloats * static_cast<std::uint32_t>(sizeof(float)), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0
        }; // aInputN.rgb
    }

    cb.bindVtxAttribState(dk::detail::ArrayProxy<const DkVtxAttribState>(attribCount, attribs.data()));
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

    for (std::uint8_t i = 0; i < sVtxRing; ++i) {
        mUboRingMemBlock[i] = dk::MemBlockMaker{ device, AlignUp(sUniformRingSize, DK_MEMBLOCK_ALIGNMENT) }
                                  .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                  .create();
        mUboRingGpu[i] = mUboRingMemBlock[i].getGpuAddr();
        mUboRingCpu[i] = static_cast<std::uint8_t*>(mUboRingMemBlock[i].getCpuAddr());
        mUboRingOffset[i] = 0;
    }
}

void GfxRenderingApiDeko3d::OnResize() {
}

void GfxRenderingApiDeko3d::StartFrame() {
    mFrameCmdBuf = mWindowBackend->BeginFrameRecording();
    mRing = mWindowBackend->GetRecordingRing();
    mVtxRingOffset[mRing] = 0; // BeginFrameRecording already fence-waited this slot, so its memory is free
    mUboRingOffset[mRing] = 0;

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