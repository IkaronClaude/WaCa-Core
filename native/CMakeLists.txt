cmake_minimum_required(VERSION 3.22)
project(WaCaNative C CXX)

# WaCaNative is a UE4SS C++ mod: it links UE4SS (the SDK = UE4SS's own source, built here). WACA_UE4SS_DIR must be
# a checkout of RE-UE4SS at the commit of the UE4SS release we ship (its log line "Git SHA #..."), with submodules -
# the Unreal one (UEPseudo) is private: it needs a GitHub account linked to Epic Games. Configure with
# -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 (UE4SS's configuration names).
set(WACA_UE4SS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/bin/src/RE-UE4SS" CACHE PATH "RE-UE4SS source checkout")
if(NOT EXISTS "${WACA_UE4SS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "WACA_UE4SS_DIR (${WACA_UE4SS_DIR}) is not a RE-UE4SS checkout - see native/build.bat")
endif()
add_subdirectory("${WACA_UE4SS_DIR}" ue4ss)

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

add_library(WaCaNative SHARED
    WaCaNative/src/dllmain.cpp
    WaCaNative/src/bridge.cpp
    WaCaNative/src/commands.cpp
    WaCaNative/src/input.cpp
    WaCaNative/src/text.cpp
    WaCaNative/src/ui.cpp
    WaCaNative/src/dev.cpp
    WaCaNative/src/steam.cpp
    WaCaNative/src/render.cpp
    WaCaNative/src/keys.cpp
    WaCaNative/src/appearance.cpp
    WaCaNative/src/tables.cpp
    WaCaNative/src/camera.cpp
    WaCaNative/src/saves.cpp
    WaCaNative/src/prey.cpp
    WaCaNative/src/hunt.cpp
    WaCaNative/src/sequences.cpp
    WaCaNative/src/screens.cpp
    WaCaNative/src/fill.cpp
    WaCaNative/src/areas.cpp
    WaCaNative/src/combat.cpp
    WaCaNative/src/bonds.cpp)
target_link_libraries(WaCaNative PRIVATE minhook UE4SS dxgi dxguid)
target_compile_definitions(WaCaNative PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(WaCaNative PROPERTIES OUTPUT_NAME main)
