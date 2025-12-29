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

#=================== nlohmann-json ===================
find_package(nlohmann_json QUIET)
if (NOT ${nlohmann_json_FOUND})
    FetchContent_Declare(
            nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG v3.11.3
            OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

#=================== libzip ===================
find_package(libzip QUIET)
if (NOT ${libzip_FOUND})
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TOOLS OFF)
    set(BUILD_REGRESS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOC OFF)
    set(BUILD_OSSFUZZ OFF)
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_Declare(
            libzip
            GIT_REPOSITORY https://github.com/nih-at/libzip.git
            GIT_TAG v1.10.1
            OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(libzip)
    list(APPEND ADDITIONAL_LIB_INCLUDES ${libzip_SOURCE_DIR}/lib ${libzip_BINARY_DIR})
endif()

# ========= StormLib =========
target_compile_definitions(storm PRIVATE _POSIX_C_SOURCE=200809L)