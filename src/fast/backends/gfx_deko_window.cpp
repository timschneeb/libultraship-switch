#if defined(ENABLE_DEKO3D)
#include "fast/backends/gfx_deko_window.h"

#include <switch.h>
#include <spdlog/spdlog.h>

#include "fast/Fast3dGui.h"
#include "ship/Context.h"
#include "ship/port/switch/SwitchImpl.h"

namespace Fast {

namespace {
constexpr std::uint32_t gCmdMemSize = 0x10000; // 64 KiB

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
    return value + alignment - 1 & ~(alignment - 1);
}

// Crash-synchronous trace.  The release logger is an async_logger, so its queue (and any flush_onrequest) is lost when
// deko3d's fatal handler calls svcBreak().  Open/write/close per line so the entry is durably on disk before any
// subsequent abort.
void DekoTrace(const char* message) {
    static const auto sPath = Ship::Context::GetPathRelativeToAppDirectory("logs/deko_trace.log");
    if (const auto file = std::fopen(sPath.c_str(), "a")) {
        std::fputs(message, file);
        std::fputc('\n', file);
        std::fclose(file);
    }

    SPDLOG_CRITICAL("{}", message);
}

// deko3d routes API misuse and fatal errors here.  Without a callback, the default response to a fatal is svcBreak().
void DekoDebugCallback(void* userData, const char* context, DkResult result, const char* message) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "[deko3d] result=%d, context='%s': %s", static_cast<int>(result),
                  context ? context : "(null)", message ? message : "(null)");
    DekoTrace(buffer);
}
} // namespace

GfxWindowBackendDeko::~GfxWindowBackendDeko() {
    Destroy();
}

void GfxWindowBackendDeko::CreateDekoDevice() {
    mDevice = dk::DeviceMaker{}.setCbDebug(DekoDebugCallback).create();
    mQueue = dk::QueueMaker{ mDevice }.setFlags(DkQueueFlags_Graphics).create();

    mCmdMemBlock = dk::MemBlockMaker{ mDevice, AlignUp(gCmdMemSize, DK_MEMBLOCK_ALIGNMENT) }
                       .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                       .create();
    mCmdBuf = dk::CmdBufMaker{ mDevice }.create();
    mCmdBuf.addMemory(mCmdMemBlock, 0, gCmdMemSize);
}

void GfxWindowBackendDeko::CreateSwapChain(std::uint32_t width, std::uint32_t height) {
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

void GfxWindowBackendDeko::RecordClearCommandLists() {
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

void GfxWindowBackendDeko::DestroySwapChain() {
    if (mQueue) {
        mQueue.waitIdle();
    }

    mSwapChain = nullptr;
    mFbMemBlock = nullptr;
    mClearCmdLists = {};
}

void GfxWindowBackendDeko::Init(const char* gameName, const char* apiName, bool startFullScreen, std::uint32_t width,
                                std::uint32_t height, std::int32_t posX, std::int32_t posY) {
    DekoTrace("Init: enter");

    // Input/events only
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        SPDLOG_ERROR("Failed to init SDL input: {}", SDL_GetError());
    }

    mWidth = width ? width : 1280;
    mHeight = height ? height : 720;

    DekoTrace("Init: creating device/queue/cmdbuf");
    CreateDekoDevice();
    DekoTrace("Init: creating swap chain");
    CreateSwapChain(mWidth, mHeight);
    mIsInitialized = true;

    // Wire up the ImGui frontend with the same handshake very other backend performs at the end of its Init().
    // Without this, Gui::Init()/ImGui::CreateContext() never run and the first ImGui call (overlay->LoadFont during
    // OTRGlobals ctor) dereferences a null context.  deko3d's Fast3dGui cases read only Backend, not the handle union,
    // so that's all we populate.
    GuiWindowInitData windowImpl = {};
    windowImpl.Backend = FAST3D_DEKO3D;
    std::dynamic_pointer_cast<Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui())->Init(windowImpl);

    DekoTrace("Init: done");
}

void GfxWindowBackendDeko::SwapBuffersBegin() {
    if (!mIsInitialized) {
        return;
    }

    static bool sIsFirst = true;
    const bool isTracing = sIsFirst;
    sIsFirst = false;

    // Acquire a free swap chain slot, submit its pre-recorded clear, present.
    if (isTracing) {
        DekoTrace("SwapBuffersBegin: acquireImage");
    }

    mCurrentSlot = mQueue.acquireImage(mSwapChain);

    if (isTracing) {
        DekoTrace("SwapBuffersBegin: submitCommands");
    }

    mQueue.submitCommands(mClearCmdLists[mCurrentSlot]);

    if (isTracing) {
        DekoTrace("SwapBuffersBegin: presentImage");
    }

    mQueue.presentImage(mSwapChain, mCurrentSlot);

    if (isTracing) {
        DekoTrace("SwapBuffersBegin: first frame done");
    }
}

