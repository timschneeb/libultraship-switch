#if defined(ENABLE_DEKO3D)
#include "fast/backends/gfx_deko3d.h"

#include <imgui.h>

#include "fast/interpreter.h"

namespace {
constexpr std::uint32_t gVtxRingSize = 0x800000; // 8 MiB per slot

struct CombinerUbo {
    std::int32_t C[2][2][4]; // [cycle][color/alpha][a,b,c,d]
    std::int32_t NumInputs;
    std::int32_t Do2Cyc;
    std::int32_t OptAlpha;
    std::int32_t OptFog;
    std::int32_t OptGrayscale;
    std::int32_t UsedTex0; // 1 -> sample uTex0 (textured variant only); 0 leaves the bound handle unsampled
    std::int32_t UsedTex1;
    std::int32_t OptTextureEdge;    // Cutout alpha test: a > 0.19 ? 1.0 : discard
    std::int32_t OptAlphaThreshold; // a < 8/256 ? discard
    float TexClampS0;               // Per-tile sub-texture clamp upper bound (normalized); < 0 => axis not clamped
    float TexClampT0;
    float TexClampS1;
    float TexClampT1;
    std::int32_t Pad[3]; // Pad to 128 bytes
};

constexpr std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
    return value + alignment - 1 & ~(alignment - 1);
}

// N64 tile clamp/mirror bits -> index into the static wrap-mode table.  CLAMP wins over MIRROR.
constexpr std::uint32_t CmToWrapIndex(std::uint32_t value) {
    if (value & G_TX_CLAMP) {
        return 0;
    }

    return value & G_TX_MIRROR ? 1 : 2;
}

constexpr DkWrapMode gWrapModes[3] = { DkWrapMode_ClampToEdge, DkWrapMode_MirroredRepeat, DkWrapMode_Repeat };

// Slot of the immutable sampler descriptor for (filter, cms, cmt).  2 filters x 3 wrapS x 3 wrapT = 18 slots, written
// once in Init and never mutated, so draws capture sampler state by value (Metal's immutable MTLSamplerState model)
// instead of racing a shared mutable slot.
constexpr std::uint32_t SamplerIndex(bool linearFilter, std::uint32_t cms, std::uint32_t cmt) {
    return (linearFilter ? 9u : 0u) + CmToWrapIndex(cms) * 3u + CmToWrapIndex(cmt);
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
    const auto i = mShaderProgramPool.find({ shaderId1, shaderId2 });
    if (i == mShaderProgramPool.end()) {
        return nullptr;
    }

    return reinterpret_cast<ShaderProgram*>(&i->second);
}

void GfxRenderingApiDeko3d::ShaderGetInfo(ShaderProgram* prg, std::uint8_t* numInputs, bool usedTextures[2]) {
    const auto& cc = reinterpret_cast<ShaderProgramDeko3d*>(prg)->Cc;

    // Truthful now: the interpreter packs texcoords (+ optional clamp floats) per used texture and the real input
    // count.  DrawTriangles forward-computes attribute offsets from the same CCFeatures.
    if (numInputs) {
        *numInputs = static_cast<std::uint8_t>(cc.numInputs);
    }

    if (usedTextures) {
        usedTextures[0] = cc.usedTextures[0];
        usedTextures[1] = cc.usedTextures[1];
    }
}

// --------------------------------------------------------------------------------------------------------------------
// Textures
// --------------------------------------------------------------------------------------------------------------------

std::uint32_t GfxRenderingApiDeko3d::NewTexture() {
    mTextures.emplace_back();
    return static_cast<std::uint32_t>(mTextures.size() - 1);
}

void GfxRenderingApiDeko3d::SelectTexture(int tile, std::uint32_t textureId) {
    mCurrentTile = tile;
    mCurrentTextureIds[tile] = textureId;
}

void GfxRenderingApiDeko3d::UploadTexture(const std::uint8_t* rgba32Buf, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }

    const auto device = mWindowBackend->GetDevice();
    auto queue = mWindowBackend->GetQueue();
    const auto texId = static_cast<std::uint32_t>(mCurrentTextureIds[mCurrentTile]);
    auto& texture = mTextures[texId];

    // (Re)create the device-local sampled image when absent or resized.  setFlags(0): block-linear, sampled, no
    // UsageRenderer/no HwCompression.
    if (!texture.ImageMemBlock || texture.Width != width || texture.Height != height) {
        // Related: https://github.com/devkitPro/deko3d/issues/10
        // For block-linear images whose base height is 6..8, dkImageLayoutGetSize() demotes the auto-picked tile
        // height (TwoGobs -> OneGob) when sizing the base level, but the copy engine (ImageInfo::fromImageView, mip 0
        // skips the demotion) and the TIC are programmed with the undemoted value, so the hardware walks one
        // GOB-column stride (1024B) past the reported size.  With one tight memblock per image, that overhang is
        // unmapped, causing uploads to silently drop every texel with x >= 64 and reads return 0xFF.  Pick the tile
        // height ourselves, replicating deko3d's pick and its base-level demotion, so size math and hardware
        // programming agree by construction.  Safe here because our images are always mipLevels == 1.
        const std::uint32_t heightAndHalfGobs = (height + height / 2u + 7u) / 8u;
        std::uint32_t tileHeight = heightAndHalfGobs >= 16u  ? 4u
                                   : heightAndHalfGobs >= 8u ? 3u
                                   : heightAndHalfGobs >= 4u ? 2u
                                   : heightAndHalfGobs >= 2u ? 1u
                                                             : 0u;

        while (tileHeight != 0 && 8u << tileHeight - 1u >= height) {
            --tileHeight;
        }

        dk::ImageLayout layout = {};
        dk::ImageLayoutMaker{ device }
            .setFlags(DkImageFlags_CustomTileSize)
            .setTileSize(static_cast<DkTileSize>(tileHeight))
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(width, height)
            .initialize(layout);

        const auto size = AlignUp(static_cast<std::uint32_t>(layout.getSize()), layout.getAlignment());
        texture.ImageMemBlock = dk::MemBlockMaker{ device, AlignUp(size, DK_MEMBLOCK_ALIGNMENT) }
                                    .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                                    .create();
        texture.Image.initialize(layout, texture.ImageMemBlock, 0);
        texture.Width = width;
        texture.Height = height;
    }

    const std::uint32_t pixelBytes = width * height * 4u;

    // CPU-visible staging buffer; the GPU copies (and block-linear swizzles) it into the image.
    dk::UniqueMemBlock staging = dk::MemBlockMaker{ device, AlignUp(pixelBytes, DK_MEMBLOCK_ALIGNMENT) }
                                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                     .create();
    std::memcpy(staging.getCpuAddr(), rgba32Buf, pixelBytes);

    // rowLength/imageHeight = 0 -> tightly packed at the image's own dimensions.
    mUploadCmdBuf.clear();
    mUploadCmdBuf.copyBufferToImage({ staging.getGpuAddr(), 0, 0 }, dk::ImageView{ texture.Image },
                                    { 0, 0, 0, width, height, 1 });
    queue.submitCommands(mUploadCmdBuf.finishList());
    queue.waitIdle();

    // Point this texture's descriptor slot at the (possibly newly recreated image).  Slot == texture ID.  Sampled by
    // dkMakeTextureHandle(id, id) at draw time; the dirty flag forces a descriptor-cache invalidate before that draw.
    mImageDescriptors[texId].initialize(dk::ImageView{ texture.Image });
    mIsDescriptorsDirty = true;
}

