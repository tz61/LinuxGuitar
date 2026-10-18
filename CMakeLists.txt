cmake_minimum_required(VERSION 3.16)
project(GuitarStudio C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Native build optimized for the current CPU. -fopenmp-simd only enables "#pragma omp simd"; no OpenMP runtime needed.
set(GS_OPT_FLAGS -march=native -fno-math-errno -fno-trapping-math -ffp-contract=fast -fopenmp-simd)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g1")
set(CMAKE_C_FLAGS_RELEASE "-O3")

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple libpulse)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

function(gs_target_defaults tgt)
    target_compile_options(${tgt} PRIVATE ${GS_OPT_FLAGS} -Wall -Wextra -Wno-unused-parameter)
    target_include_directories(${tgt} PUBLIC ${CMAKE_SOURCE_DIR}/src)
endfunction()

# Static library per module; configures fine even while a module has no .cpp files yet
function(gs_library name)
    file(GLOB _src CONFIGURE_DEPENDS ${ARGN})
    set(_stub ${CMAKE_BINARY_DIR}/stub_${name}.cpp)
    if(NOT EXISTS ${_stub})
        file(WRITE ${_stub} "// generated empty file\n")
    endif()
    add_library(${name} STATIC ${_src} ${_stub})
    gs_target_defaults(${name})
endfunction()

# ---------------------------------------------------------------- module libraries
# DSP + effects
gs_library(gs_common src/common/*.cpp)
target_link_libraries(gs_common PUBLIC Threads::Threads)

gs_library(gs_fx src/dsp/*.cpp src/fx/*.cpp)
target_link_libraries(gs_fx PUBLIC gs_common)
target_link_libraries(gs_fx PUBLIC Threads::Threads)

# Song model + import (gtab / ASCII tab / MusicXML / MIDI / Guitar Pro)
gs_library(gs_song src/song/*.cpp)
target_link_libraries(gs_song PUBLIC gs_common ZLIB::ZLIB)

# Pitch / onset analysis
gs_library(gs_analysis src/analysis/*.cpp)
target_link_libraries(gs_analysis PUBLIC gs_common Threads::Threads)

# Practice: transport / scoring / reference synth / metronome
gs_library(gs_practice src/practice/*.cpp)
target_link_libraries(gs_practice PUBLIC gs_song gs_analysis)

# Audio engine: direct ALSA / device reservation / rtkit / card mixer / PulseAudio bridge
gs_library(gs_audio src/audio/*.cpp)
target_link_libraries(gs_audio PUBLIC gs_fx gs_practice PkgConfig::ALSA PkgConfig::PULSE PkgConfig::DBUS Threads::Threads ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------- Dear ImGui
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
if(NOT EXISTS ${IMGUI_DIR}/imgui.h)
    message(FATAL_ERROR "Dear ImGui is missing; run scripts/fetch_deps.sh first")
endif()
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC PkgConfig::SDL2 OpenGL::GL ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------- application
if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.cpp)
    file(GLOB GS_UI_SRC CONFIGURE_DEPENDS src/ui/*.cpp)
    add_executable(guitarstudio src/main.cpp ${GS_UI_SRC})
    gs_target_defaults(guitarstudio)
    target_link_libraries(guitarstudio PRIVATE gs_audio imgui ZLIB::ZLIB)
    target_compile_definitions(guitarstudio PRIVATE GS_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
endif()

# ---------------------------------------------------------------- tests (offline, no sound card needed)
macro(gs_test name)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/${name}.cpp)
        add_executable(${name} tests/${name}.cpp)
        gs_target_defaults(${name})
        target_link_libraries(${name} PRIVATE ${ARGN})
        target_compile_definitions(${name} PRIVATE GS_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
    endif()
endmacro()
gs_test(test_fx gs_fx)
gs_test(test_song gs_song)
gs_test(test_analysis gs_analysis)
gs_test(test_practice gs_practice)
gs_test(test_engine gs_audio)