void GfxWindowBackendDeko::SwapBuffersEnd() {
    mCurrentSlot = -1;
}

void GfxWindowBackendDeko::Destroy() {
    if (!mIsInitialized) {
        return;
    }

    DestroySwapChain();
    mCmdBuf = nullptr;
    mCmdMemBlock = nullptr;
    mQueue = nullptr;
    mDevice = nullptr;
    mIsInitialized = false;
}

void GfxWindowBackendDeko::Close() {
    mIsRunning = false;
}

bool GfxWindowBackendDeko::IsRunning() {
    return mIsRunning && Ship::Switch::IsRunning();
}

void GfxWindowBackendDeko::HandleEvents() {
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

void GfxWindowBackendDeko::GetDimensions(std::uint32_t* width, std::uint32_t* height, std::int32_t* posX,
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

void GfxWindowBackendDeko::SetDimensions(std::uint32_t width, std::uint32_t height, std::int32_t posX,
                                         std::int32_t posY) {
    if ((width == mWidth && height == mHeight) || width == 0 || height == 0) {
        return;
    }

    DestroySwapChain();
    CreateSwapChain(width, height);
}

void GfxWindowBackendDeko::GetActiveWindowRefreshRate(std::uint32_t* refreshRate) {
    if (refreshRate) {
        *refreshRate = 60;
    }
}

Ship::WindowRect GfxWindowBackendDeko::GetPrimaryMonitorRect() {
    return { 0, 0, static_cast<std::int32_t>(mWidth), static_cast<std::int32_t>(mHeight) };
}

// --------------------------------------------------------------------------------------------------------------------
// Timing
// --------------------------------------------------------------------------------------------------------------------

double GfxWindowBackendDeko::GetTime() {
    return 0.0;
}

int GfxWindowBackendDeko::GetTargetFps() {
    return mTargetFps;
}

void GfxWindowBackendDeko::SetTargetFps(int fps) {
    mTargetFps = fps;
}

void GfxWindowBackendDeko::SetMaxFrameLatency(int latency) {
}

bool GfxWindowBackendDeko::IsFrameReady() {
    return true;
}

bool GfxWindowBackendDeko::CanDisableVsync() {
    return false; // deko3d present is vsynced via the swap chain.
}

// --------------------------------------------------------------------------------------------------------------------
// Fullscreen
// --------------------------------------------------------------------------------------------------------------------

void GfxWindowBackendDeko::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool)) {
}

void GfxWindowBackendDeko::SetFullscreen(bool fullscreen) {
}

bool GfxWindowBackendDeko::IsFullscreen() {
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// deko3d
// --------------------------------------------------------------------------------------------------------------------

dk::Device GfxWindowBackendDeko::GetDevice() const {
    return mDevice;
}

dk::Queue GfxWindowBackendDeko::GetQueue() const {
    return mQueue;
}

int GfxWindowBackendDeko::GetCurrentImageSlot() const {
    return mCurrentSlot;
}

const dk::Image& GfxWindowBackendDeko::GetFramebuffer(int slot) const {
    return mFramebuffers[slot];
}

// --------------------------------------------------------------------------------------------------------------------
// Keyboard (no-op)
// --------------------------------------------------------------------------------------------------------------------

void GfxWindowBackendDeko::SetKeyboardCallbacks(bool (*onKeyDown)(int), bool (*onKeyUp)(int), void (*onAllKeysUp)()) {
    mOnAllKeysUp = onAllKeysUp;
}

const char* GfxWindowBackendDeko::GetKeyName(int scancode) {
    return "";
}

// --------------------------------------------------------------------------------------------------------------------
// Mouse (no-op)
// --------------------------------------------------------------------------------------------------------------------

void GfxWindowBackendDeko::SetMouseCallbacks(bool (*onMouseButtonDown)(int), bool (*onMouseButtonUp)(int)) {
}

void GfxWindowBackendDeko::SetCursorVisibility(bool visability) {
}

void GfxWindowBackendDeko::SetMousePos(std::int32_t posX, std::int32_t posY) {
}

void GfxWindowBackendDeko::GetMousePos(std::int32_t* x, std::int32_t* y) {
    if (x) {
        *x = 0;
    }
    if (y) {
        *y = 0;
    }
}

void GfxWindowBackendDeko::GetMouseDelta(std::int32_t* x, std::int32_t* y) {
    if (x) {
        *x = 0;
    }

    if (y) {
        *y = 0;
    }
}

void GfxWindowBackendDeko::GetMouseWheel(float* x, float* y) {
    if (x) {
        *x = 0.0f;
    }

    if (y) {
        *y = 0.0f;
    }
}

bool GfxWindowBackendDeko::GetMouseState(std::uint32_t btn) {
    return false;
}

void GfxWindowBackendDeko::SetMouseCapture(bool capture) {
}

bool GfxWindowBackendDeko::IsMouseCaptured() {
    return false;
}
} // namespace Fast
#endif