void GfxRenderingApiDeko3d::SetSamplerParameters(int sampler, bool linearFilter, std::uint32_t cms, std::uint32_t cmt) {
    // Sampler state is captured by value: resolve to one of the 18 immutable descriptors and remember the index on
    // the texture.  DrawTriangles bakes it into the handle at record time, so the twice-per-texture call pattern
    // (defaults, then real values) and mid-frame param changes can no longer rewrite state under recorded or in-flight
    // draws.  Three-point filtering is deferred -> nearest/linear only.
    const auto id = static_cast<std::uint32_t>(mCurrentTextureIds[sampler]);
    if (id >= mTextures.size()) {
        return; // No texture selected on this tile yet; nothing to attach the sampler to.
    }

    mTextures[id].SamplerIndex = SamplerIndex(linearFilter, cms, cmt);
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
    // ImTextureID is patched to void* in SoH's ImGui config.  Encode the texture's DkResHandle (image slot + immutable
    // sampler slot) as an opaque pointer value; RenderDrawData decodes it and binds it against the descriptor sets
    // already bound in StartFrame.  Handles are stable (slot == ID, sampler index stored per texture), so, unlike the
    // standalone reference, we can pass the handle by value rather than a pointer into a table.
    if (id < 0 || static_cast<std::size_t>(id) >= mTextures.size()) {
        return nullptr;
    }

    const auto handle = dkMakeTextureHandle(static_cast<std::uint32_t>(id), mTextures[id].SamplerIndex);
    return reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(handle));
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
    // Flip Y against the *current* render target (mGameFb at internal resolution, or the window for fb 0), set by the
    // last StartDrawToFramebuffer.  Flipping against the window height while the game FB is bound is the stretch this
    // slice removes.
    auto cb = mFrameCmdBuf;
    cb.setViewports(
        0, { { static_cast<float>(x),
               static_cast<float>(static_cast<std::int32_t>(mRenderTargetHeight) - y - height), // Top-left origin
               static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f } });
}

