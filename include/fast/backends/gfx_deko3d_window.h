#pragma once

#if defined(ENABLE_DEKO3D)
#include "gfx_window_manager_api.h"

#include <SDL2/SDL.h>
#include <deko3d.hpp>

namespace Fast {
/**
 * @brief: Window/timing/presentation backend for deko3d.
 *
 * Ownership: this class owns dk::Device, dk::Queue, and dk::Swapchain bound to nWindowGetDefault().
 * GfxRenderingApiDeko3d borrows the queue and the per-frame command buffer from here to record draws; presentation
 * lives here.
 *
 * SDL is initialized for input/events only (no SDL_INIT_VIDEO), so deko3d owns the applet's default window cleanly and
 * the existing SDL controller-mapping layer keeps working.
 */
class GfxWindowBackendDeko3d final : public GfxWindowBackend {
  public:
    GfxWindowBackendDeko3d() = default;
    ~GfxWindowBackendDeko3d() override;

    void Init(const char* gameName, const char* apiName, bool startFullScreen, std::uint32_t width,
              std::uint32_t height, std::int32_t posX, std::int32_t posY) override;
    void Close() override;

    static constexpr std::uint8_t sFramebuffers = 2;

    // ----------------------------------------------------------------------------------------------------------------
    // Timing
    // ----------------------------------------------------------------------------------------------------------------

    double GetTime() override;
    int GetTargetFps() override;
    void SetTargetFps(int fps) override;
    void SetMaxFrameLatency(int latency) override;
    bool IsFrameReady() override;
    bool CanDisableVsync() override;

    // ----------------------------------------------------------------------------------------------------------------
    // Fullscreen
    // ----------------------------------------------------------------------------------------------------------------

    void SetFullscreen(bool fullscreen) override;
    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) override;
    bool IsFullscreen() override;

    // ----------------------------------------------------------------------------------------------------------------
    // Window
    // ----------------------------------------------------------------------------------------------------------------

    void GetDimensions(std::uint32_t* width, std::uint32_t* height, std::int32_t* posX, std::int32_t* posY) override;
    void SetDimensions(std::uint32_t width, std::uint32_t height, std::int32_t posX, std::int32_t posY) override;
    void GetActiveWindowRefreshRate(std::uint32_t* refreshRate) override;
    Ship::WindowRect GetPrimaryMonitorRect() override;

    void HandleEvents() override;
    void SwapBuffersBegin() override;
    void SwapBuffersEnd() override;
    bool IsRunning() override;
    void Destroy() override;

    // ----------------------------------------------------------------------------------------------------------------
    // deko3d
    // ----------------------------------------------------------------------------------------------------------------

    // The rapi borrows the current frame's cmdbuf, records draws, and returns the finished list; the window backend
    // submits it after the clear in SwapBuffersBegin.
    dk::CmdBuf BeginFrameRecording();
    void EndFrameRecording(DkCmdList drawList);

    dk::Device GetDevice() const;
    dk::Queue GetQueue() const;

    const dk::Image& GetFramebuffer(int slot) const;
    // Slot acquired for the current frame, or -1 between frames. The rendering API binds this framebuffer as its
    // render target.
    int GetCurrentImageSlot() const;

    // Resources the rapi binds while recording (window backend owns GPU memory; rapi borrows).
    const dk::Shader& GetColorVsh() const;
    const dk::Shader& GetColorFsh() const;

    std::uint32_t GetRecordingRing() const;

    // ----------------------------------------------------------------------------------------------------------------
    // Keyboard (no-op)
    // ----------------------------------------------------------------------------------------------------------------

    void SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                              void (*onAllKeysUp)()) override;
    const char* GetKeyName(int scancode) override;

    // ----------------------------------------------------------------------------------------------------------------
    // Mouse (no-op)
    // ----------------------------------------------------------------------------------------------------------------

    void SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) override;
    void SetCursorVisibility(bool visability) override;
    void SetMousePos(std::int32_t posX, std::int32_t posY) override;
    void GetMousePos(std::int32_t* x, std::int32_t* y) override;
    void GetMouseDelta(std::int32_t* x, std::int32_t* y) override;
    void GetMouseWheel(float* x, float* y) override;
    bool GetMouseState(std::uint32_t btn) override;
    void SetMouseCapture(bool capture) override;
    bool IsMouseCaptured() override;

    // ----------------------------------------------------------------------------------------------------------------
    // Debug
    // ----------------------------------------------------------------------------------------------------------------

    void Trace(const char* message);

  private:
    void CreateDeko3dDevice();
    void CreateSwapChain(std::uint32_t width, std::uint32_t height);
    void DestroySwapChain();
    void RecordClearCommandLists();
    // Load a single-stage .dksh from romfs into the shared shader code memblock.  Removed with the rest of the
    // scaffolding once GfxRenderingApiDeko3d records real draws.
    bool LoadDeko3dShader(dk::Shader& shader, const char* path);

    // deko3d core objects (owned).
    dk::UniqueDevice mDevice = {};
    dk::UniqueQueue mQueue = {};
    dk::UniqueSwapchain mSwapChain = {};

    // Framebuffer images live in a single GPU-cached image memblock.
    dk::UniqueMemBlock mFbMemBlock = {};
    std::array<dk::Image, sFramebuffers> mFramebuffers = {};
    std::int32_t mCurrentSlot = -1;

    // Command memory + a cmdbuf holding the pre-recorded per-slot clear lists.
    dk::UniqueMemBlock mCmdMemBlock = {};
    dk::UniqueCmdBuf mCmdBuf = {};
    std::array<DkCmdList, sFramebuffers> mClearCmdLists = {};

    // Shader code memory + the two vertex color shaders, plus a static vertex buffer feeding their attributes.
    dk::UniqueMemBlock mShaderCodeMemBlock = {};
    dk::Shader mColorVsh = {};
    dk::Shader mColorFsh = {};
    std::uint32_t mShaderCodeOffset = 0;

    // Command region + cmdbuf + fence per swap chain image, cycled each frame; the fence gates reuse so we never
    // rerecord memory the GPU is still executing.
    std::array<dk::UniqueMemBlock, sFramebuffers> mFrameCmdMemBlock = {};
    std::array<dk::UniqueCmdBuf, sFramebuffers> mFrameCmdBuf = {};
    std::array<dk::Fence, sFramebuffers> mFrameFence = {};
    std::array<bool, sFramebuffers> mIsFrameFenceValid = {};
    std::uint32_t mFrameIndex = 0;
    DkCmdList mFrameDrawList = {};
    bool mHasFrameDrawList = false;
    std::uint32_t mRecordingRing = 0;

    std::uint32_t mWidth = 1280;
    std::uint32_t mHeight = 720;
    std::int32_t mTargetFps = 60;
    bool mIsRunning = true;
    bool mIsInitialized = false;

    void (*mOnAllKeysUp)() = nullptr;
};
} // namespace Fast
#endif