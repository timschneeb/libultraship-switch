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
constexpr std::uint32_t gShaderCodeMemSize = 0x20000; // 128 KiB
constexpr std::uint32_t gFrameCmdMemSize = 0x400000;  // 4 MiB per slot

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

constexpr std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
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

// ---------

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t gDbgFence = 0;
std::int64_t gDbgAcquire = 0;
std::int64_t gDbgSubmit = 0;
std::int64_t gDbgPresent = 0;
std::int64_t gDbgRecord = 0;
std::int64_t gDbgFenceMax = 0;
std::int64_t gDbgAcquireMax = 0;
std::int64_t gDbgPresentMax = 0;
std::int64_t gDbgRecordStart = 0;
std::int32_t gDbgFrames = 0;
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

    for (std::uint8_t i = 0; i < sFramebuffers; ++i) {
        mFrameCmdBuf[i] = dk::CmdBufMaker{ mDevice }.create();

        mFrameCmdMemBlock[i] = dk::MemBlockMaker{ mDevice, AlignUp(gFrameCmdMemSize, DK_MEMBLOCK_ALIGNMENT) }
                                   .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                   .create();

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

    const auto fbSize = fbLayout.getSize();
    const auto fbAlign = fbLayout.getAlignment();
    const auto fbStride = AlignUp(static_cast<std::uint32_t>(fbSize), fbAlign);

    const auto poolSize = AlignUp(fbStride * sFramebuffers, DK_MEMBLOCK_ALIGNMENT);
    mFbMemBlock =
        dk::MemBlockMaker{ mDevice, poolSize }.setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image).create();

    std::array<const DkImage*, sFramebuffers> swapChainImages = {};
    for (std::uint8_t i = 0; i < sFramebuffers; ++i) {
        mFramebuffers[i].initialize(fbLayout, mFbMemBlock, fbStride * i);
        swapChainImages[i] = &mFramebuffers[i];
    }

    // Depth buffer per swap chain image.  Rendering is double-buffered (sFrameBuffers in flight), so each frame needs
    // its own depth target; the frame fence already gates reuse of slot i.
    dk::ImageLayout depthLayout = {};
    dk::ImageLayoutMaker{ mDevice }
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_Z24S8)
        .setDimensions(width, height)
        .initialize(depthLayout);

    const auto depthSize = depthLayout.getSize();
    const auto depthAlign = depthLayout.getAlignment();
    const auto depthStride = AlignUp(static_cast<std::uint32_t>(depthSize), depthAlign);
    const auto depthPoolSize = AlignUp(depthStride * sFramebuffers, DK_MEMBLOCK_ALIGNMENT);

    mDepthMemBlock = dk::MemBlockMaker{ mDevice, depthPoolSize }
                         .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                         .create();

    for (std::uint8_t i = 0; i < sFramebuffers; ++i) {
        mDepthBuffers[i].initialize(depthLayout, mDepthMemBlock, depthStride * i);
    }

    mSwapChain = dk::SwapchainMaker{ mDevice, nwindowGetDefault(), swapChainImages }.create();

    RecordClearCommandLists();
}

