#if defined(ENABLE_DEKO3D)
#include "fast/backends/gfx_deko3d_window.h"

#include <vector>

#include <switch.h>
#include <spdlog/spdlog.h>

#include "fast/Fast3dGui.h"
#include "ship/Context.h"
#include "ship/port/switch/SwitchImpl.h"

namespace Fast {

namespace {
constexpr std::uint32_t gCmdMemSize = 0x10000;        // 64 KiB
constexpr std::uint32_t gShaderCodeMemSize = 0x10000; // 64 KiB
constexpr std::uint32_t gVtxMemSize = 0x1000;         // 4 KiB
constexpr std::uint32_t gFrameCmdMemSize = 0x10000;   // 64 KiB per ring slot

// .dksh on-disk layout: [control section (control_sz bytes, starts with this header)][code section (code_sz bytes)]
struct DkshHeader {
    std::uint32_t Magic = 0;
    std::uint32_t HeaderSz = 0;
    std::uint32_t ControlSz = 0;
    std::uint32_t CodeSz = 0;
    std::uint32_t ProgramsOff = 0;
    std::uint32_t NumPrograms = 0;
};

struct TracerVertex {
    float Pos[4];
    float Color[3];
};

static_assert(sizeof(TracerVertex) == 28, "vertex stride must match the shader's attribute layout");

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
    return value + alignment - 1 & ~(alignment - 1);
}

// Crash-synchronous trace.  The release logger is an async_logger, so its queue (and any flush_onrequest) is lost when
// deko3d's fatal handler calls svcBreak().  Open/write/close per line so the entry is durably on disk before any
// subsequent abort.
void Deko3dTrace(const char* message) {
    static const auto sPath = Ship::Context::GetPathRelativeToAppDirectory("logs/deko3d_trace.log");
    if (const auto file = std::fopen(sPath.c_str(), "a")) {
        std::fputs(message, file);
        std::fputc('\n', file);
        std::fclose(file);
    }

    SPDLOG_CRITICAL("{}", message);
}

// deko3d routes API misuse and fatal errors here.  Without a callback, the default response to a fatal is svcBreak().
void Deko3dDebugCallback(void* userData, const char* context, DkResult result, const char* message) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "[deko3d] result=%d, context='%s': %s", static_cast<int>(result),
                  context ? context : "(null)", message ? message : "(null)");
    Deko3dTrace(buffer);
}
} // namespace

GfxWindowBackendDeko3d::~GfxWindowBackendDeko3d() {
    GfxWindowBackendDeko3d::Destroy();
}

void GfxWindowBackendDeko3d::CreateDeko3dDevice() {
    mDevice = dk::DeviceMaker{}.setCbDebug(Deko3dDebugCallback).create();
    mQueue = dk::QueueMaker{ mDevice }.setFlags(DkQueueFlags_Graphics).create();

    mCmdMemBlock = dk::MemBlockMaker{ mDevice, AlignUp(gCmdMemSize, DK_MEMBLOCK_ALIGNMENT) }
                       .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                       .create();
    mCmdBuf = dk::CmdBufMaker{ mDevice }.create();
    mCmdBuf.addMemory(mCmdMemBlock, 0, gCmdMemSize);

    // GPU code memory for the tracer shaders.  Code memblocks reserve DK_SHADER_CODE_UNUSABLE_SIZE at the start, so we
    // begin allocating at the first DK_SHADER_CODE_ALIGNMENT boundary past it.
    mShaderCodeMemBlock = dk::MemBlockMaker{ mDevice, AlignUp(gShaderCodeMemSize, DK_MEMBLOCK_ALIGNMENT) }
                              .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code)
                              .create();
    mShaderCodeOffset = DK_SHADER_CODE_ALIGNMENT;

    // Static vertex buffer (clip-space positions, per-vertex color as the single input)
    mVtxMemBlock = dk::MemBlockMaker{ mDevice, AlignUp(gVtxMemSize, DK_MEMBLOCK_ALIGNMENT) }
                       .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
                       .create();

    constexpr TracerVertex vertices[3] = {
        { { 0.0f, 0.6f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f } },
        { { -0.6f, -0.6f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f } },
        { { 0.6f, -0.6f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } },
    };

    std::memcpy(mVtxMemBlock.getCpuAddr(), vertices, sizeof(vertices));
    mVtxGpuAddr = mVtxMemBlock.getGpuAddr();
    mVtxSize = sizeof(vertices);

    // Per-frame command memory + cmdbufs (one per swap chain image).
    for (std::uint8_t i = 0; i < sFramebuffers; ++i) {
        mFrameCmdMemBlock[i] = dk::MemBlockMaker{ mDevice, AlignUp(gFrameCmdMemSize, DK_MEMBLOCK_ALIGNMENT) }
                                   .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                   .create();
        mFrameCmdBuf[i] = dk::CmdBufMaker{ mDevice }.create();
        mFrameCmdBuf[i].addMemory(mFrameCmdMemBlock[i], 0, gFrameCmdMemSize);
    }
}