void GfxRenderingApiDeko3d::SetScissor(int x, int y, int width, int height) {
    const auto w = static_cast<std::int32_t>(mRenderTargetWidth);
    const auto h = static_cast<std::int32_t>(mRenderTargetHeight);
    const auto flippedY = h - y - height;

    // deko3d rejects scissors outside the render target; clamp.
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

    // Per-vertex attribute layout in the interpreter's packing order, forward computed from the same CCFeatures the
    // interpreter used: pos(4) | per used texture [u,v (+clampS)(+clampT)] | fog(4)? | gray(4) ? | inputs(N * width).
    const std::uint32_t inputWidth = cc.opt_alpha ? 4u : 3u;
    const std::uint32_t numInputs = static_cast<std::uint32_t>(cc.numInputs);
    const std::uint32_t fogFloats = cc.opt_fog ? 4u : 0u;
    const std::uint32_t grayFloats = cc.opt_grayscale ? 4u : 0u;

    // Byte-offset of each texcoord's u,v, within the vertex.
    std::uint32_t tex0Floats = 0;
    std::uint32_t tex1Floats = 0;
    std::uint32_t cursor = 4u;

    if (cc.usedTextures[0]) {
        tex0Floats = cursor;
        cursor += 2u + (cc.clamp[0][0] ? 1u : 0u) + (cc.clamp[0][1] ? 1u : 0u);
    }

    if (cc.usedTextures[1]) {
        tex1Floats = cursor;
        cursor += 2u + (cc.clamp[1][0] ? 1u : 0u) + (cc.clamp[1][1] ? 1u : 0u);
    }

    const std::uint32_t inputsBase = cursor + fogFloats + grayFloats; // First input's float offset

    auto cb = mFrameCmdBuf;

    const bool isDepthEnabled = mDepthTest || mDepthMask;
    cb.bindDepthStencilState(
        dk::DepthStencilState{}
            .setDepthTestEnable(isDepthEnabled)
            .setDepthWriteEnable(mDepthMask)
            .setDepthCompareOp(mDepthTest ? (mDecal ? DkCompareOp_Lequal : DkCompareOp_Less) : DkCompareOp_Always));

    // Per-draw blend enable.  The blend equation/factors are fixed (over-blend, bound once in StartFrame); only the
    // RT0 enable bit varies, driven by SetUseAlpha.
    cb.bindColorState(dk::ColorState{}.setBlendEnable(0, mUseAlpha));

    // Decal surfaces (paths, shadows) are coplanar with the ground.  LEQUAL above lets them pass the depth test;
    // a small negative depth bias pushes them toward the player so they composite on instead of z-fighting.
    cb.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None).setDepthBiasEnable(mDecal));
    if (mDecal) {
        cb.setDepthBias(-2.0f, 0.0f, -2.0f);
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Combiner uniform
    // ----------------------------------------------------------------------------------------------------------------

    CombinerUbo ubo = {};
    std::memcpy(ubo.C, cc.c, sizeof(ubo.C)); // int[2][2][4] -> int32[2][2][4], same layout
    ubo.NumInputs = cc.numInputs;
    ubo.Do2Cyc = cc.opt_2cyc ? 1 : 0;
    ubo.OptAlpha = cc.opt_alpha ? 1 : 0;
    ubo.OptFog = cc.opt_fog ? 1 : 0;
    ubo.OptGrayscale = cc.opt_grayscale ? 1 : 0;
    ubo.UsedTex0 = cc.usedTextures[0] ? 1 : 0;
    ubo.UsedTex1 = cc.usedTextures[1] ? 1 : 0;
    ubo.OptTextureEdge = cc.opt_texture_edge ? 1 : 0;
    ubo.OptAlphaThreshold = cc.opt_alpha_threshold ? 1 : 0;

    // Sub-texture clamp bounds, read from vertex 0.  Negative => the axis is not clamped, so the shader leaves it to
    // the sampler wrap mode.
    ubo.TexClampS0 = cc.usedTextures[0] && cc.clamp[0][0] ? bufVbo[tex0Floats + 2] : -1.0f;
    ubo.TexClampT0 = cc.usedTextures[0] && cc.clamp[0][1] ? bufVbo[tex0Floats + 2 + (cc.clamp[0][0] ? 1 : 0)] : -1.0f;
    ubo.TexClampS1 = cc.usedTextures[1] && cc.clamp[1][0] ? bufVbo[tex1Floats + 2] : -1.0f;
    ubo.TexClampT1 = cc.usedTextures[1] && cc.clamp[1][1] ? bufVbo[tex1Floats + 2 + (cc.clamp[1][0] ? 1 : 0)] : -1.0f;

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
    // Shader variant + vertex attributes
    //
    //  Untextured: loc0 pos, loc1..N inputs (color.vert).
    //  Textured:   loc0 pos, loc1 texCoord0, loc2 texCoord1, loc3..N inputs (color_texture.vert).
    //
    // Untextured texcoord slots are bound at offset 0 (dead varying), so input locations stay fixed across
    // tex0-only/tex1-only/both.
    // ----------------------------------------------------------------------------------------------------------------

    if (const std::int32_t isTextured = isUntextured ? 0 : 1; mCurrentShaderTextured != isTextured) {
        if (isTextured) {
            cb.bindShaders(DkStageFlag_GraphicsMask,
                           { &mWindowBackend->GetColorTextureVsh(), &mWindowBackend->GetColorTextureFsh() });
        } else {
            cb.bindShaders(DkStageFlag_GraphicsMask,
                           { &mWindowBackend->GetColorVsh(), &mWindowBackend->GetColorFsh() });
        }

        mCurrentShaderTextured = isTextured;
    }

    std::array<DkVtxAttribState, 3 + 4> attribs = {};
    std::uint32_t attribCount = 0;

    const auto GetFloatOff = [](std::uint32_t floats) { return floats * static_cast<std::uint32_t>(sizeof(float)); };

    attribs[attribCount++] = { 0, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0 }; // loc0 @ aVtxPos

    if (!isUntextured) {
        // loc1/loc2 texcoords; unused slot reads byte 0 (dead), so its varying is harmless.
        attribs[attribCount++] = { 0, 0, GetFloatOff(tex0Floats), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 };
        attribs[attribCount++] = { 0, 0, GetFloatOff(tex1Floats), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 };
    }

    // 4 floats per input when opt_alpha (rgba), else 3 (rgb).  When 3x32, the shader's vec4 input gets a default .a
    // that the alpha column never reads (it is gated on uOptAlpha), so no reliance on vertex-fetch component defaults.
    const DkVtxAttribSize inputAttribSize = cc.opt_alpha ? DkVtxAttribSize_4x32 : DkVtxAttribSize_3x32;

    for (std::uint32_t i = 0; i < numInputs; ++i) {
        attribs[attribCount++] = {
            0, 0, GetFloatOff(inputsBase + i * inputWidth), inputAttribSize, DkVtxAttribType_Float, 0
        }; // aInputN
    }

    if (!isUntextured) {
        // Reference each texture by its image descriptor slot (== ID) paired with its immutable sampler slot.  The
        // unused unit gets a valid handle (the used ID), so the shader always has a bound texture even though
        // uUsedTexN gates sampling.  Invalidate the descriptor cache once if any image descriptor was written since
        // the last bind (samplers are write-once at Init and never dirty).
        if (mIsDescriptorsDirty) {
            cb.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors | DkInvalidateFlags_Image);
            mIsDescriptorsDirty = false;
        }

        const std::uint32_t anyId = cc.usedTextures[0] ? mCurrentTextureIds[0] : mCurrentTextureIds[1];
        const std::uint32_t id0 = cc.usedTextures[0] ? mCurrentTextureIds[0] : anyId;
        const std::uint32_t id1 = cc.usedTextures[1] ? mCurrentTextureIds[1] : anyId;
        const DkResHandle handles[2] = { dkMakeTextureHandle(id0, mTextures[id0].SamplerIndex),
                                         dkMakeTextureHandle(id1, mTextures[id1].SamplerIndex) };

        cb.bindTextures(DkStage_Fragment, 0, dk::detail::ArrayProxy(2, handles));
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

    // Reusable command buffer for synchronous texture uploads (copyBufferToImage).
    mUploadCmdMemBlock = dk::MemBlockMaker{ device, AlignUp(sUploadCmdMemSize, DK_MEMBLOCK_ALIGNMENT) }
                             .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                             .create();
    mUploadCmdBuf = dk::CmdBufMaker{ device }.create();
    mUploadCmdBuf.addMemory(mUploadCmdMemBlock, 0, sUploadCmdMemSize);

    // Texture descriptor sets (CPU-writable, GPU-read).  Slot == texture ID; bound once per frame in StartFrame.
    constexpr auto imgSetSize =
        AlignUp(sMaxTextures * static_cast<std::uint32_t>(sizeof(dk::ImageDescriptor)), DK_MEMBLOCK_ALIGNMENT);
    mImageDescMemBlock = dk::MemBlockMaker{ device, imgSetSize }
                             .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                             .create();
    mImageDescriptors = static_cast<dk::ImageDescriptor*>(mImageDescMemBlock.getCpuAddr());

    constexpr auto smpSetSize =
        AlignUp(sSamplerCount * static_cast<std::uint32_t>(sizeof(dk::SamplerDescriptor)), DK_MEMBLOCK_ALIGNMENT);
    mSamplerDescMemBlock = dk::MemBlockMaker{ device, smpSetSize }
                               .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                               .create();
    mSamplerDescriptors = static_cast<dk::SamplerDescriptor*>(mSamplerDescMemBlock.getCpuAddr());

    // Immutable sampler table: slot layout mirrors SamplerIndex().  Written once here, never touched again.
    for (std::uint32_t f = 0; f < 2; ++f) {
        for (std::uint32_t s = 0; s < 3; ++s) {
            for (std::uint32_t t = 0; t < 3; ++t) {
                const auto filter = f != 0 ? DkFilter_Linear : DkFilter_Nearest;
                dk::Sampler smp = {};

                smp.setFilter(filter, filter);
                smp.setWrapMode(gWrapModes[s], gWrapModes[t], DkWrapMode_Repeat);
                mSamplerDescriptors[f * 9 + s * 3 + t].initialize(smp);
            }
        }
    }

    // The table is written before any frame is recorded, but force one descriptor-cache invalidate on the first
    // textured draw regardless of upload order.
    mIsDescriptorsDirty = true;

    // Reserve framebuffer ID 0 for the window/swap chain (owns no image; its surfaces come from the window backend's
    // acquired slot).  CreateFramebuffer appends real offscreen targets starting at ID 1.
    mFramebuffers.resize(1);

    // Seed the current-target dims with the window size so a stray draw before the first StartDrawToFramebuffer (the
    // interpreter never does this, but be defensive) flips against a sane height rather than 0.
    mWindowBackend->GetDimensions(&mRenderTargetWidth, &mRenderTargetHeight, nullptr, nullptr);
}

