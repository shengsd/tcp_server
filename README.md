# C++11 High-Performance TCP Server (Standalone Asio)

基于 Linux C++11 和 **Standalone Asio** 实现的高性能 TCP 服务端框架，采用 **One io_context per Thread (IO 线程池)** 架构，内置心跳超时检测、发送缓冲队列与 C++11 事件回调。

---

## CentOS 7 构建指南

CentOS 7 默认环境极其轻量，构建本库有两种方式：

### 方式 A：标准自动化构建 (推荐)

项目 CMake 已配置 **自动依赖兜底**：如果 CentOS 7 系统中未安装 `asio-devel`，CMake 会自动从 GitHub 下载 Standalone Asio 纯头文件，**无需手工配置依赖**。

1. **安装构建工具**：
   ```bash
   sudo yum install -y gcc-c++ make cmake3 git
   ```

2. **编译**：
   ```bash
   cd tcp_server
   mkdir -p build && cd build
   cmake3 ..
   make -j4
   ```

3. **运行测试**：
   ```bash
   ./tcp_server_example
   ```

---

### 方式 B：使用 yum 安装系统 asio-devel 包

如果您希望直接使用 CentOS EPEL 源中的 asio 包：

```bash
# 1. 安装 epel-release 与 asio-devel
sudo yum install -y epel-release
sudo yum install -y asio-devel cmake3 gcc-c++ make

# 2. 编译
mkdir -p build && cd build
cmake3 ..
make -j4
```

> **提示 (CentOS 7 编译器建议)**：
> CentOS 7 自带 GCC 4.8.5 已完整支持 `-std=c++11`。如需提升性能或使用更新编译器，可启用 scl `devtoolset-9`：
> ```bash
> sudo yum install -y centos-release-scl
> sudo yum install -y devtoolset-9-gcc-c++
> scl enable devtoolset-9 bash
> ```