void GfxWindowBackendDeko3d::CreateSwapChain(std::uint32_t width, std::uint32_t height) {
    mWidth = width;
    mHeight = height;

    // All framebuffers share one layout (same dims/format), so size/alignment are identical.
    dk::ImageLayout fbLayout = {};
    dk::ImageLayoutMaker{ mDevice }
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(width, height)
        .initialize(fbLayout);

    const std::uint64_t fbSize = fbLayout.getSize();
    const std::uint32_t fbAlign = fbLayout.getAlignment();
    const std::uint32_t fbStride = AlignUp(static_cast<std::uint32_t>(fbSize), fbAlign);

    const std::uint32_t poolSize = AlignUp(fbStride * sFramebuffers, DK_MEMBLOCK_ALIGNMENT);
    mFbMemBlock =
        dk::MemBlockMaker{ mDevice, poolSize }.setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image).create();

    std::array<const DkImage*, sFramebuffers> swapChainImages = {};
    for (std::uint8_t i = 0; i < sFramebuffers; ++i) {
        mFramebuffers[i].initialize(fbLayout, mFbMemBlock, fbStride * i);
        swapChainImages[i] = &mFramebuffers[i];
    }

    mSwapChain = dk::SwapchainMaker{ mDevice, nwindowGetDefault(), swapChainImages }.create();

    RecordClearCommandLists();
}

void GfxWindowBackendDeko3d::RecordClearCommandLists() {
    // Pre-record one bind+clear list per framebuffer. The lists coexist in mCmdBuf's memory (no clear() between
    // recordings), so per frame we only acquire -> submit[slot] -> present.
    mCmdBuf.clear();

    for (std::uint8_t i = 0; i < sFramebuffers; ++i) {
        dk::ImageView view{ mFramebuffers[i] };
        mCmdBuf.bindRenderTargets({ &view }, nullptr);
        mCmdBuf.setViewports(0,
                             { { 0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f } });
        mCmdBuf.setScissors(0, { { 0, 0, mWidth, mHeight } });
        mCmdBuf.clearColor(0, DkColorMask_RGBA, 1.0f, 0.0f, 1.0f, 1.0f);

        mClearCmdLists[i] = mCmdBuf.finishList();
    }
}

