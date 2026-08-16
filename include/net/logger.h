#ifndef TCP_SERVER_NET_LOGGER_H_
#define TCP_SERVER_NET_LOGGER_H_

#include <log4cxx/logger.h>
#include <log4cxx/level.h>

#include <string>
#include <cstdarg>

namespace net {

/**
 * @brief Logger: 统一的 log4cxx 日志管理与格式化封装类。
 *
 * 提供线程安全的日志初始化、根 Logger 获取以及 printf 变参格式化接口。
 */
class Logger {
public:
    /**
     * @brief 初始化日志系统。
     * @param config_file 配置文件路径（支持 .properties 或 .xml）。
     *                    若为空或文件不存在，将首先尝试当前目录下的 log4cxx.properties，
     *                    仍不存在则自动回退至控制台 BasicConfigurator 默认配置。
     */
    static void Init(const std::string& config_file = "");

    /**
     * @brief 获取全局统一的根 Logger 实例。
     * @return log4cxx::LoggerPtr 智能指针
     */
    static log4cxx::LoggerPtr GetRootLogger();

    /**
     * @brief 格式化并输出日志。
     * @param level 日志级别（LevelPtr）
     * @param location 代码位置信息（包含文件名、行号、函数名）
     * @param format printf 风格格式化字符串
     */
    static void Log(log4cxx::LevelPtr level, const log4cxx::spi::LocationInfo& location, const char* format, ...);

private:
    /**
     * @brief 确保日志系统已完成基本初始化（惰性初始化保证）。
     */
    static void EnsureInitialized();
};

} // namespace net

// ================= printf 风格变参日志宏定义 ================= //

#define LOG_TRACE(fmt, ...) \
    do { \
        if (::net::Logger::GetRootLogger()->isTraceEnabled()) { \
            ::net::Logger::Log(::log4cxx::Level::getTrace(), LOG4CXX_LOCATION, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define LOG_DEBUG(fmt, ...) \
    do { \
        if (::net::Logger::GetRootLogger()->isDebugEnabled()) { \
            ::net::Logger::Log(::log4cxx::Level::getDebug(), LOG4CXX_LOCATION, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define LOG_INFO(fmt, ...) \
    do { \
        if (::net::Logger::GetRootLogger()->isInfoEnabled()) { \
            ::net::Logger::Log(::log4cxx::Level::getInfo(), LOG4CXX_LOCATION, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define LOG_WARN(fmt, ...) \
    do { \
        if (::net::Logger::GetRootLogger()->isWarnEnabled()) { \
            ::net::Logger::Log(::log4cxx::Level::getWarn(), LOG4CXX_LOCATION, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define LOG_ERROR(fmt, ...) \
    do { \
        if (::net::Logger::GetRootLogger()->isErrorEnabled()) { \
            ::net::Logger::Log(::log4cxx::Level::getError(), LOG4CXX_LOCATION, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define LOG_FATAL(fmt, ...) \
    do { \
        if (::net::Logger::GetRootLogger()->isFatalEnabled()) { \
            ::net::Logger::Log(::log4cxx::Level::getFatal(), LOG4CXX_LOCATION, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#endif // TCP_SERVER_NET_LOGGER_H_