void GfxWindowBackendDeko3d::RecordClearCommandLists() {
    mCmdBuf.clear();

    for (std::uint8_t i = 0; i < sFramebuffers; ++i) {
        dk::ImageView colorView{ mFramebuffers[i] };
        dk::ImageView depthView{ mDepthBuffers[i] };

        mCmdBuf.bindRenderTargets({ &colorView }, &depthView);
        mCmdBuf.setViewports(0,
                             { { 0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f } });
        mCmdBuf.setScissors(0, { { 0, 0, mWidth, mHeight } });
        mCmdBuf.clearColor(0, DkColorMask_RGBA, 1.0f, 0.0f, 1.0f, 1.0f);
        mCmdBuf.clearDepthStencil(true, 1.0f, 0xFF, 0); // far = 1.0 in 0..1 depth

        mClearCmdLists[i] = mCmdBuf.finishList();
    }
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
void GfxWindowBackendDeko3d::SyncFrameRateWithTime() {
    const std::int32_t fps = mTargetFps > 0 ? mTargetFps : 60;
    const std::int64_t frameNs = 1000000000LL / fps;

    const auto now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const auto next = mPreviousTimeNs + frameNs;
    const auto left = next - now;

    if (left > 0) {
        const timespec spec = { static_cast<std::time_t>(left / 1000000000LL), static_cast<long>(left % 1000000000LL) };
        nanosleep(&spec, nullptr);
    }

    auto after =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();

    // If we slept and woke within ~1ms of the deadline, snap to it so wake up latency doesn't drag the rate below
    // target over time.
    if (left > 0 && after - next < 1000000LL) {
        after = next;
    }

    mPreviousTimeNs = after;
}

void GfxWindowBackendDeko3d::DestroySwapChain() {
    if (mQueue) {
        mQueue.waitIdle();
    }

    mSwapChain = nullptr;
    mFbMemBlock = nullptr;
    mDepthMemBlock = nullptr;
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

    if (!LoadDeko3dShader(mColorVsh, "romfs:/shaders/color.vert.dksh")) {
        Deko3dTrace("Init: color vsh load failed");
    }

    if (!LoadDeko3dShader(mColorFsh, "romfs:/shaders/color.frag.dksh")) {
        Deko3dTrace("Init: color fsh load failed");
    }

    if (!LoadDeko3dShader(mColorTextureVsh, "romfs:/shaders/color_texture.vert.dksh")) {
        Deko3dTrace("Init: color_texture vsh load failed");
    }

    if (!LoadDeko3dShader(mColorTextureFsh, "romfs:/shaders/color_texture.frag.dksh")) {
        Deko3dTrace("Init: color_texture fsh load failed");
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

    gDbgRecord += NowNs() - gDbgRecordStart;

    const auto a0 = NowNs();
    mCurrentSlot = mQueue.acquireImage(mSwapChain);
    const auto a1 = NowNs();
    gDbgAcquire += a1 - a0;

    if (a1 - a0 > gDbgAcquireMax) {
        gDbgAcquireMax = a1 - a0;
    }

    mQueue.submitCommands(mClearCmdLists[mCurrentSlot]); // RT + viewport + scissor + clear

    if (mHasFrameDrawList) {
        mQueue.submitCommands(mFrameDrawList); // rapi's draws, inheriting the bound state
    }

    mQueue.signalFence(mFrameFence[mRecordingRing]); // Signals after the draws complete
    mIsFrameFenceValid[mRecordingRing] = true;
    gDbgSubmit += NowNs() - a1;

    SyncFrameRateWithTime();

    const auto p0 = NowNs();
    mQueue.presentImage(mSwapChain, mCurrentSlot);
    const auto p1 = NowNs();
    gDbgPresent += p1 - p0;

    if (p1 - p0 > gDbgPresentMax) {
        gDbgPresentMax = p1 - p0;
    }

    if (++gDbgFrames >= 60) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "[deko-perf] avg us/60f: fence=%lld acquire=%lld submit=%lld present=%lld record=%lld | "
                      "max us: fence=%lld acquire=%lld present=%lld",
                      gDbgFence / 60 / 1000, gDbgAcquire / 60 / 1000, gDbgSubmit / 60 / 1000, gDbgPresent / 60 / 1000,
                      gDbgRecord / 60 / 1000, gDbgFenceMax / 1000, gDbgAcquireMax / 1000, gDbgPresentMax / 1000);

        Deko3dTrace(buf);
        gDbgFence = gDbgAcquire = gDbgSubmit = gDbgPresent = gDbgRecord = 0;
        gDbgFenceMax = gDbgAcquireMax = gDbgPresentMax = 0;
        gDbgFrames = 0;
    }

    ++mFrameIndex;
    mHasFrameDrawList = false;
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

void GfxWindowBackendDeko3d::SetTargetFps(int fps) {
    mTargetFps = fps;
}

void GfxWindowBackendDeko3d::SetMaxFrameLatency(int latency) {
}

double GfxWindowBackendDeko3d::GetTime() {
    return 0.0;
}

int GfxWindowBackendDeko3d::GetTargetFps() {
    return mTargetFps;
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

dk::CmdBuf GfxWindowBackendDeko3d::BeginFrameRecording() {
    mRecordingRing = mFrameIndex % sFramebuffers;

    const auto t0 = NowNs();

    if (mIsFrameFenceValid[mRecordingRing]) {
        mFrameFence[mRecordingRing].wait(); // Gate reuse of this ring slot's command memory
    }

    const auto t1 = NowNs();
    gDbgFence += t1 - t0;

    if (t1 - t0 > gDbgFenceMax) {
        gDbgFenceMax = t1 - t0;
    }

    mFrameCmdBuf[mRecordingRing].clear();

    gDbgRecordStart = NowNs();

    return mFrameCmdBuf[mRecordingRing];
}

void GfxWindowBackendDeko3d::EndFrameRecording(DkCmdList drawList) {
    mFrameDrawList = drawList;
    mHasFrameDrawList = true;
}

dk::Device GfxWindowBackendDeko3d::GetDevice() const {
    return mDevice;
}

dk::Queue GfxWindowBackendDeko3d::GetQueue() const {
    return mQueue;
}

const dk::Image& GfxWindowBackendDeko3d::GetFramebuffer(int slot) const {
    return mFramebuffers[slot];
}

int GfxWindowBackendDeko3d::GetCurrentImageSlot() const {
    return mCurrentSlot;
}

const dk::Shader& GfxWindowBackendDeko3d::GetColorVsh() const {
    return mColorVsh;
}

const dk::Shader& GfxWindowBackendDeko3d::GetColorFsh() const {
    return mColorFsh;
}

const dk::Shader& GfxWindowBackendDeko3d::GetColorTextureVsh() const {
    return mColorTextureVsh;
}

const dk::Shader& GfxWindowBackendDeko3d::GetColorTextureFsh() const {
    return mColorTextureFsh;
}

std::uint32_t GfxWindowBackendDeko3d::GetRecordingRing() const {
    return mRecordingRing;
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

void GfxWindowBackendDeko3d::Trace(const char* message) {
    Deko3dTrace(message);
}
} // namespace Fast
#endif