#pragma once

#include <cstdint>
#include <string>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>
#include "SwitchPerformanceProfiles.h"

namespace Ship {
enum SwitchProfiles { MAXIMUM, HIGH, BOOST, STOCK, POWERSAVINGM1, POWERSAVINGM2, POWERSAVINGM3 };

enum SwitchPhase { PreInitPhase, PostInitPhase };

class Switch {
  public:
    static void Init(SwitchPhase phase);
    static void Exit();
    static void ImGuiSetupFont(ImFontAtlas* fonts);
    static void CreateKeyboard();
    // Pumps the inline keyboard applet; called once per frame.
    static void UpdateKeyboard();
    // Shows the inline software keyboard, seeded with initialText.
    // The typed string is streamed via callbacks, read with ConsumeKeyboardText.
    static void ShowKeyboard(ImGuiID owner, const std::string& initialText);
    static bool IsKeyboardActive();
    // Fills out the latest keyboard text once per change; returns false when nothing new.
    // Apply the result to the field and rebuild only when this returns true.
    static bool ConsumeKeyboardText(ImGuiID owner, std::string& out);
    static bool IsRunning();
    static void GetDisplaySize(int* width, int* height);
    static void ApplyOverclock();
    static void ShowErrorApplet(const char* format, ...);
    static void ThrowMissingOTR(std::string otrPath);
    static char* GetControllerUUID(int controller);
};
}; // namespace Ship