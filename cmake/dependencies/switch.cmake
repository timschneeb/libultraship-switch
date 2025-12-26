# ========= ImGui =========
target_include_directories(ImGui PRIVATE ${DEVKITPRO}/portlibs/switch/include/)

# ========= StormLib =========
if(INCLUDE_MPQ_SUPPORT)
    target_compile_definitions(StormLib PRIVATE -D_POSIX_C_SOURCE=200809L)
endif()