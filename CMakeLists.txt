cmake_minimum_required(VERSION 3.14)
project(tcp_server CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 强制定义 Standalone Asio 标志 (脱离 Boost 依赖)
add_compile_definitions(ASIO_STANDALONE)

# ==================== 自动探测 vcpkg 路径 (Windows) ====================
if (WIN32 AND NOT CMAKE_TOOLCHAIN_FILE)
    set(POTENTIAL_VCPKG_ROOTS
        "$ENV{VCPKG_ROOT}"
        "$ENV{USERPROFILE}/vcpkg"
        "C:/vcpkg"
        "D:/vcpkg"
        "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg"
    )
    foreach(VCPKG_PATH ${POTENTIAL_VCPKG_ROOTS})
        if (VCPKG_PATH AND EXISTS "${VCPKG_PATH}/scripts/buildsystems/vcpkg.cmake")
            message(STATUS "Found vcpkg at: ${VCPKG_PATH}")
            list(APPEND CMAKE_PREFIX_PATH 
                "${VCPKG_PATH}/installed/x64-windows"
                "${VCPKG_PATH}/installed/x86-windows"
            )
            break()
        endif()
    endforeach()
endif()

# ==================== 跨平台编译配置 ====================
if (WIN32)
    add_compile_definitions(
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
    set(PLATFORM_NETWORK_LIBS ws2_32 mswsock)
    if (MSVC)
        add_compile_options(/utf-8)
    endif()
else()
    set(PLATFORM_NETWORK_LIBS "")
endif()

# 1. 优先尝试在系统中查找 asio.hpp (兼容 Windows / CentOS 7 / Ubuntu / macOS)
find_path(ASIO_INCLUDE_DIR asio.hpp
    HINTS
        "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
        "$ENV{USERPROFILE}/vcpkg/installed/x64-windows/include"
        "C:/vcpkg/installed/x64-windows/include"
        /usr/include
        /usr/local/include
        /usr/include/asio
        /opt/homebrew/include
)

# 2. 如果未安装 asio，自动通过 FetchContent 下载 Standalone Asio 头文件
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

# 查找 log4cxx 日志库 (支持 CONFIG 模式与 Module/Path 模式)
find_package(log4cxx CONFIG QUIET)
if (NOT TARGET log4cxx::log4cxx AND NOT TARGET log4cxx)
    find_package(log4cxx QUIET)
endif()

if (TARGET log4cxx::log4cxx)
    set(LOG4CXX_LIBRARIES log4cxx::log4cxx)
elseif (TARGET log4cxx)
    set(LOG4CXX_LIBRARIES log4cxx)
elseif (log4cxx_FOUND AND LOG4CXX_LIBRARIES)
    # log4cxx_FOUND set by find_package
else()
    find_path(LOG4CXX_INCLUDE_DIR log4cxx/logger.h
        HINTS
            "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
            "$ENV{USERPROFILE}/vcpkg/installed/x64-windows/include"
            "C:/vcpkg/installed/x64-windows/include"
            /usr/include
            /usr/local/include
            /opt/homebrew/include
    )
    find_library(LOG4CXX_LIBRARY NAMES log4cxx log4cxxs liblog4cxx liblog4cxxs
        HINTS
            "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
            "$ENV{USERPROFILE}/vcpkg/installed/x64-windows/lib"
            "C:/vcpkg/installed/x64-windows/lib"
            /usr/lib
            /usr/local/lib
            /usr/lib/x86_64-linux-gnu
            /usr/lib64
            /opt/homebrew/lib
    )
    if (LOG4CXX_INCLUDE_DIR AND LOG4CXX_LIBRARY)
        set(LOG4CXX_LIBRARIES ${LOG4CXX_LIBRARY})
        include_directories(${LOG4CXX_INCLUDE_DIR})
        message(STATUS "Found log4cxx: ${LOG4CXX_LIBRARY}")
    else()
        if (WIN32)
            message(FATAL_ERROR "log4cxx library or header not found on Windows.\nPlease run 'setup_vcpkg.bat' to automatically install vcpkg and log4cxx, or run cmake with '-DCMAKE_TOOLCHAIN_FILE=<vcpkg_path>/scripts/buildsystems/vcpkg.cmake'.")
        else()
            message(FATAL_ERROR "log4cxx library or header not found. Please install liblog4cxx (e.g. 'sudo apt install liblog4cxx-dev' on Ubuntu/Debian, 'sudo yum install log4cxx-devel' on CentOS/RHEL, or 'brew install log4cxx' on macOS).")
        endif()
    endif()
endif()

# tcp_server 静态库
add_library(tcp_server_lib STATIC
    src/logger.cpp
    src/tcp_session.cpp
    src/io_thread_pool.cpp
    src/tcp_server.cpp
)

target_link_libraries(tcp_server_lib PUBLIC ${LOG4CXX_LIBRARIES} ${PLATFORM_NETWORK_LIBS} Threads::Threads ${CMAKE_THREAD_LIBS_INIT})

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