void GfxRenderingApiDeko3d::OnResize() {
}

void GfxRenderingApiDeko3d::StartFrame() {
    mFrameCmdBuf = mWindowBackend->BeginFrameRecording();
    mRing = mWindowBackend->GetRecordingRing();
    mVtxRingOffset[mRing] = 0;
    mUboRingOffset[mRing] = 0;

    auto cb = mFrameCmdBuf;

    // No render target is bound here anymore.  The interpreter always issues StartDrawToFramebuffer (mGameFb or 0)
    // immediately after StartFrame, followed by ClearFramebuffer -- that is where the RT bind + clear now live (slice
    // 1a bound fb 0 here only because StartDrawToFramebuffer was still a no-op).  Binding here would just be an extra
    // bind the first StartDrawToFramebuffer overwrites.

    // Bind the texture descriptor sets for the frame; individual textures are referenced per draw via bindTextures.
    cb.bindImageDescriptorSet(mImageDescMemBlock.getGpuAddr(), sMaxTextures);
    cb.bindSamplerDescriptorSet(mSamplerDescMemBlock.getGpuAddr(), sSamplerCount);
    mCurrentShaderTextured = -1; // Force the first draw to bind its shader variant
    cb.bindColorState(dk::ColorState{});
    cb.bindColorWriteState(dk::ColorWriteState{});

    // Fixed blend equation for the frame: standard over-blend.  Whether it is applied is the ColorState blend-enable
    // bit, set per draw in DrawTriangles from mUseAlpha.  bindBlendStates encodes the state into the command stream,
    // so the local is safe to let go.
    dk::BlendState blendState = {};
    blendState.setColorBlendOp(DkBlendOp_Add)
        .setSrcColorBlendFactor(DkBlendFactor_SrcAlpha)
        .setDstColorBlendFactor(DkBlendFactor_InvSrcAlpha)
        .setAlphaBlendOp(DkBlendOp_Add)
        .setSrcAlphaBlendFactor(DkBlendFactor_SrcAlpha)
        .setDstAlphaBlendFactor(DkBlendFactor_InvSrcAlpha);
    cb.bindBlendStates(0, dk::detail::ArrayProxy<const DkBlendState>(1, &blendState));
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
    // ID 0 is reserved for the window in Init(); real targets start at 1.  The color surface is a reserved texture
    // slot so it samples through the shared descriptor set (GetFramebufferTextureId/SelectTextureFb -> DkResHandle).
    const auto id = static_cast<std::int32_t>(mFramebuffers.size());
    auto& fb = mFramebuffers.emplace_back();
    fb.TextureId = static_cast<std::int32_t>(NewTexture());

    // FB surfaces are sampled as whole images (ImGui composite, FB effects), so linear + clamp -- never wrap/nearest.
    mTextures[fb.TextureId].SamplerIndex = SamplerIndex(true, G_TX_CLAMP, G_TX_CLAMP);
    return id;
}