DkCmdList GfxWindowBackendDeko3d::RecordFrameDrawList(std::uint32_t ringIndex) {
    dk::CmdBuf cb = mFrameCmdBuf[ringIndex];
    cb.clear();

    // RT/viewport/scissor are bound by the clear list submitted just before this one.  Queue state persists across
    // submits, so we only record pipeline + vertex state + the draw here.
    cb.bindShaders(DkStageFlag_GraphicsMask, { &mColorVsh, &mColorFsh });
    cb.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
    cb.bindColorState(dk::ColorState{});
    cb.bindColorWriteState(dk::ColorWriteState{});
    cb.bindDepthStencilState(dk::DepthStencilState{}.setDepthTestEnable(false));
    cb.bindVtxAttribState({
        { 0, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0 },
        { 0, 0, 16, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
    });
    cb.bindVtxBufferState({ { sizeof(TracerVertex), 0 } });
    cb.bindVtxBuffer(0, mVtxGpuAddr, mVtxSize);
    cb.draw(DkPrimitive_Triangles, 3, 1, 0, 0);

    return cb.finishList();
}

bool GfxWindowBackendDeko3d::LoadDeko3dShader(dk::Shader& shader, const char* path) {
    const auto file = std::fopen(path, "rb");
    if (!file) {
        return false;
    }

    DkshHeader dksh = {};
    if (std::fread(&dksh, sizeof(dksh), 1, file) != 1) {
        std::fclose(file);
        return false;
    }

    // Control section is consumed by dkShaderInitialize and not needed afterward.  It's the first control_sz bytes.
    std::vector<std::uint8_t> control(dksh.ControlSz);
    std::rewind(file);

    if (std::fread(control.data(), dksh.ControlSz, 1, file) != 1) {
        std::fclose(file);
        return false;
    }

    // Code section follows the control section in the file; it lives in GPU code memory.
    const std::uint32_t codeOffset = mShaderCodeOffset;
    if (codeOffset + dksh.CodeSz > gShaderCodeMemSize) {
        std::fclose(file);
        return false; // Code pool too small
    }

    std::fseek(file, dksh.ControlSz, SEEK_SET);
    {
        if (const auto codeCpu = static_cast<std::uint8_t*>(mShaderCodeMemBlock.getCpuAddr()) + codeOffset;
            std::fread(codeCpu, dksh.CodeSz, 1, file) != 1) {
            std::fclose(file);
            return false;
        }
    }
    std::fclose(file);

    dk::ShaderMaker{ mShaderCodeMemBlock, codeOffset }.setControl(control.data()).setProgramId(0).initialize(shader);

    mShaderCodeOffset += AlignUp(dksh.CodeSz, DK_SHADER_CODE_ALIGNMENT);
    return true;
}

void GfxWindowBackendDeko3d::DestroySwapChain() {
    if (mQueue) {
        mQueue.waitIdle();
    }

    mSwapChain = nullptr;
    mFbMemBlock = nullptr;
    mClearCmdLists = {};
}

void GfxWindowBackendDeko3d::Init(const char* gameName, const char* apiName, bool startFullScreen, std::uint32_t width,
                                  std::uint32_t height, std::int32_t posX, std::int32_t posY) {
    Deko3dTrace("Init: enter");

    // Input/events only
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        SPDLOG_ERROR("Failed to init SDL input: {}", SDL_GetError());
    }

    mWidth = width ? width : 1280;
    mHeight = height ? height : 720;

    Deko3dTrace("Init: creating device/queue/cmdbuf");
    CreateDeko3dDevice();

    // SoH normally doesn't mount the nro's embedded romfs, so do it here for the .dksh blobs.  Load before
    // CreateSwapChain() because RecordClearCommandLists() bind these shaders.
    if (R_FAILED(romfsInit())) {
        Deko3dTrace("Init: romfsInit failed (no embedded romfs?)");
    }

    if (!LoadDeko3dShader(mColorVsh, "romfs:/shaders/deko3d/color.vert.dksh")) {
        Deko3dTrace("Init: color vsh load failed");
    }

    if (!LoadDeko3dShader(mColorFsh, "romfs:/shaders/deko3d/color.frag.dksh")) {
        Deko3dTrace("Init: color fsh load failed");
    }

    Deko3dTrace("Init: creating swap chain");
    CreateSwapChain(mWidth, mHeight);
    mIsInitialized = true;

    // Wire up the ImGui frontend with the same handshake very other backend performs at the end of its Init().
    // Without this, Gui::Init()/ImGui::CreateContext() never run and the first ImGui call (overlay->LoadFont during
    // OTRGlobals ctor) dereferences a null context.  deko3d's Fast3dGui cases read only Backend, not the handle union,
    // so that's all we populate.
    GuiWindowInitData windowImpl = {};
    windowImpl.Backend = FAST3D_DEKO3D;
    std::dynamic_pointer_cast<Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui())->Init(windowImpl);

    Deko3dTrace("Init: done");
}

void GfxWindowBackendDeko3d::SwapBuffersBegin() {
    if (!mIsInitialized) {
        return;
    }

    mCurrentSlot = mQueue.acquireImage(mSwapChain);

    const std::uint32_t ring = mFrameIndex % sFramebuffers;
    // Gate reuse of the ring slot's command memory on the GPU having finishes its previous use.
    if (mIsFrameFenceValid[ring]) {
        mFrameFence[ring].wait();
    }

    const auto drawList = RecordFrameDrawList(ring);

    mQueue.submitCommands(mClearCmdLists[mCurrentSlot]); // Binds RT[slot] + viewport + scissor + clear
    mQueue.submitCommands(drawList);                     // Draws inherit that bound state
    mQueue.signalFence(mFrameFence[ring]);               // Signals after the draws complete
    mIsFrameFenceValid[ring] = true;

    mQueue.presentImage(mSwapChain, mCurrentSlot);
    ++mFrameIndex;
}

void GfxWindowBackendDeko3d::SwapBuffersEnd() {
    mCurrentSlot = -1;
}

void GfxWindowBackendDeko3d::Destroy() {
    if (!mIsInitialized) {
        return;
    }

    if (mQueue) {
        mQueue.waitIdle(); // Ensure no ring memory is in flight before teardown
    }

    DestroySwapChain();

    for (std::uint8_t i = 0; i < sFramebuffers; ++i) {
        mFrameCmdBuf[i] = nullptr;
        mFrameCmdMemBlock[i] = nullptr;
    }

    mCmdBuf = nullptr;
    mCmdMemBlock = nullptr;
    mShaderCodeMemBlock = nullptr;
    mVtxMemBlock = nullptr;
    mQueue = nullptr;
    mDevice = nullptr;
    mIsInitialized = false;
}

void GfxWindowBackendDeko3d::Close() {
    mIsRunning = false;
}

bool GfxWindowBackendDeko3d::IsRunning() {
    return mIsRunning && Ship::Switch::IsRunning();
}

