#pragma once

#include <sstream>

#include "log_callback.h"

#if defined(ANDROID)
#include <android/log.h>
#else
#include <sys/time.h>

#include <ctime>
#include <iostream>
#include <string>
#endif

#if !defined(WINDOWS)
// Remove path and extract only filename.
#define FILENAME \
    (__builtin_strrchr(__FILE__, '/') ? __builtin_strrchr(__FILE__, '/') + 1 : __FILE__)
#else
#define FILENAME __FILE__
#endif

namespace base {

#define call_user_callback(...) call_user_callback_located(FILENAME, __LINE__, __VA_ARGS__)

#define LogDebug() LogDebugDetailed(FILENAME, __LINE__)
#define LogInfo() LogInfoDetailed(FILENAME, __LINE__)
#define LogWarn() LogWarnDetailed(FILENAME, __LINE__)
#define LogError() LogErrDetailed(FILENAME, __LINE__)

enum class Color {
    Red,
    Green,
    Yellow,
    Blue,
    Gray,
    Reset
};

void set_color(Color color);

class LogDetailed {
public:
    LogDetailed(const char *filename, int filenumber)
        : _s(), _caller_filename(filename), _caller_filenumber(filenumber) {}

    template <typename T>
    LogDetailed &operator<<(const T &x) {
        _s << x;
        return *this;
    }

    virtual ~LogDetailed() {
        if (log::get_callback() &&
            log::get_callback()(_log_level, _s.str(), _caller_filename, _caller_filenumber)) {
            return;
        }

#if ANDROID
        switch (_log_level) {
            case log::Level::Debug:
                __android_log_print(ANDROID_LOG_DEBUG, "Mavsdk", "%s", _s.str().c_str());
                break;
            case log::Level::Info:
                __android_log_print(ANDROID_LOG_INFO, "Mavsdk", "%s", _s.str().c_str());
                break;
            case log::Level::Warn:
                __android_log_print(ANDROID_LOG_WARN, "Mavsdk", "%s", _s.str().c_str());
                break;
            case log::Level::Err:
                __android_log_print(ANDROID_LOG_ERROR, "Mavsdk", "%s", _s.str().c_str());
                break;
        }
        // Unused:
        (void)_caller_filename;
        (void)_caller_filenumber;
#else

        switch (_log_level) {
            case log::Level::Debug:
                set_color(Color::Green);
                break;
            case log::Level::Info:
                set_color(Color::Blue);
                break;
            case log::Level::Warn:
                set_color(Color::Yellow);
                break;
            case log::Level::Err:
                set_color(Color::Red);
                break;
        }

        auto current_time_with_ms = []() -> std::string {
            char time_buffer[13];  // "HH:MM:SS.mmm" + null terminator

            struct timeval tv;
            gettimeofday(&tv, NULL);  // Get current time: seconds + microseconds

            struct tm *local_time = localtime(&tv.tv_sec);  // Convert to local time

            int milliseconds = tv.tv_usec / 1000;

            // Format into HH:MM:SS.mmm
            snprintf(time_buffer, sizeof(time_buffer), "%02d:%02d:%02d.%03d", local_time->tm_hour,
                     local_time->tm_min, local_time->tm_sec, milliseconds);
            return std::string(time_buffer);
        };
        std::cout << "[" << current_time_with_ms();

        switch (_log_level) {
            case log::Level::Debug:
                std::cout << "|Debug] ";
                break;
            case log::Level::Info:
                std::cout << "|Info ] ";
                break;
            case log::Level::Warn:
                std::cout << "|Warn ] ";
                break;
            case log::Level::Err:
                std::cout << "|Error] ";
                break;
        }

        set_color(Color::Reset);

        std::cout << _s.str();
        std::cout << " (" << _caller_filename << ":" << std::dec << _caller_filenumber << ")";

        std::cout << '\n';
#endif
    }

    LogDetailed(const base::LogDetailed &) = delete;
    void operator=(const base::LogDetailed &) = delete;
protected:
    log::Level _log_level = log::Level::Debug;
private:
    std::stringstream _s;
    const char *_caller_filename;
    int _caller_filenumber;
};

class LogDebugDetailed : public LogDetailed {
public:
    LogDebugDetailed(const char *filename, int filenumber) : LogDetailed(filename, filenumber) {
        _log_level = log::Level::Debug;
    }
};

class LogInfoDetailed : public LogDetailed {
public:
    LogInfoDetailed(const char *filename, int filenumber) : LogDetailed(filename, filenumber) {
        _log_level = log::Level::Info;
    }
};

class LogWarnDetailed : public LogDetailed {
public:
    LogWarnDetailed(const char *filename, int filenumber) : LogDetailed(filename, filenumber) {
        _log_level = log::Level::Warn;
    }
};

class LogErrDetailed : public LogDetailed {
public:
    LogErrDetailed(const char *filename, int filenumber) : LogDetailed(filename, filenumber) {
        _log_level = log::Level::Err;
    }
};

}  // namespace base
