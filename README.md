# C++11 High-Performance TCP Server (Standalone Asio & log4cxx)

基于 C++11 和 **Standalone Asio** 实现的高性能跨平台 TCP 服务端框架，支持 **Linux / Windows / macOS**。采用 **One io_context per Thread (IO 线程池)** 架构，内置心跳超时检测、发送缓冲队列、C++11 事件回调与 **Apache log4cxx** 结构化日志系统。

> 📖 **详细架构设计与底层机制请参阅**：[DESIGN.md](file:///c:/Users/bootcamp/Documents/Github/tcp_server/DESIGN.md)

---

## 跨平台依赖与构建指南

### 1. Windows 构建 (Visual Studio / MSVC / vcpkg)

Windows 环境推荐使用 `vcpkg` 管理第三方依赖。项目内置了**一键自动化安装与构建脚本**：

#### 方式 A：一键式构建 (推荐)
1. **安装依赖**：双击运行 `setup_vcpkg.bat`（或在 PowerShell 中执行 `.\setup_vcpkg.ps1`），脚本会自动克隆 vcpkg 并安装 `log4cxx:x64-windows`。
2. **编译工程**：双击运行 `build_windows.bat`（或在 PowerShell 中执行 `.\build_windows.ps1`），脚本会自动关联 vcpkg 工具链并完成多核编译。
3. **运行程序**：
   ```cmd
   .\build\Release\tcp_server_example.exe
   ```

#### 方式 B：手动 CMake 命令构建
```cmd
# 1. 安装 log4cxx
vcpkg install log4cxx:x64-windows

# 2. 配置与编译
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release -j
```

---

### 2. Linux 构建 (CentOS / RHEL / Ubuntu / Debian)

#### (1) 安装依赖
- **CentOS / RHEL 7+**：
  ```bash
  sudo yum install -y epel-release
  sudo yum install -y gcc-c++ make cmake3 git log4cxx-devel
  ```
- **Ubuntu / Debian**：
  ```bash
  sudo apt-get update
  sudo apt-get install -y build-essential cmake git liblog4cxx-dev
  ```

#### (2) 编译与运行
```bash
mkdir -p build && cd build
cmake ..
make -j4
./tcp_server_example
```

---

### 3. macOS 构建
```bash
brew install cmake log4cxx
mkdir -p build && cd build
cmake ..
make -j4
./tcp_server_example
```

---

## 日志系统使用说明

项目在 `include/net/logger.h` 提供了基于 printf 风格的便捷变参宏：

```cpp
#include "net/logger.h"

LOG_TRACE("Detailed trace message: code=%d", 100);
LOG_DEBUG("Worker thread #%d started", thread_idx);
LOG_INFO("Accepted connection from %s:%u", ip.c_str(), port);
LOG_WARN("Heartbeat timeout from %s:%u", ip.c_str(), port);
LOG_ERROR("Socket read error: %s (code: %d)", ec.message().c_str(), ec.value());
LOG_FATAL("Unrecoverable system error");
```

可通过修改 `log4cxx.properties` 动态调整日志级别、控制台格式以及文件切分策略。
