cmake_minimum_required(VERSION 3.16)

project(video-pipeline LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build test programs" OFF)
option(ENABLE_PCAP "Build pcap/Npcap ethernet capture support" ON)
set(VIDEO_PIPELINE_PROFILE "tauri" CACHE STRING "Build profile: tauri, native, all, or custom")
set_property(CACHE VIDEO_PIPELINE_PROFILE PROPERTY STRINGS tauri native all custom)
option(VIDEO_PIPELINE_BUILD_CLI "Build standalone C++ CLI/demo executable" ON)
option(VIDEO_PIPELINE_BUILD_TAURI_MODULE "Build embeddable Tauri C ABI module when src/c_api sources exist" OFF)
option(VIDEO_PIPELINE_BUILD_NATIVE_PLAYER "Build Boost based native player/web service app" OFF)
set(VIDEO_PIPELINE_BOOST_MIN_VERSION "1.88" CACHE STRING "Minimum Boost version for native player/web service app")

set(VIDEO_PIPELINE_VALID_PROFILES tauri native all custom)
if(NOT VIDEO_PIPELINE_PROFILE IN_LIST VIDEO_PIPELINE_VALID_PROFILES)
    message(FATAL_ERROR "Unknown VIDEO_PIPELINE_PROFILE='${VIDEO_PIPELINE_PROFILE}'. Expected one of: ${VIDEO_PIPELINE_VALID_PROFILES}")
endif()

if(VIDEO_PIPELINE_PROFILE STREQUAL "native")
    set(VIDEO_PIPELINE_BUILD_NATIVE_PLAYER ON)
elseif(VIDEO_PIPELINE_PROFILE STREQUAL "all")
    set(VIDEO_PIPELINE_BUILD_NATIVE_PLAYER ON)
    set(VIDEO_PIPELINE_BUILD_TAURI_MODULE ON)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/lib")
endforeach()

# ============================================================
# Header-only dependencies
# ============================================================
set(ASIO_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/asio-1.36.0")
if(NOT EXISTS "${ASIO_INCLUDE_DIR}/asio.hpp")
    message(FATAL_ERROR "ASIO headers not found: ${ASIO_INCLUDE_DIR}")
endif()

add_library(video_pipeline_asio INTERFACE)
target_include_directories(video_pipeline_asio INTERFACE "${ASIO_INCLUDE_DIR}")
target_compile_definitions(video_pipeline_asio INTERFACE ASIO_STANDALONE)

set(SPDLOG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/spdlog-1.17.0/include")
if(NOT EXISTS "${SPDLOG_INCLUDE_DIR}/spdlog/spdlog.h")
    message(FATAL_ERROR "spdlog headers not found: ${SPDLOG_INCLUDE_DIR}")
endif()

add_library(video_pipeline_spdlog INTERFACE)
target_include_directories(video_pipeline_spdlog INTERFACE "${SPDLOG_INCLUDE_DIR}")

# ============================================================
# Boost dependencies for the native player/service profile
# ============================================================
if(VIDEO_PIPELINE_BUILD_NATIVE_PLAYER)
    set(Boost_USE_MULTITHREADED ON)
    find_package(Boost ${VIDEO_PIPELINE_BOOST_MIN_VERSION} CONFIG QUIET COMPONENTS json process)
    if(NOT Boost_FOUND)
        find_package(Boost ${VIDEO_PIPELINE_BOOST_MIN_VERSION} REQUIRED COMPONENTS json process)
    endif()

    add_library(video_pipeline_boost INTERFACE)
    add_library(video-pipeline::boost ALIAS video_pipeline_boost)

    if(TARGET Boost::headers)
        target_link_libraries(video_pipeline_boost INTERFACE Boost::headers)
    elseif(TARGET Boost::boost)
        target_link_libraries(video_pipeline_boost INTERFACE Boost::boost)
    endif()

    foreach(boost_component json process)
        if(TARGET Boost::${boost_component})
            target_link_libraries(video_pipeline_boost INTERFACE Boost::${boost_component})
        else()
            message(FATAL_ERROR "Boost target Boost::${boost_component} was not found")
        endif()
    endforeach()

    foreach(boost_header_target beast asio)
        if(TARGET Boost::${boost_header_target})
            target_link_libraries(video_pipeline_boost INTERFACE Boost::${boost_header_target})
        endif()
    endforeach()

    target_compile_definitions(video_pipeline_boost
        INTERFACE
            BOOST_ASIO_NO_DEPRECATED
            BOOST_BEAST_NO_DEPRECATED
    )
endif()

# ============================================================
# FFmpeg
# ============================================================
if(WIN32)
    set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-8.1")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")

    set(FFMPEG_LIBRARIES
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/avfilter.lib"
        "${FFMPEG_LIB_DIR}/avdevice.lib"
    )

    foreach(ffmpeg_lib IN LISTS FFMPEG_LIBRARIES)
        if(NOT EXISTS "${ffmpeg_lib}")
            message(FATAL_ERROR "FFmpeg library not found: ${ffmpeg_lib}")
        endif()
    endforeach()

    file(GLOB FFMPEG_RUNTIME_DLLS CONFIGURE_DEPENDS "${FFMPEG_ROOT}/bin/*.dll")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec
        libavformat
        libavutil
        libswscale
        libswresample
        libavfilter
        libavdevice
    )
    set(FFMPEG_RUNTIME_DLLS "")
endif()

function(video_pipeline_copy_runtime_dlls target_name)
    if(WIN32)
        foreach(dll IN LISTS FFMPEG_RUNTIME_DLLS)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${dll}"
                    "$<TARGET_FILE_DIR:${target_name}>"
                COMMENT "Copying FFmpeg runtime: ${dll}"
            )
        endforeach()
    endif()
