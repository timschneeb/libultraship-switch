# ========= ImGui =========
target_include_directories(ImGui PRIVATE ${DEVKITPRO}/portlibs/switch/include/ ${DEVKITPRO}/portlibs/switch/include/SDL2)

# ========= spdlog =========
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(SPDLOG_BUILD_EXAMPLE OFF)
FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.14.1
        OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(spdlog)

# ========= StormLib =========
target_compile_definitions(storm PRIVATE _POSIX_C_SOURCE=200809L)