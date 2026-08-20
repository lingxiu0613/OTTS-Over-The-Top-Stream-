cmake_minimum_required(VERSION 3.20)
project(otts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OTTS_ENABLE_WEBRTC_NATIVE "Enable optional libwebrtc native dependency integration" OFF)
set(OTTS_LIBWEBRTC_ROOT "" CACHE PATH "Path to extracted crow-misia/libwebrtc-bin package")
option(OTTS_ENABLE_WEBRTC_DATACHANNEL "Enable native WHIP/WHEP through libdatachannel" OFF)
set(OTTS_LIBDATACHANNEL_ROOT "" CACHE PATH "Path to libdatachannel install prefix")

add_executable(otts_rtmp
    cpp/src/main.cpp
    cpp/src/auth/stream_auth.cpp
    cpp/src/codec/video_codec.cpp
    cpp/src/core/logger.cpp
    cpp/src/core/process_supervisor.cpp
    cpp/src/http/flv_mux.cpp
    cpp/src/http/http_server.cpp
    cpp/src/rtmp/amf0.cpp
    cpp/src/rtmp/stream_registry.cpp
    cpp/src/rtmp/rtmp_server.cpp
    cpp/src/rtmp/rtmp_session.cpp
    cpp/src/rtsp/rtsp_play_server.cpp
    cpp/src/rtsp/rtsp_publish_server.cpp
    cpp/src/srt/srt_native_server.cpp
    cpp/src/webrtc/webrtc_service.cpp
)

target_include_directories(otts_rtmp PRIVATE cpp/include)
if (MSVC)
    target_compile_options(otts_rtmp PRIVATE /W4)
else()
    target_compile_options(otts_rtmp PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SRT REQUIRED srt)
target_include_directories(otts_rtmp PRIVATE ${SRT_INCLUDE_DIRS})
target_link_libraries(otts_rtmp PRIVATE Threads::Threads OpenSSL::Crypto ${SRT_LIBRARIES})

if (OTTS_ENABLE_WEBRTC_NATIVE)
    if (NOT OTTS_LIBWEBRTC_ROOT)
        message(FATAL_ERROR "OTTS_ENABLE_WEBRTC_NATIVE=ON requires -DOTTS_LIBWEBRTC_ROOT=/path/to/libwebrtc")
    endif()

    find_path(OTTS_LIBWEBRTC_INCLUDE_DIR
        NAMES api/peer_connection_interface.h
        PATHS "${OTTS_LIBWEBRTC_ROOT}" "${OTTS_LIBWEBRTC_ROOT}/include"
        NO_DEFAULT_PATH)
    find_library(OTTS_LIBWEBRTC_LIBRARY
        NAMES webrtc libwebrtc
        PATHS "${OTTS_LIBWEBRTC_ROOT}" "${OTTS_LIBWEBRTC_ROOT}/lib" "${OTTS_LIBWEBRTC_ROOT}/lib/x64" "${OTTS_LIBWEBRTC_ROOT}/lib/linux-x64"
        NO_DEFAULT_PATH)

    if (NOT OTTS_LIBWEBRTC_INCLUDE_DIR OR NOT OTTS_LIBWEBRTC_LIBRARY)
        message(FATAL_ERROR "Could not find libwebrtc headers/library under OTTS_LIBWEBRTC_ROOT=${OTTS_LIBWEBRTC_ROOT}")
    endif()

    target_compile_definitions(otts_rtmp PRIVATE
        OTTS_WEBRTC_NATIVE_DEPENDENCY=1
        WEBRTC_POSIX
        WEBRTC_LINUX)
    target_include_directories(otts_rtmp SYSTEM PRIVATE
        "${OTTS_LIBWEBRTC_INCLUDE_DIR}"
        "${OTTS_LIBWEBRTC_INCLUDE_DIR}/third_party/abseil-cpp")
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(otts_rtmp PRIVATE -fpermissive)
    endif()
    target_link_libraries(otts_rtmp PRIVATE "${OTTS_LIBWEBRTC_LIBRARY}")
    message(STATUS "OTTS WebRTC native dependency enabled: ${OTTS_LIBWEBRTC_LIBRARY}")
else()
    target_compile_definitions(otts_rtmp PRIVATE OTTS_WEBRTC_NATIVE_DEPENDENCY=0)
endif()

if (OTTS_ENABLE_WEBRTC_DATACHANNEL)
    if (NOT OTTS_LIBDATACHANNEL_ROOT)
        message(FATAL_ERROR "OTTS_ENABLE_WEBRTC_DATACHANNEL=ON requires -DOTTS_LIBDATACHANNEL_ROOT=/path/to/libdatachannel-install")
    endif()
    find_path(OTTS_LIBDATACHANNEL_INCLUDE_DIR
        NAMES rtc/rtc.hpp
        PATHS "${OTTS_LIBDATACHANNEL_ROOT}/include"
        NO_DEFAULT_PATH)
    find_library(OTTS_LIBDATACHANNEL_LIBRARY
        NAMES datachannel libdatachannel
        PATHS "${OTTS_LIBDATACHANNEL_ROOT}/lib"
        NO_DEFAULT_PATH)
    if (NOT OTTS_LIBDATACHANNEL_INCLUDE_DIR OR NOT OTTS_LIBDATACHANNEL_LIBRARY)
        message(FATAL_ERROR "Could not find libdatachannel headers/library under OTTS_LIBDATACHANNEL_ROOT=${OTTS_LIBDATACHANNEL_ROOT}")
    endif()
    pkg_check_modules(LIBNICE REQUIRED nice)
    pkg_check_modules(LIBSRTP REQUIRED libsrtp2)
    pkg_check_modules(AVCODEC REQUIRED libavcodec)
    pkg_check_modules(AVUTIL REQUIRED libavutil)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    find_library(USRSCTP_LIBRARY NAMES usrsctp REQUIRED)
    target_sources(otts_rtmp PRIVATE
        cpp/src/webrtc/audio_transcoder.cpp
        cpp/src/webrtc/video_transcoder.cpp)
    target_compile_definitions(otts_rtmp PRIVATE OTTS_WEBRTC_DATACHANNEL=1)
    target_include_directories(otts_rtmp SYSTEM PRIVATE
        "${OTTS_LIBDATACHANNEL_INCLUDE_DIR}"
        ${LIBNICE_INCLUDE_DIRS}
        ${LIBSRTP_INCLUDE_DIRS}
        ${AVCODEC_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
        ${SWRESAMPLE_INCLUDE_DIRS})
    target_link_libraries(otts_rtmp PRIVATE
        "${OTTS_LIBDATACHANNEL_LIBRARY}"
        ${LIBNICE_LIBRARIES}
        ${LIBSRTP_LIBRARIES}
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SWRESAMPLE_LIBRARIES}
        "${USRSCTP_LIBRARY}"
        OpenSSL::SSL)
    message(STATUS "OTTS WebRTC datachannel backend enabled: ${OTTS_LIBDATACHANNEL_LIBRARY}")
else()
    target_compile_definitions(otts_rtmp PRIVATE OTTS_WEBRTC_DATACHANNEL=0)
endif()
