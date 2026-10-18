#pragma once

#include <cstdio>
#include <string>


namespace adofai {

enum class LogLevel { Debug, Info, Warning, Error };

class Logger {
public:
    static Logger& instance();

    void init(const std::string& logPath, bool debugConsole = false);

    // 本进程实际在写的日志文件（init 之后有效）。给"必须落在日志旁边"的附属文件用
    // —— 目前只有 GameWindow 的 profiler（ADOCAO.prof）；它原来按 CWD 落，用户从
    // .bat/快捷方式启动时文件跑到别处，看起来就是"profiler 什么也没留下"。
    const std::string& path() const { return m_path; }

    void log(LogLevel level, const char* fmt, ...);

private:
    FILE* m_file = nullptr;
    bool m_debugConsole = false;
    std::string m_path;
    Logger() = default;
};

#define LOG_D(fmt, ...) Logger::instance().log(LogLevel::Debug,   fmt, ##__VA_ARGS__)
#define LOG_I(fmt, ...) Logger::instance().log(LogLevel::Info,    fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) Logger::instance().log(LogLevel::Warning, fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) Logger::instance().log(LogLevel::Error,   fmt, ##__VA_ARGS__)

}  // namespace adofai
