#include "Logger.hpp"
#include <cstdarg>
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>


namespace adofai {

static const char* levelStr(LogLevel lv) {
    switch (lv) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
    }
    return "?";
}

static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << tm.tm_year + 1900 << '.'
        << std::setw(2) << tm.tm_mon + 1 << '.'
        << std::setw(2) << tm.tm_mday << ' '
        << std::setw(2) << tm.tm_hour << ':'
        << std::setw(2) << tm.tm_min << ':'
        << std::setw(2) << tm.tm_sec << '.'
        << std::setw(3) << ms.count();
    return oss.str();
}

Logger& Logger::instance() {
    static Logger s;
    return s;
}

void Logger::init(const std::string& logPath, bool debugConsole) {
    if (m_file) fclose(m_file);
    m_file = fopen(logPath.c_str(), "a");
    m_debugConsole = debugConsole;
    if (m_file) {
        fprintf(m_file, "=== ADOCAO Log started at %s ===\n", timestamp().c_str());
        fflush(m_file);
    }
}

void Logger::log(LogLevel level, const char* fmt, ...) {
    std::string ts = timestamp();
    std::string prefix = "[ " + ts + " | " + levelStr(level) + " ] ";

    va_list args;

    if (m_file) {
        fprintf(m_file, "%s", prefix.c_str());
        va_start(args, fmt);
        vfprintf(m_file, fmt, args);
        va_end(args);
        fprintf(m_file, "\n");
        fflush(m_file);
    }

    if (m_debugConsole) {
        fprintf(stderr, "%s", prefix.c_str());
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
    }
}

}  // namespace adofai
