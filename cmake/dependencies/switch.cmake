# ========= ImGui =========
target_include_directories(ImGui PRIVATE ${DEVKITPRO}/portlibs/switch/include/)

# ========= spdlog =========
find_package(spdlog QUIET)
if (NOT ${spdlog_FOUND})
    FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.14.1
            OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(spdlog)
endif()


# ========= StormLib =========
if(INCLUDE_MPQ_SUPPORT)
    target_compile_definitions(StormLib PRIVATE -D_POSIX_C_SOURCE=200809L)
endif()