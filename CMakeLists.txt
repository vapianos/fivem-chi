cmake_minimum_required(VERSION 3.20)
project(fivem-cheat CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)

# CRT estatica: el payload no debe depender del MSVCP140/VCRUNTIME140 de FiveM.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

if(MSVC)
    add_compile_options(/utf-8 /MP /GS- /Gy /Oi)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    add_link_options(/OPT:REF /OPT:ICF)
endif()

include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKen/MinHook.git
    GIT_TAG master)
FetchContent_MakeAvailable(imgui minhook)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC d3d11 dxgi)

add_library(minhook_lib STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook_lib PUBLIC ${minhook_SOURCE_DIR}/include)

add_executable(launcher WIN32
    injector/main.cpp
    injector/gui.cpp
    injector/inject.cpp
    injector/manualmap.cpp)
target_link_libraries(launcher PRIVATE imgui_lib d3d11 dxgi comdlg32 psapi shell32 ntdll)

add_library(cheat SHARED
    payload/dllmain.cpp
    payload/core/config.cpp
    payload/core/hooks.cpp
    payload/core/lua_runtime.cpp
    payload/core/module.cpp
    payload/core/scanner.cpp
    payload/features/native.cpp
    payload/features/script_loader.cpp
    payload/render/menu.cpp
    payload/render/overlay.cpp
    payload/utils/log.cpp
    payload/utils/paths.cpp)
target_include_directories(cheat PRIVATE payload)
target_link_libraries(cheat PRIVATE imgui_lib minhook_lib d3d11 dxgi psapi)

set_target_properties(cheat PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cheat"
    WIN32_EXECUTABLE TRUE)

# Los scripts Lua tienen que acabar donde paths::LuaDir() los busca.
add_custom_command(TARGET cheat POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:cheat>/payload
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:cheat> $<TARGET_FILE_DIR:cheat>/payload/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/lua $<TARGET_FILE_DIR:cheat>/lua
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/lua "$ENV{USERPROFILE}/Documents/VeniceFiveM/lua")