void GfxRenderingApiDeko3d::UpdateFramebufferParameters(int fbId, std::uint32_t width, std::uint32_t height,
                                                        std::uint32_t msaaLevel, bool openglInvertY, bool renderTarget,
                                                        bool hasDepthBuffer, bool canExtractDepth) {
    // fb 0 is the window: its surfaces are owned by the window backend and sized to the swap chain, so there is
    // nothing to (re)allocate here.
    if (fbId == 0 || fbId >= static_cast<std::int32_t>(mFramebuffers.size())) {
        return;
    }

    width = std::max(width, 1u);
    height = std::max(height, 1u);

    auto& [TextureId, DepthMemBlock, DepthImage, Width, Height, HasDepth, IsYInverted] = mFramebuffers[fbId];
    auto& texture = mTextures[TextureId];
    const bool isSizeChanged = texture.Width != width || texture.Height != height;

    const auto device = mWindowBackend->GetDevice();

    if (isSizeChanged) {
        // Resize frees an image a still-in-flight frame may sample (resolution change, dock/undock).  Drain first --
        // this path is rare (not per-frame), so the stall is acceptable and prevents a use-after-free of GPU memory.
        mWindowBackend->GetQueue().waitIdle();

        // Color surface: render target + sampled, no HwCompression (sampled every frame; compression would need a
        // resolve before each sample).  Default tiling -- the UploadTexture CustomTileSize workaround targets small
        // block-linear sampled uploads.
        dk::ImageLayout colorLayout = {};
        dk::ImageLayoutMaker{ device }
            .setFlags(DkImageFlags_UsageRender)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(width, height)
            .initialize(colorLayout);

        const auto colorSize = AlignUp(static_cast<std::uint32_t>(colorLayout.getSize()), colorLayout.getAlignment());
        texture.ImageMemBlock = dk::MemBlockMaker{ device, AlignUp(colorSize, DK_MEMBLOCK_ALIGNMENT) }
                                    .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                                    .create();
        texture.Image.initialize(colorLayout, texture.ImageMemBlock, 0);
        texture.Width = width;
        texture.Height = height;

        // Repoint this texture's descriptor slot at the new image and force a cache invalidate before it is sampled.
        mImageDescriptors[TextureId].initialize(dk::ImageView{ texture.Image });
        mIsDescriptorsDirty = true;
    }

    if (hasDepthBuffer && (isSizeChanged || !HasDepth)) {
        dk::ImageLayout depthLayout = {};
        dk::ImageLayoutMaker{ device }
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
            .setFormat(DkImageFormat_Z24S8)
            .setDimensions(width, height)
            .initialize(depthLayout);

        const auto depthSize = AlignUp(static_cast<std::uint32_t>(depthLayout.getSize()), depthLayout.getAlignment());
        DepthMemBlock = dk::MemBlockMaker{ device, AlignUp(depthSize, DK_MEMBLOCK_ALIGNMENT) }
                            .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                            .create();
        DepthImage.initialize(depthLayout, DepthMemBlock, 0);
    }

    Width = width;
    Height = height;
    HasDepth = hasDepthBuffer;
    IsYInverted = openglInvertY;
}

void GfxRenderingApiDeko3d::StartDrawToFramebuffer(int fbId, float noiseScale) {
    if (fbId < 0 || fbId >= static_cast<std::int32_t>(mFramebuffers.size())) {
        return;
    }

    auto cb = mFrameCmdBuf;

    // Flush the previous target's fragment writes and invalidate the texture/descriptor caches before binding the new
    // one, so a later draw that samples the just-rendered target (ImGui compositing mGameFb; FB effects in a later
    // slice) reads finished, non-stale texels.  A handful per frame -- cheap against total frame cost.
    cb.barrier(DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);

    if (fbId == 0) {
        const std::int32_t slot = mWindowBackend->GetCurrentImageSlot();
        dk::ImageView colorView{ mWindowBackend->GetFramebuffer(slot) };
        const dk::ImageView depthView{ mWindowBackend->GetDepthBuffer(slot) };
        cb.bindRenderTargets({ &colorView }, &depthView);
        mWindowBackend->GetDimensions(&mRenderTargetWidth, &mRenderTargetHeight, nullptr, nullptr);
    } else {
        const auto& fb = mFramebuffers[fbId];
        dk::ImageView colorView{ mTextures[fb.TextureId].Image };

        if (fb.HasDepth) {
            const dk::ImageView depthView{ fb.DepthImage };
            cb.bindRenderTargets({ &colorView }, &depthView);
        } else {
            cb.bindRenderTargets({ &colorView }, nullptr);
        }

        mRenderTargetWidth = fb.Width;
        mRenderTargetHeight = fb.Height;
    }

    mCurrentFb = fbId;

    // TODO: ubershader has no noise term yet
    static_cast<void>(noiseScale);
}

