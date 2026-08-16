#include "net/logger.h"

#include <log4cxx/logger.h>
#include <log4cxx/basicconfigurator.h>
#include <log4cxx/propertyconfigurator.h>
#include <log4cxx/xml/domconfigurator.h>
#include <log4cxx/patternlayout.h>
#include <log4cxx/consoleappender.h>
#include <log4cxx/logmanager.h>

#include <mutex>
#include <fstream>
#include <vector>
#include <cstdio>

namespace net {

static std::once_flag g_init_flag;
static bool g_is_custom_initialized = false;

static bool FileExists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream f(path.c_str());
    return f.good();
}

static bool EndsWith(const std::string& str, const std::string& suffix) {
    if (str.length() < suffix.length()) return false;
    return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
}

void Logger::Init(const std::string& config_file) {
    std::call_once(g_init_flag, [&]() {
        if (!config_file.empty() && FileExists(config_file)) {
            if (EndsWith(config_file, ".xml")) {
                log4cxx::xml::DOMConfigurator::configure(config_file);
            } else {
                log4cxx::PropertyConfigurator::configure(config_file);
            }
            g_is_custom_initialized = true;
            return;
        }

        // 尝试自动查找当前目录下的标准配置文件
        if (FileExists("log4cxx.properties")) {
            log4cxx::PropertyConfigurator::configure("log4cxx.properties");
            g_is_custom_initialized = true;
            return;
        } else if (FileExists("log4cxx.xml")) {
            log4cxx::xml::DOMConfigurator::configure("log4cxx.xml");
            g_is_custom_initialized = true;
            return;
        }

        // 回退默认配置：输出到控制台，带时间戳和级别格式
        log4cxx::LayoutPtr layout(new log4cxx::PatternLayout("%d{yyyy-MM-dd HH:mm:ss.SSS} [%t] %-5p %c - %m%n"));
        log4cxx::AppenderPtr appender(new log4cxx::ConsoleAppender(layout));
        log4cxx::BasicConfigurator::configure(appender);
        g_is_custom_initialized = true;
    });
}

void Logger::EnsureInitialized() {
    if (!g_is_custom_initialized) {
        Init("");
    }
}

log4cxx::LoggerPtr Logger::GetRootLogger() {
    EnsureInitialized();
    static log4cxx::LoggerPtr root_logger = log4cxx::Logger::getLogger("TcpServer");
    return root_logger;
}

void Logger::Log(log4cxx::LevelPtr level, const log4cxx::spi::LocationInfo& location, const char* format, ...) {
    if (!format) return;

    EnsureInitialized();

    char buffer[1024];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (len < 0) return;

    if (static_cast<size_t>(len) < sizeof(buffer)) {
        GetRootLogger()->forcedLog(level, std::string(buffer, len), location);
    } else {
        std::vector<char> heap_buf(len + 1);
        va_start(args, format);
        vsnprintf(heap_buf.data(), heap_buf.size(), format, args);
        va_end(args);
        GetRootLogger()->forcedLog(level, std::string(heap_buf.data(), len), location);
    }
}

} // namespace net