void GfxWindowBackendDeko3d::HandleEvents() {
    // Pump SDL controller/joystick events; the LUS controller-mapping layer consumes them.
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            mIsRunning = false;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------
// Window
// --------------------------------------------------------------------------------------------------------------------

void GfxWindowBackendDeko3d::GetDimensions(std::uint32_t* width, std::uint32_t* height, std::int32_t* posX,
                                           std::int32_t* posY) {
    if (width) {
        *width = mWidth;
    }

    if (height) {
        *height = mHeight;
    }

    if (posX) {
        *posX = 0;
    }

    if (posY) {
        *posY = 0;
    }
}

void GfxWindowBackendDeko3d::SetDimensions(std::uint32_t width, std::uint32_t height, std::int32_t posX,
                                           std::int32_t posY) {
    if ((width == mWidth && height == mHeight) || width == 0 || height == 0) {
        return;
    }

    DestroySwapChain();
    CreateSwapChain(width, height);
}

void GfxWindowBackendDeko3d::GetActiveWindowRefreshRate(std::uint32_t* refreshRate) {
    if (refreshRate) {
        *refreshRate = 60;
    }
}

Ship::WindowRect GfxWindowBackendDeko3d::GetPrimaryMonitorRect() {
    return { 0, 0, static_cast<std::int32_t>(mWidth), static_cast<std::int32_t>(mHeight) };
}

// --------------------------------------------------------------------------------------------------------------------
// Timing
// --------------------------------------------------------------------------------------------------------------------

double GfxWindowBackendDeko3d::GetTime() {
    return 0.0;
}

int GfxWindowBackendDeko3d::GetTargetFps() {
    return mTargetFps;
}

void GfxWindowBackendDeko3d::SetTargetFps(int fps) {
    mTargetFps = fps;
}

void GfxWindowBackendDeko3d::SetMaxFrameLatency(int latency) {
}

bool GfxWindowBackendDeko3d::IsFrameReady() {
    return true;
}

bool GfxWindowBackendDeko3d::CanDisableVsync() {
    return false; // deko3d present is vsynced via the swap chain.
}

// --------------------------------------------------------------------------------------------------------------------
// Fullscreen
// --------------------------------------------------------------------------------------------------------------------

void GfxWindowBackendDeko3d::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool)) {
}

void GfxWindowBackendDeko3d::SetFullscreen(bool fullscreen) {
}

bool GfxWindowBackendDeko3d::IsFullscreen() {
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// deko3d
// --------------------------------------------------------------------------------------------------------------------

dk::Device GfxWindowBackendDeko3d::GetDevice() const {
    return mDevice;
}

dk::Queue GfxWindowBackendDeko3d::GetQueue() const {
    return mQueue;
}

int GfxWindowBackendDeko3d::GetCurrentImageSlot() const {
    return mCurrentSlot;
}

const dk::Image& GfxWindowBackendDeko3d::GetFramebuffer(int slot) const {
    return mFramebuffers[slot];
}

// --------------------------------------------------------------------------------------------------------------------
// Keyboard (no-op)
// --------------------------------------------------------------------------------------------------------------------

void GfxWindowBackendDeko3d::SetKeyboardCallbacks(bool (*onKeyDown)(int), bool (*onKeyUp)(int), void (*onAllKeysUp)()) {
    mOnAllKeysUp = onAllKeysUp;
}

const char* GfxWindowBackendDeko3d::GetKeyName(int scancode) {
    return "";
}

// --------------------------------------------------------------------------------------------------------------------
// Mouse (no-op)
// --------------------------------------------------------------------------------------------------------------------

void GfxWindowBackendDeko3d::SetMouseCallbacks(bool (*onMouseButtonDown)(int), bool (*onMouseButtonUp)(int)) {
}

void GfxWindowBackendDeko3d::SetCursorVisibility(bool visability) {
}

void GfxWindowBackendDeko3d::SetMousePos(std::int32_t posX, std::int32_t posY) {
}

void GfxWindowBackendDeko3d::GetMousePos(std::int32_t* x, std::int32_t* y) {
    if (x) {
        *x = 0;
    }
    if (y) {
        *y = 0;
    }
}

void GfxWindowBackendDeko3d::GetMouseDelta(std::int32_t* x, std::int32_t* y) {
    if (x) {
        *x = 0;
    }

    if (y) {
        *y = 0;
    }
}

void GfxWindowBackendDeko3d::GetMouseWheel(float* x, float* y) {
    if (x) {
        *x = 0.0f;
    }

    if (y) {
        *y = 0.0f;
    }
}

bool GfxWindowBackendDeko3d::GetMouseState(std::uint32_t btn) {
    return false;
}

void GfxWindowBackendDeko3d::SetMouseCapture(bool capture) {
}

bool GfxWindowBackendDeko3d::IsMouseCaptured() {
    return false;
}
} // namespace Fast
#endif