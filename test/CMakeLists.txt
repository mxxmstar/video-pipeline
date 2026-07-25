add_executable(test_spdlog test_spdlog.cpp)
target_link_libraries(test_spdlog PRIVATE video-pipeline::spdlog)
video_pipeline_copy_runtime_dlls(test_spdlog)
if(MSVC)
    target_compile_options(test_spdlog PRIVATE /W3 /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(test_spdlog PRIVATE -Wall -Wextra)
endif()
add_test(NAME test_spdlog COMMAND test_spdlog)

add_executable(test_publisher_protocol media/test_publisher_protocol.cpp)
target_link_libraries(test_publisher_protocol PRIVATE video_pipeline_lib)
video_pipeline_copy_runtime_dlls(test_publisher_protocol)
if(MSVC)
    target_compile_options(test_publisher_protocol PRIVATE /W3 /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(test_publisher_protocol PRIVATE -Wall -Wextra)
endif()
add_test(NAME test_publisher_protocol COMMAND test_publisher_protocol)

if(VIDEO_PIPELINE_BUILD_INFERENCE)
    add_executable(test_inference_smoke inference/test_inference_smoke.cpp)
    target_link_libraries(test_inference_smoke PRIVATE video_pipeline_lib)
    video_pipeline_copy_runtime_dlls(test_inference_smoke)
    if(MSVC)
        target_compile_options(test_inference_smoke PRIVATE /W3 /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(test_inference_smoke PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME test_inference_smoke COMMAND test_inference_smoke)

    add_executable(test_yolov5_inference inference/test_yolov5_inference.cpp)
    target_link_libraries(test_yolov5_inference PRIVATE video_pipeline_lib)
    target_compile_definitions(test_yolov5_inference
        PRIVATE
            VIDEO_PIPELINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    video_pipeline_copy_runtime_dlls(test_yolov5_inference)
    if(MSVC)
        target_compile_options(test_yolov5_inference PRIVATE /W3 /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(test_yolov5_inference PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME test_yolov5_inference COMMAND test_yolov5_inference)
endif()

add_executable(test_ffmpeg_audio_encoder media/test_ffmpeg_audio_encoder.cpp)
target_link_libraries(test_ffmpeg_audio_encoder PRIVATE video_pipeline_lib)
video_pipeline_copy_runtime_dlls(test_ffmpeg_audio_encoder)
if(MSVC)
    target_compile_options(test_ffmpeg_audio_encoder PRIVATE /W3 /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(test_ffmpeg_audio_encoder PRIVATE -Wall -Wextra)
endif()
add_test(NAME test_ffmpeg_audio_encoder COMMAND test_ffmpeg_audio_encoder)

add_executable(test_rtsp_server_publisher media/test_rtsp_server_publisher.cpp)
target_link_libraries(test_rtsp_server_publisher PRIVATE video_pipeline_lib)
target_compile_definitions(test_rtsp_server_publisher
    PRIVATE
        VIDEO_PIPELINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(TARGET video_pipeline_common_process)
    target_link_libraries(test_rtsp_server_publisher PRIVATE video_pipeline_common_process)
    target_compile_definitions(test_rtsp_server_publisher
        PRIVATE
            VIDEO_PIPELINE_HAS_COMMON_PROCESS=1
    )
endif()
video_pipeline_copy_runtime_dlls(test_rtsp_server_publisher)
if(MSVC)
    target_compile_options(test_rtsp_server_publisher PRIVATE /W3 /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(test_rtsp_server_publisher PRIVATE -Wall -Wextra)
endif()
add_executable(test_local_mp4_decode_rtsp_publisher media/test_local_mp4_decode_rtsp_publisher.cpp)
target_link_libraries(test_local_mp4_decode_rtsp_publisher PRIVATE video_pipeline_lib)
target_compile_definitions(test_local_mp4_decode_rtsp_publisher
    PRIVATE
        VIDEO_PIPELINE_TEST_MP4_PATH="${CMAKE_SOURCE_DIR}/test.mp4"
)
video_pipeline_copy_runtime_dlls(test_local_mp4_decode_rtsp_publisher)
if(MSVC)
    target_compile_options(test_local_mp4_decode_rtsp_publisher PRIVATE /W3 /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(test_local_mp4_decode_rtsp_publisher PRIVATE -Wall -Wextra)
endif()
add_test(
    NAME test_local_mp4_decode_rtsp_publisher
    COMMAND test_local_mp4_decode_rtsp_publisher
        --once
        --self-test
        --port 0
        --max-video-packets 300
        --max-decoded-frames 120
        --no-realtime
)

add_executable(test_stream_decode media/test_stream_decode.cpp)
target_link_libraries(test_stream_decode PRIVATE video_pipeline_lib)
video_pipeline_copy_runtime_dlls(test_stream_decode)
if(MSVC)
    target_compile_options(test_stream_decode PRIVATE /W3 /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(test_stream_decode PRIVATE -Wall -Wextra)
endif()

if(TARGET video_pipeline_common_process)
    add_executable(test_zlmediakit_process media/test_zlmediakit_process.cpp)
    target_link_libraries(test_zlmediakit_process PRIVATE video_pipeline_common_process)
    video_pipeline_copy_runtime_dlls(test_zlmediakit_process)
    if(MSVC)
        target_compile_options(test_zlmediakit_process PRIVATE /W3 /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(test_zlmediakit_process PRIVATE -Wall -Wextra)
    endif()

    add_executable(test_rtsp_decode_encode_push_zlm media/test_rtsp_decode_encode_push_zlm.cpp)
    target_link_libraries(test_rtsp_decode_encode_push_zlm
        PRIVATE
            video_pipeline_lib
            video_pipeline_common_process
    )
    video_pipeline_copy_runtime_dlls(test_rtsp_decode_encode_push_zlm)
    if(MSVC)
        target_compile_options(test_rtsp_decode_encode_push_zlm PRIVATE /W3 /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(test_rtsp_decode_encode_push_zlm PRIVATE -Wall -Wextra)
    endif()
endif()
