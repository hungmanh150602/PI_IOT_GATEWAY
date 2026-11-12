cmake_minimum_required(VERSION 3.16)

project(ESP32CAM_Gateway
    VERSION 1.0
    LANGUAGES C
)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# main programs
add_executable(main
    src/main.c
)

# http_capture_tcp
add_executable(http_capture_tcp
    src/http_capture_tcp.c
)

# mjpeg_client_tcp
add_executable(mjpeg_client_tcp
    src/mjpeg_client_tcp.c
)

# MJPEG client for Chrome browser
add_executable(stream_video_tcp_web
    src/stream_video_tcp_web.c
)

# MJPEG client for FFplay display
add_executable(stream_video_tcp_hdmi
    src/stream_video_tcp_hdmi.c
)

# MJPEG udp for Chrome browser
add_executable(stream_video_udp_web
    src/stream_video_udp_web.c
)

# test_api_request
add_executable(test_api_request
    src/test_api_request.c
)

# Compiler warnings
foreach(target
    main
    http_capture_tcp
    mjpeg_client_tcp
    stream_video_tcp_web
    stream_video_tcp_hdmi
    stream_video_udp_web
    test_api_request
)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Werror
    )
endforeach()