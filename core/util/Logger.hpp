#pragma once

#include <cstdio>
#include <string>


namespace adofai {

enum class LogLevel { Debug, Info, Warning, Error };

class Logger {
public:
    static Logger& instance();

    void init(const std::string& logPath, bool debugConsole = false);
    void setDebugConsole(bool on) { m_debugConsole = on; }

    void log(LogLevel level, const char* fmt, ...);

private:
    FILE* m_file = nullptr;
    bool m_debugConsole = false;
    Logger() = default;
};

#define LOG_D(fmt, ...) Logger::instance().log(LogLevel::Debug,   fmt, ##__VA_ARGS__)
#define LOG_I(fmt, ...) Logger::instance().log(LogLevel::Info,    fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) Logger::instance().log(LogLevel::Warning, fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) Logger::instance().log(LogLevel::Error,   fmt, ##__VA_ARGS__)

}  // namespace adofai