void GfxRenderingApiDeko3d::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                            int dstX0, int dstY0, int dstX1, int dstY1) {
    if (fbSrcId < 0 || fbSrcId >= static_cast<std::int32_t>(mFramebuffers.size()) || fbDstId < 0 ||
        fbDstId >= static_cast<std::int32_t>(mFramebuffers.size())) {
        return;
    }

    // Resolve each ID to its color image + dimensions.  fb 0 is the window's current swap chain slot; offscreen IDs
    // use their reserved mTextures slot.
    const auto Resolve = [&](std::int32_t fbId, dk::Image& outImage, std::uint32_t& outW, std::uint32_t& outH) -> bool {
        if (fbId == 0) {
            const auto slot = mWindowBackend->GetCurrentImageSlot();
            if (slot < 0) {
                return false;
            }

            outImage = mWindowBackend->GetFramebuffer(slot);
            mWindowBackend->GetDimensions(&outW, &outH, nullptr, nullptr);
            return true;
        }

        const auto& fb = mFramebuffers[fbId];
        if (fb.TextureId < 0) {
            return false;
        }

        outImage = mTextures[fb.TextureId].Image;
        outW = mTextures[fb.TextureId].Width;
        outH = mTextures[fb.TextureId].Height;
        return true;
    };

    dk::Image srcImage = {};
    dk::Image dstImage = {};
    std::uint32_t srcW = 0;
    std::uint32_t srcH = 0;
    std::uint32_t dstW = 0;
    std::uint32_t dstH = 0;

    if (!Resolve(fbSrcId, srcImage, srcW, srcH) || !Resolve(fbDstId, dstImage, dstW, dstH)) {
        return;
    }

    // Clamp both regions to their image bounds (interpreter coords are top-left, matching deko3d's copy convention;
    // no Y-flip -- consistent with the top-left origin used throughout this backend).
    const auto ClampRect = [](std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, std::uint32_t w,
                              std::uint32_t h, std::uint32_t& ox, std::uint32_t& oy, std::uint32_t& ow,
                              std::uint32_t& oh) {
        const auto cx0 = std::clamp(x0, 0, static_cast<std::int32_t>(w));
        const auto cy0 = std::clamp(y0, 0, static_cast<std::int32_t>(h));
        const auto cx1 = std::clamp(x1, 0, static_cast<std::int32_t>(w));
        const auto cy1 = std::clamp(y1, 0, static_cast<std::int32_t>(h));
        ox = static_cast<std::uint32_t>(cx0);
        oy = static_cast<std::uint32_t>(cy0);
        ow = static_cast<std::uint32_t>(std::max(0, cx1 - cx0));
        oh = static_cast<std::uint32_t>(std::max(0, cy1 - cy0));
    };

    std::uint32_t sx = 0;
    std::uint32_t sy = 0;
    std::uint32_t sw = 0;
    std::uint32_t sh = 0;
    std::uint32_t dx = 0;
    std::uint32_t dy = 0;
    std::uint32_t dw = 0;
    std::uint32_t dh = 0;

    ClampRect(srcX0, srcY0, srcX1, srcY1, srcW, srcH, sx, sy, sw, sh);
    ClampRect(dstX0, dstY0, dstX1, dstY1, dstW, dstH, dx, dy, dw, dh);

    if (sw == 0 || sh == 0) {
        return;
    }

    // deko3d copyImage is a 1:1 transfer -- extents must match.
    const std::uint32_t width = std::min(sw, dw);
    const std::uint32_t height = std::min(sh, dh);

    // TODO
    if (sw != dw || sh != dh) {
        mWindowBackend->Trace("CopyFramebuffer: scaled copy requested; skipping");
        return;
    }

    auto cb = mFrameCmdBuf;
    // Flush any pending fragment writes to the source before the transfer reads it.  copyImage is not a render-pass
    // operation and does not disturb the currently bound render target, so no rebind afterward.
    cb.barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);

    const DkImageRect srcRect = { sx, sy, 0, width, height, 1 };
    const DkImageRect dstRect = { dx, dy, 0, width, height, 1 };

    cb.copyImage(dk::ImageView{ srcImage }, srcRect, dk::ImageView{ dstImage }, dstRect);

    // The destination's contents changed; force a texture-cache invalidate before it is next sampled (reuses the
    // descriptor-dirty path, whose barrier invalidates the image cache).
    mIsDescriptorsDirty = true;
}

void GfxRenderingApiDeko3d::ClearFramebuffer(bool color, bool depth) {
    if (!color && !depth) {
        return;
    }

    auto cb = mFrameCmdBuf;

    // Clears honor the scissor; widen viewport+scissor to the whole current target so the clear covers it regardless
    // of the last draw's scissor.  The interpreter re-emits viewport/scissor after this (viewport_or_scissor_changed),
    // so this does not leak into subsequent draws.
    cb.setViewports(0, { { 0.0f, 0.0f, static_cast<float>(mRenderTargetWidth), static_cast<float>(mRenderTargetHeight),
                           0.0f, 1.0f } });
    cb.setScissors(0, { { 0, 0, mRenderTargetWidth, mRenderTargetHeight } });

    if (color) {
        cb.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    }
    if (depth) {
        cb.clearDepthStencil(true, 1.0f, 0xFF, 0); // far = 1.0 in 0..1 depth
    }
}

void GfxRenderingApiDeko3d::ReadFramebufferToCPU(int fbId, std::uint32_t width, std::uint32_t height,
                                                 std::uint16_t* rgba16Buf) {
}

void GfxRenderingApiDeko3d::ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) {
}

std::unordered_map<std::pair<float, float>, std::uint16_t, hash_pair_ff>
GfxRenderingApiDeko3d::GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) {
    // Real depth readback isn't implemented yet, but Interpreter::GetPixelDepth() does find(coord)->second on this
    // result with no end() check.  An empty map makes that dereference end().  Return a defined placeholder for
    // every requested coordinate so the lookup resolves.  Replaced by a true readback with the real backend.
    std::unordered_map<std::pair<float, float>, std::uint16_t, hash_pair_ff> result;
    for (const auto& coord : coordinates) {
        result.emplace(coord, 0); // 0 reads as near; 0xFFFF reads as empty/far.
    }

    return result;
}

void* GfxRenderingApiDeko3d::GetFramebufferTextureId(int fbId) {
    // Same handle encoding as GetTextureById: the interpreter stores this as mGfxFrameBuffer and passes it to
    // ImGui::Image, which flows through RenderDrawData as a DkResHandle into the shared descriptor set.  fb 0 (window)
    // owns no sampled surface -> null (the interpreter only requests this for offscreen targets).
    if (fbId <= 0 || fbId >= static_cast<std::int32_t>(mFramebuffers.size())) {
        return nullptr;
    }

    const auto& fb = mFramebuffers[fbId];
    if (fb.TextureId < 0) {
        return nullptr;
    }

    const auto handle =
        dkMakeTextureHandle(static_cast<std::uint32_t>(fb.TextureId), mTextures[fb.TextureId].SamplerIndex);
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle));
}

