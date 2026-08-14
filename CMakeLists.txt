cmake_minimum_required(VERSION 3.20)
project(otts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(otts_rtmp
    cpp/src/main.cpp
    cpp/src/core/logger.cpp
    cpp/src/core/process_supervisor.cpp
    cpp/src/http/flv_mux.cpp
    cpp/src/http/http_server.cpp
    cpp/src/rtmp/amf0.cpp
    cpp/src/rtmp/stream_registry.cpp
    cpp/src/rtmp/rtmp_server.cpp
    cpp/src/rtmp/rtmp_session.cpp
    cpp/src/webrtc/webrtc_service.cpp
)

target_include_directories(otts_rtmp PRIVATE cpp/include)
if (MSVC)
    target_compile_options(otts_rtmp PRIVATE /W4)
else()
    target_compile_options(otts_rtmp PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)
target_link_libraries(otts_rtmp PRIVATE Threads::Threads)