endfunction()

# ============================================================
# pcap/Npcap headers
# ============================================================
if(ENABLE_PCAP)
    if(WIN32)
        set(PCAP_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/pcap/Include")
        if(NOT EXISTS "${PCAP_INCLUDE_DIR}/pcap.h")
            message(FATAL_ERROR "pcap headers not found: ${PCAP_INCLUDE_DIR}")
        endif()
    else()
        find_path(PCAP_INCLUDE_DIR NAMES pcap.h)
        if(NOT PCAP_INCLUDE_DIR)
            message(FATAL_ERROR "pcap headers not found; configure with -DENABLE_PCAP=OFF to skip EthernetCapture")
        endif()
    endif()
endif()

# ============================================================
# Core library
# ============================================================
find_package(Threads REQUIRED)

file(GLOB_RECURSE VIDEO_PIPELINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)
list(FILTER VIDEO_PIPELINE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
list(FILTER VIDEO_PIPELINE_SOURCES EXCLUDE REGEX ".*\\\\main\\.cpp$")

if(NOT ENABLE_PCAP)
    list(FILTER VIDEO_PIPELINE_SOURCES EXCLUDE REGEX ".*/ethernet_capture\\.cpp$")
    list(FILTER VIDEO_PIPELINE_SOURCES EXCLUDE REGEX ".*\\\\ethernet_capture\\.cpp$")
endif()

add_library(video_pipeline_lib STATIC ${VIDEO_PIPELINE_SOURCES})
add_library(video-pipeline::core ALIAS video_pipeline_lib)

target_include_directories(video_pipeline_lib
    PUBLIC
        "${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(video_pipeline_lib
    PUBLIC
        video_pipeline_asio
        video_pipeline_spdlog
)

if(WIN32)
    target_include_directories(video_pipeline_lib PUBLIC "${FFMPEG_INCLUDE_DIR}")
    if(ENABLE_PCAP)
        target_include_directories(video_pipeline_lib PUBLIC "${PCAP_INCLUDE_DIR}")
        target_compile_definitions(video_pipeline_lib PUBLIC ENABLE_PCAP=1)
    endif()
    target_compile_definitions(video_pipeline_lib PUBLIC _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(video_pipeline_lib PUBLIC ws2_32 mswsock ${FFMPEG_LIBRARIES})
else()
    target_link_libraries(video_pipeline_lib PUBLIC PkgConfig::FFMPEG dl)
    if(ENABLE_PCAP)
        target_include_directories(video_pipeline_lib PUBLIC "${PCAP_INCLUDE_DIR}")
        target_compile_definitions(video_pipeline_lib PUBLIC ENABLE_PCAP=1)
    endif()
endif()

target_link_libraries(video_pipeline_lib PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(video_pipeline_lib PRIVATE /W3 /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(video_pipeline_lib PRIVATE -Wall -Wextra)
endif()

# ============================================================
# CLI app
# ============================================================
if(VIDEO_PIPELINE_BUILD_CLI)
    add_executable(video-pipeline "${CMAKE_SOURCE_DIR}/src/main.cpp")
    target_link_libraries(video-pipeline PRIVATE video_pipeline_lib)
    video_pipeline_copy_runtime_dlls(video-pipeline)

    if(MSVC)
        target_compile_options(video-pipeline PRIVATE /W3 /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(video-pipeline PRIVATE -Wall -Wextra)
    endif()
endif()

# ============================================================
# Tauri embeddable module
# ============================================================
if(VIDEO_PIPELINE_BUILD_TAURI_MODULE)
    file(GLOB_RECURSE VIDEO_PIPELINE_C_API_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/c_api/*.cpp"
    )

    if(VIDEO_PIPELINE_C_API_SOURCES)
        add_library(video_pipeline_tauri SHARED ${VIDEO_PIPELINE_C_API_SOURCES})
        add_library(video-pipeline::tauri ALIAS video_pipeline_tauri)
        target_link_libraries(video_pipeline_tauri PRIVATE video_pipeline_lib)
        video_pipeline_copy_runtime_dlls(video_pipeline_tauri)

        if(MSVC)
            target_compile_options(video_pipeline_tauri PRIVATE /W3 /utf-8 /Zc:__cplusplus)
        else()
            target_compile_options(video_pipeline_tauri PRIVATE -Wall -Wextra)
        endif()
    else()
        message(STATUS "Tauri module requested, but src/c_api has no sources yet; skipping video_pipeline_tauri")
    endif()
endif()

# ============================================================
# Native player / web service app
# ============================================================
if(VIDEO_PIPELINE_BUILD_NATIVE_PLAYER)
    if(EXISTS "${CMAKE_SOURCE_DIR}/apps/player/CMakeLists.txt")
        add_subdirectory(apps/player)
    else()
        message(STATUS "Native player requested, but apps/player is not created yet; Boost support target is available as video-pipeline::boost")
    endif()
endif()

# ============================================================
# Tests
# ============================================================
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

message(STATUS "===========================================")
message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Profile: ${VIDEO_PIPELINE_PROFILE}")
message(STATUS "Build tests: ${BUILD_TESTS}")
message(STATUS "Build CLI: ${VIDEO_PIPELINE_BUILD_CLI}")
message(STATUS "Build Tauri module: ${VIDEO_PIPELINE_BUILD_TAURI_MODULE}")
message(STATUS "Build native player: ${VIDEO_PIPELINE_BUILD_NATIVE_PLAYER}")
message(STATUS "Enable pcap: ${ENABLE_PCAP}")
message(STATUS "Runtime output: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "===========================================")
