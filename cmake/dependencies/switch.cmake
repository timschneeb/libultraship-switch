# ========= ImGui =========
target_include_directories(ImGui PRIVATE ${DEVKITPRO}/portlibs/switch/include/ ${DEVKITPRO}/portlibs/switch/include/SDL2)

# ========= spdlog =========
set(spdlog_switch_patch_file ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/patches/spdlog-switch.patch)
set(spdlog_apply_patch_command ${CMAKE_COMMAND} -Dpatch_file=${spdlog_switch_patch_file} -Dwith_reset=TRUE -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/git-patch.cmake)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(SPDLOG_BUILD_EXAMPLE OFF)
FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.14.1
        PATCH_COMMAND ${spdlog_apply_patch_command}
        OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(spdlog)

# ========= StormLib =========
target_compile_definitions(storm PRIVATE _POSIX_C_SOURCE=200809L)