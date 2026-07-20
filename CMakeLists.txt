cmake_minimum_required(VERSION 3.16)

# Visual Studio loads the vcpkg toolchain during project(), so vcpkg cache
# variables must be set before that call.
set(VIDEO_PIPELINE_VCPKG_INSTALLED_DIR "C:/vcpkg_env/vcpkg_installed" CACHE PATH "Shared vcpkg installed root")
if(NOT DEFINED VCPKG_INSTALLED_DIR)
    set(VCPKG_INSTALLED_DIR "${VIDEO_PIPELINE_VCPKG_INSTALLED_DIR}" CACHE PATH "vcpkg installed root")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg target triplet")
endif()
if(NOT DEFINED VCPKG_MANIFEST_INSTALL)
    set(VCPKG_MANIFEST_INSTALL OFF CACHE BOOL "Do not automatically run vcpkg install during CMake configure")
endif()

project(video-pipeline LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build test programs" ON)
option(ENABLE_PCAP "Build pcap/Npcap ethernet capture support" ON)
set(VIDEO_PIPELINE_PROFILE "tauri" CACHE STRING "Build profile: tauri, native, all, or custom")
set_property(CACHE VIDEO_PIPELINE_PROFILE PROPERTY STRINGS tauri native all custom)
option(VIDEO_PIPELINE_BUILD_CLI "Build standalone C++ CLI/demo executable" ON)
option(VIDEO_PIPELINE_BUILD_TAURI_MODULE "Build embeddable Tauri C ABI module when src/c_api sources exist" OFF)
option(VIDEO_PIPELINE_BUILD_NATIVE_PLAYER "Build Boost based native player/web service app" OFF)
set(VIDEO_PIPELINE_BOOST_MIN_VERSION "1.91" CACHE STRING "Minimum Boost version for project Boost components")
set(VIDEO_PIPELINE_DEFAULT_DEPS_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
set(VIDEO_PIPELINE_DEPS_ROOT "${VIDEO_PIPELINE_DEFAULT_DEPS_ROOT}" CACHE PATH "vcpkg installed prefix for Boost, spdlog, and runtime DLLs")
set(VIDEO_PIPELINE_BOOST_ROOT "" CACHE PATH "Deprecated alias for VIDEO_PIPELINE_DEPS_ROOT")
option(VIDEO_PIPELINE_BUILD_ZLMEDIAKIT_TEST "Build manual ZLMediaKit process smoke test" ON)

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

set(VIDEO_PIPELINE_BUILD_COMMON_PROCESS ${VIDEO_PIPELINE_BUILD_NATIVE_PLAYER})
if(BUILD_TESTS AND VIDEO_PIPELINE_BUILD_ZLMEDIAKIT_TEST)
    set(VIDEO_PIPELINE_BUILD_COMMON_PROCESS ON)
endif()

if(VIDEO_PIPELINE_BOOST_ROOT AND VIDEO_PIPELINE_DEPS_ROOT STREQUAL VIDEO_PIPELINE_DEFAULT_DEPS_ROOT)
    set(VIDEO_PIPELINE_DEPS_ROOT "${VIDEO_PIPELINE_BOOST_ROOT}" CACHE PATH "vcpkg installed prefix for Boost, spdlog, and runtime DLLs" FORCE)
endif()

if(VIDEO_PIPELINE_DEPS_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${VIDEO_PIPELINE_DEPS_ROOT}")
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
# vcpkg dependencies
# ============================================================
set(Boost_USE_MULTITHREADED ON)
find_package(Boost ${VIDEO_PIPELINE_BOOST_MIN_VERSION} CONFIG REQUIRED COMPONENTS json process)
find_package(spdlog CONFIG REQUIRED)

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

foreach(boost_header_target asio beast)
    if(TARGET Boost::${boost_header_target})
        target_link_libraries(video_pipeline_boost INTERFACE Boost::${boost_header_target})
    endif()
endforeach()

target_compile_definitions(video_pipeline_boost
    INTERFACE
        BOOST_ASIO_NO_DEPRECATED
        BOOST_BEAST_NO_DEPRECATED
)

add_library(video_pipeline_spdlog INTERFACE)
add_library(video-pipeline::spdlog ALIAS video_pipeline_spdlog)

if(TARGET spdlog::spdlog)
    target_link_libraries(video_pipeline_spdlog INTERFACE spdlog::spdlog)
elseif(TARGET spdlog::spdlog_header_only)
    target_link_libraries(video_pipeline_spdlog INTERFACE spdlog::spdlog_header_only)
else()
    message(FATAL_ERROR "spdlog target was not found")
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

set(VIDEO_PIPELINE_RESOLVED_DEPS_ROOT "${VIDEO_PIPELINE_DEPS_ROOT}")
if(NOT VIDEO_PIPELINE_RESOLVED_DEPS_ROOT AND Boost_DIR)
    get_filename_component(VIDEO_PIPELINE_RESOLVED_DEPS_ROOT "${Boost_DIR}/../.." ABSOLUTE)
endif()
set(VIDEO_PIPELINE_VCPKG_RELEASE_RUNTIME_DIR "")
set(VIDEO_PIPELINE_VCPKG_DEBUG_RUNTIME_DIR "")
if(WIN32 AND VIDEO_PIPELINE_RESOLVED_DEPS_ROOT)
    set(VIDEO_PIPELINE_VCPKG_RELEASE_RUNTIME_DIR "${VIDEO_PIPELINE_RESOLVED_DEPS_ROOT}/bin")
    if(EXISTS "${VIDEO_PIPELINE_RESOLVED_DEPS_ROOT}/debug/bin")
        set(VIDEO_PIPELINE_VCPKG_DEBUG_RUNTIME_DIR "${VIDEO_PIPELINE_RESOLVED_DEPS_ROOT}/debug/bin")
    else()
        set(VIDEO_PIPELINE_VCPKG_DEBUG_RUNTIME_DIR "${VIDEO_PIPELINE_VCPKG_RELEASE_RUNTIME_DIR}")
    endif()
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

        if(VIDEO_PIPELINE_VCPKG_RELEASE_RUNTIME_DIR)
            if(EXISTS "${VIDEO_PIPELINE_VCPKG_RELEASE_RUNTIME_DIR}")
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "$<IF:$<CONFIG:Debug>,${VIDEO_PIPELINE_VCPKG_DEBUG_RUNTIME_DIR},${VIDEO_PIPELINE_VCPKG_RELEASE_RUNTIME_DIR}>"
                        "$<TARGET_FILE_DIR:${target_name}>"
                    COMMENT "Copying vcpkg runtime DLLs"
                )
            endif()
        endif()
    endif()
endfunction()

function(video_pipeline_copy_boost_runtime_dlls target_name)
    video_pipeline_copy_runtime_dlls(${target_name})
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
list(FILTER VIDEO_PIPELINE_SOURCES EXCLUDE REGEX ".*/common/process/.*\\.cpp$")
list(FILTER VIDEO_PIPELINE_SOURCES EXCLUDE REGEX ".*\\\\common\\\\process\\\\.*\\.cpp$")

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
        video-pipeline::boost
        video-pipeline::spdlog
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
# Common Boost.Process helper
# ============================================================
if(VIDEO_PIPELINE_BUILD_COMMON_PROCESS)
    add_library(video_pipeline_common_process STATIC
        "${CMAKE_SOURCE_DIR}/src/common/process/process.cpp"
    )
    add_library(video-pipeline::common_process ALIAS video_pipeline_common_process)

    target_include_directories(video_pipeline_common_process
        PUBLIC
            "${CMAKE_SOURCE_DIR}/include"
    )

    target_link_libraries(video_pipeline_common_process
        PUBLIC
            video-pipeline::boost
            Threads::Threads
    )

    if(WIN32)
        target_compile_definitions(video_pipeline_common_process PUBLIC _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(video_pipeline_common_process PUBLIC ws2_32 mswsock)
    endif()

    if(MSVC)
        target_compile_options(video_pipeline_common_process PRIVATE /W3 /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(video_pipeline_common_process PRIVATE -Wall -Wextra)
    endif()
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
message(STATUS "Build common process: ${VIDEO_PIPELINE_BUILD_COMMON_PROCESS}")
message(STATUS "Build ZLMediaKit test: ${VIDEO_PIPELINE_BUILD_ZLMEDIAKIT_TEST}")
message(STATUS "Deps root: ${VIDEO_PIPELINE_DEPS_ROOT}")
message(STATUS "Resolved deps root: ${VIDEO_PIPELINE_RESOLVED_DEPS_ROOT}")
message(STATUS "Enable pcap: ${ENABLE_PCAP}")
message(STATUS "Runtime output: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "===========================================")
