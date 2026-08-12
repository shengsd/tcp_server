cmake_minimum_required(VERSION 3.14)
project(tcp_server CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 强制定义 Standalone Asio 标志 (脱离 Boost 依赖)
add_compile_definitions(ASIO_STANDALONE)

# 1. 优先尝试在系统中查找 asio.hpp (兼容 CentOS 7 / Ubuntu / macOS)
find_path(ASIO_INCLUDE_DIR asio.hpp
    HINTS
        /usr/include
        /usr/local/include
        /usr/include/asio
        /opt/homebrew/include
)

# 2. 如果 CentOS 7 系统未安装 asio-devel，自动通过 FetchContent 下载 Standalone Asio 头文件
if (NOT ASIO_INCLUDE_DIR)
    message(STATUS "Asio header not found in system. Fetching Standalone Asio from GitHub...")
    include(FetchContent)
    FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-30-2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(asio)
    set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
endif()

message(STATUS "Asio Include Directory: ${ASIO_INCLUDE_DIR}")

# 包含头文件目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ASIO_INCLUDE_DIR}
)

# 系统依赖库 (pthread)
find_package(Threads REQUIRED)

# tcp_server 静态库
add_library(tcp_server_lib STATIC
    src/tcp_session.cpp
    src/io_thread_pool.cpp
    src/tcp_server.cpp
)

target_link_libraries(tcp_server_lib PRIVATE Threads::Threads ${CMAKE_THREAD_LIBS_INIT})

# 示例程序
add_executable(tcp_server_example example/main.cpp)
target_link_libraries(tcp_server_example PRIVATE tcp_server_lib Threads::Threads ${CMAKE_THREAD_LIBS_INIT})

if (BUILD_TESTING)
    add_executable(tcp_session_concurrency_test
        tests/tcp_session_concurrency_test.cpp
    )
    target_link_libraries(tcp_session_concurrency_test
        PRIVATE tcp_server_lib Threads::Threads ${CMAKE_THREAD_LIBS_INIT}
    )
    add_test(NAME tcp_session_concurrency_test COMMAND tcp_session_concurrency_test)
    set_tests_properties(tcp_session_concurrency_test PROPERTIES TIMEOUT 15)
endif()
