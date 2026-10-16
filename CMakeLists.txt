cmake_minimum_required(VERSION 3.22)

project(Splinter VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
endif()

option(SPLINTER_BUILD_PLUGIN "Build the JUCE plugin (VST3 / AU / Standalone)" ON)
option(SPLINTER_BUILD_TESTS  "Build the DSP unit tests and the offline render tool" ON)

# ---------------------------------------------------------------------------
# DSP core: plain C++17, no framework dependency. Everything that makes sound
# lives here so it can be unit tested and rendered offline without a host.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SPLINTER_DSP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/source/dsp/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/dsp/*.h)

add_library(splinter_dsp STATIC ${SPLINTER_DSP_SOURCES})
target_include_directories(splinter_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source)

if(MSVC)
    set(SPLINTER_WARNING_FLAGS /W4)
else()
    set(SPLINTER_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()
target_compile_options(splinter_dsp PRIVATE ${SPLINTER_WARNING_FLAGS})

# ---------------------------------------------------------------------------
# Tests and offline render tool
# ---------------------------------------------------------------------------
if(SPLINTER_BUILD_TESTS)
    enable_testing()

    # Everything in tools/render except main() is a library, so the tests can cover it too.
    file(GLOB SPLINTER_RENDER_SUPPORT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/render/*.cpp)
    list(FILTER SPLINTER_RENDER_SUPPORT_SOURCES EXCLUDE REGEX "RenderMain\\.cpp$")
    add_library(splinter_render_support STATIC ${SPLINTER_RENDER_SUPPORT_SOURCES})
    target_include_directories(splinter_render_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools)
    target_link_libraries(splinter_render_support PUBLIC splinter_dsp)

    add_executable(splinter_render ${CMAKE_CURRENT_SOURCE_DIR}/tools/render/RenderMain.cpp)
    target_link_libraries(splinter_render PRIVATE splinter_render_support)

    file(GLOB SPLINTER_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    add_executable(splinter_tests ${SPLINTER_TEST_SOURCES})
    target_link_libraries(splinter_tests PRIVATE splinter_render_support)
    target_include_directories(splinter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME splinter_tests COMMAND splinter_tests)
endif()

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
if(SPLINTER_BUILD_PLUGIN)
    # To build offline, point CMake at a local JUCE checkout:
    #   cmake -B build -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.10
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)

    set(SPLINTER_FORMATS VST3 Standalone)
    if(APPLE)
        list(APPEND SPLINTER_FORMATS AU)
    endif()

    juce_add_plugin(Splinter
        COMPANY_NAME                "Splinter Audio"
        PLUGIN_MANUFACTURER_CODE    Splr
        PLUGIN_CODE                 Spl1
        FORMATS                     ${SPLINTER_FORMATS}
        PRODUCT_NAME                "Splinter"
        IS_SYNTH                    FALSE
        NEEDS_MIDI_INPUT            FALSE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD     FALSE
        VST3_CATEGORIES             Fx
        AU_MAIN_TYPE                kAudioUnitType_Effect)

    file(GLOB SPLINTER_PLUGIN_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/source/plugin/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/plugin/*.h)
    target_sources(Splinter PRIVATE ${SPLINTER_PLUGIN_SOURCES})

    target_compile_definitions(Splinter PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(Splinter
        PRIVATE
            splinter_dsp
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()