void GfxRenderingApiDeko3d::SelectTextureFb(int fbId) {
    // Bind the FB's color surface as tile 0's sampled texture (game sampling a framebuffer: reflections, subscreen,
    // freeze-frame).  Its descriptor already exists (UpdateFramebufferParameters) and its writes were flushed by the
    // RT-switch barrier when the game stopped rendering to it, so no extra barrier here.  fb 0 owns no sampled
    // surface.
    if (fbId <= 0 || fbId >= static_cast<std::int32_t>(mFramebuffers.size()) || mFramebuffers[fbId].TextureId < 0) {
        return;
    }

    SelectTexture(0, static_cast<std::uint32_t>(mFramebuffers[fbId].TextureId));
}

// --------------------------------------------------------------------------------------------------------------------
// ImGui renderer
// --------------------------------------------------------------------------------------------------------------------

void GfxRenderingApiDeko3d::EnsureImGuiFontsUploaded() {
    const auto& io = ImGui::GetIO();

    // Current and resident -> nothing to do.  SoH dirties the atlas (IsBuilt() -> false) by adding fonts after
    // Gui::Init(); NewFrame calls this every frame, and we re-upload only on that transition.
    if (io.Fonts->IsBuilt() && mImguiFontTexId >= 0) {
        return;
    }

    std::uint8_t* pixels = nullptr;
    std::int32_t width = 0;
    std::int32_t height = 0;

    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height); // (re)builds the atlas as a side effect
    if (!pixels || width <= 0 || height <= 0) {
        return;
    }

    if (mImguiFontTexId < 0) {
        mImguiFontTexId = static_cast<std::int32_t>(NewTexture()); // Reserve a permanent slot the first time only
    }

    // UploadTexture keys off the current tile's selected ID; point tile 0 at the font slot for the upload, then
    // restore so we don't perturb whatever tile state a caller had set.
    const auto savedTile = mCurrentTile;
    const auto savedId = mCurrentTextureIds[0];
    SelectTexture(0, static_cast<std::uint32_t>(mImguiFontTexId));
    UploadTexture(pixels, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    mCurrentTile = savedTile;
    mCurrentTextureIds[0] = savedId;

    // Glyph sampling: linear + clamp on both axes -> the corresponding immutable sampler slot.
    mTextures[mImguiFontTexId].SamplerIndex = SamplerIndex(true, G_TX_CLAMP, G_TX_CLAMP);
    io.Fonts->SetTexID(GetTextureById(mImguiFontTexId));
}

void GfxRenderingApiDeko3d::RenderDrawData(ImDrawData* drawData) {
    // Index format below assumes 16-bit indices.  SoH's ImGui config leaves ImDrawIdx at the default; assert so a
    // future config change (32-bit) fails the build instead of silently corrupting geometry.
    static_assert(sizeof(ImDrawIdx) == sizeof(std::uint16_t),
                  "ImGui index width changed; switch the DkIdxFormat below to UInt32");

    if (!drawData || drawData->CmdListsCount == 0 || drawData->TotalVtxCount == 0) {
        return;
    }

    auto cb = mFrameCmdBuf;
    const auto device = mWindowBackend->GetDevice();

    // Grow the per-ring vertex/index buffers to fit this frame (2x headroom, matching the game vtx ring's
    // amortization).
    const std::size_t vtxBytes = static_cast<std::size_t>(drawData->TotalVtxCount) * sizeof(ImDrawVert);
    const std::size_t idxBytes = static_cast<std::size_t>(drawData->TotalIdxCount) * sizeof(ImDrawIdx);

    if (!mImguiVtxMemBlock[mRing] || mImguiVtxMemBlock[mRing].getSize() < vtxBytes) {
        mImguiVtxMemBlock[mRing] = nullptr; // Release before realloc; the ring's frame fence already gates reuse.
        mImguiVtxMemBlock[mRing] =
            dk::MemBlockMaker{ device, AlignUp(std::max<std::uint32_t>(static_cast<std::uint32_t>(2 * vtxBytes),
                                                                       DK_MEMBLOCK_ALIGNMENT),
                                               DK_MEMBLOCK_ALIGNMENT) }
                .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                .create();
    }
    if (!mImguiIdxMemBlock[mRing] || mImguiIdxMemBlock[mRing].getSize() < idxBytes) {
        mImguiIdxMemBlock[mRing] = nullptr;
        mImguiIdxMemBlock[mRing] =
            dk::MemBlockMaker{ device, AlignUp(std::max<std::uint32_t>(static_cast<std::uint32_t>(2 * idxBytes),
                                                                       DK_MEMBLOCK_ALIGNMENT),
                                               DK_MEMBLOCK_ALIGNMENT) }
                .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                .create();
    }

    std::uint32_t fbWidth = 0;
    std::uint32_t fbHeight = 0;
    mWindowBackend->GetDimensions(&fbWidth, &fbHeight, nullptr, nullptr);

    // fb 0 is still the bound render target (StartFrame bound it; nothing rebinds it in this slice).  Set every piece
    // of pipeline state ImGui needs explicitly rather than inheriting the game's last-draw state.  The blend equation
    // is deliberately not re-bound: it is frame-global in this backend (bound once in StartFrame, standard
    // over-blend), and ImGui wants the same, so we only flip the per-target blend-enable via ColorState.
    cb.bindShaders(DkStageFlag_GraphicsMask, { &mWindowBackend->GetImGuiVsh(), &mWindowBackend->GetImGuiFsh() });
    cb.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
    cb.bindColorState(dk::ColorState{}.setBlendEnable(0, true));
    cb.bindColorWriteState(dk::ColorWriteState{});
    cb.bindDepthStencilState(dk::DepthStencilState{}.setDepthTestEnable(false).setDepthWriteEnable(false));
    cb.setViewports(0, { { 0.0f, 0.0f, static_cast<float>(fbWidth), static_cast<float>(fbHeight), 0.0f, 1.0f } });

    // Orthographic projection from ImGui pixel space (origin top-left, y down; using DisplayPos/DisplaySize so the SoH
    // on-screen-keyboard y-offset applied to DisplayPos is honored) to deko3d clip space.  Column-major mat4;
    // translation in column 3.  ImGui z is 0, so the z row is correctness only.
    const auto left = drawData->DisplayPos.x;
    const auto right = drawData->DisplayPos.x + drawData->DisplaySize.x;
    const auto top = drawData->DisplayPos.y;
    const auto bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;

    float projection[16] = {};
    projection[0] = 2.0f / (right - left);
    projection[5] = 2.0f / (top - bottom);
    projection[10] = -0.5f;
    projection[12] = -(right + left) / (right - left);
    projection[13] = -(top + bottom) / (top - bottom);
    projection[14] = 0.5f;
    projection[15] = 1.0f;

    const auto uboOff = AlignUp(mUboRingOffset[mRing], DK_UNIFORM_BUF_ALIGNMENT);
    if (uboOff + sizeof(projection) > sUniformRingSize) {
        mWindowBackend->Trace("RenderDrawData: UBO ring overflow (bump sUniformRingSize)");
        return;
    }

    std::memcpy(mUboRingCpu[mRing] + uboOff, projection, sizeof(projection));
    mUboRingOffset[mRing] = uboOff + sUniformSlotSize;
    cb.bindUniformBuffer(DkStage_Vertex, 0, mUboRingGpu[mRing] + uboOff, sUniformSlotSize);

    constexpr std::array attribs = {
        DkVtxAttribState{ 0, 0, offsetof(ImDrawVert, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
        DkVtxAttribState{ 0, 0, offsetof(ImDrawVert, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
        DkVtxAttribState{ 0, 0, offsetof(ImDrawVert, col), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0 },
    };

    cb.bindVtxAttribState(dk::detail::ArrayProxy(attribs.size(), attribs.data()));
    cb.bindVtxBufferState({ { sizeof(ImDrawVert), 0 } });
    cb.bindVtxBuffer(0, mImguiVtxMemBlock[mRing].getGpuAddr(), mImguiVtxMemBlock[mRing].getSize());
    cb.bindIdxBuffer(DkIdxFormat_Uint16, mImguiIdxMemBlock[mRing].getGpuAddr());

    // Make ImGui self-sufficient about descriptor freshness rather than leaning on a preceding RT-switch invalidate:
    // the font atlas (and any FB color surface it samples) writes its descriptor with mIsDescriptorsDirty set, and
    // ImGui may be the first consumer to sample it this frame.
    if (mIsDescriptorsDirty) {
        cb.barrier(DkBarrier_None, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
        mIsDescriptorsDirty = false;
    }

    const auto vtxBase = static_cast<std::uint8_t*>(mImguiVtxMemBlock[mRing].getCpuAddr());
    const auto idxBase = static_cast<std::uint8_t*>(mImguiIdxMemBlock[mRing].getCpuAddr());

    std::size_t vtxByteOff = 0;
    std::size_t idxByteOff = 0;
    DkResHandle boundHandle = ~0u; // Force the first bindTextures; ~0 is not a valid dkMakeTextureHandle value.

    const ImVec2 clipOff = drawData->DisplayPos;

    for (std::int32_t n = 0; n < drawData->CmdListsCount; ++n) {
        const auto list = drawData->CmdLists[n];
        const std::size_t listVtxBytes = static_cast<std::size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        const std::size_t listIdxBytes = static_cast<std::size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);

        std::memcpy(vtxBase + vtxByteOff, list->VtxBuffer.Data, listVtxBytes);
        std::memcpy(idxBase + idxByteOff, list->IdxBuffer.Data, listIdxBytes);

        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                cmd.UserCallback(list, &cmd); // i.e., ImDrawCallback_ResetRenderState-style hooks
                continue;
            }

            // Clip rect -> scissor, shifted by DisplayPos and clamped to the render target (deko3d rejects scissors
            // outside the target).  Drop fully-clipped commands.
            const auto x0 = std::clamp(static_cast<std::int32_t>(cmd.ClipRect.x - clipOff.x), 0,
                                       static_cast<std::int32_t>(fbWidth));
            const auto y0 = std::clamp(static_cast<std::int32_t>(cmd.ClipRect.y - clipOff.y), 0,
                                       static_cast<std::int32_t>(fbHeight));
            const auto x1 = std::clamp(static_cast<std::int32_t>(cmd.ClipRect.z - clipOff.x), 0,
                                       static_cast<std::int32_t>(fbWidth));
            const auto y1 = std::clamp(static_cast<std::int32_t>(cmd.ClipRect.w - clipOff.y), 0,
                                       static_cast<std::int32_t>(fbHeight));

            if (x1 <= x0 || y1 <= y0) {
                continue;
            }

            cb.setScissors(0, { { static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
                                  static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0) } });

            if (const auto handle = static_cast<DkResHandle>(reinterpret_cast<std::uintptr_t>(cmd.GetTexID()));
                handle != boundHandle) {
                boundHandle = handle;
                cb.bindTextures(DkStage_Fragment, 0, handle);
            }

            // VtxOffset is honored because ImGuiBackendFlags_RendererHasVtxOffset is set in Fast3dGui's deko3d init.
            cb.drawIndexed(DkPrimitive_Triangles, cmd.ElemCount, 1,
                           cmd.IdxOffset + static_cast<std::uint32_t>(idxByteOff / sizeof(ImDrawIdx)),
                           cmd.VtxOffset + static_cast<std::uint32_t>(vtxByteOff / sizeof(ImDrawVert)), 0);
        }

        vtxByteOff += listVtxBytes;
        idxByteOff += listIdxBytes;
    }

    // Flush the tiler's fragment writes to fb 0 before it is presented in SwapBuffersBegin.
    cb.barrier(DkBarrier_Fragments, 0);
}
} // namespace Fast
#endif