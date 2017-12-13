#include "bxx/logger.h"

#include <cassert>
#include <ctime>
#include <atomic>
#include <cstdio>

#include "bx/string.h"

#if BX_PLATFORM_WINDOWS
#   define WIN32_LEAN_AND_MEAN
#   include <Windows.h>
#elif BX_PLATFORM_ANDROID
#   include <android/log.h>
#endif

#define EXCLUDE_LIST_COUNT 6

#define CHECK_LOGGER_INIT() if (!g_logger) { g_logger = new Logger(); }

namespace bx
{
    struct Logger
    {
        std::atomic_bool timestamps;

        FILE* logFile;
        FILE* errFile;
        LogCallbackFn callback;
        void* userParam;

        std::atomic_bool insideProgress;
        LogTimeFormat::Enum timeFormat;

        LogType::Enum excludeList[EXCLUDE_LIST_COUNT];
        std::atomic_int numExcludes;
        std::atomic_int numErrors;
        std::atomic_int numWarnings;
        std::atomic_int numMessages;
        LogColor::Enum colorOverride;

#if BX_PLATFORM_WINDOWS
        HANDLE consoleHdl;
        WORD consoleAttrs;
#endif
        char tag[32];

        Logger()
        {
            timestamps = false;
            logFile = nullptr;
            errFile = nullptr;
            callback = nullptr;
            userParam = nullptr;
            insideProgress = false;
            timeFormat = LogTimeFormat::Time;
            numExcludes = 0;
            numErrors = 0;
            numWarnings = 0;
            numMessages = 0;
            colorOverride = LogColor::None;
            bx::memSet(excludeList, 0x00, sizeof(excludeList));
            tag[0] = 0;

#if BX_PLATFORM_WINDOWS
            consoleHdl = nullptr;
            consoleAttrs = 0;
#endif
        }
    };

    static Logger* g_logger = nullptr;

    bool enableLogToFile(const char* filepath, const char* errFilepath)
    {
        CHECK_LOGGER_INIT();
        disableLogToFile();

        g_logger->logFile = fopen(filepath, "wt");
        if (!g_logger->logFile)
            return false;
        if (errFilepath) {
            g_logger->errFile = fopen(errFilepath, "wt");
            if (!g_logger->errFile)
                return false;
        }

        return true;
    }

    bool enableLogToFileHandle(FILE* file, FILE* errFile)
    {
        CHECK_LOGGER_INIT();
        disableLogToFile();

        if (g_logger->logFile == stdout && g_logger->errFile != stderr)
            return false;

        g_logger->logFile = file;
        g_logger->errFile = errFile;

#if BX_PLATFORM_WINDOWS
        if (file == stdout || errFile == stderr) {
            g_logger->consoleHdl = GetStdHandle(STD_OUTPUT_HANDLE);
            CONSOLE_SCREEN_BUFFER_INFO coninfo;
            GetConsoleScreenBufferInfo(g_logger->consoleHdl, &coninfo);
            g_logger->consoleAttrs = coninfo.wAttributes;
        }
#endif

        return true;
    }

    void enableLogToCallback(LogCallbackFn callback, void* userParam)
    {
        CHECK_LOGGER_INIT();
        assert(callback);

        g_logger->callback = callback;
        g_logger->userParam = userParam;
    }

    void enableLogTimestamps(LogTimeFormat::Enum timeFormat)
    {
        CHECK_LOGGER_INIT();
        g_logger->timestamps = true;
        g_logger->timeFormat = timeFormat;
    }

    void disableLogToFile()
    {
        CHECK_LOGGER_INIT();
#if BX_PLATFORM_WINDOWS
        if (g_logger->consoleHdl) {
            SetConsoleTextAttribute(g_logger->consoleHdl, g_logger->consoleAttrs);
            CloseHandle(g_logger->consoleHdl);
            g_logger->consoleHdl = nullptr;
        }
#endif

        if (g_logger->logFile) {
            fclose(g_logger->logFile);
            g_logger->logFile = nullptr;
        }

        if (g_logger->errFile) {
            fclose(g_logger->errFile);
            g_logger->errFile = nullptr;
        }
    }

    void disableLogToCallback()
    {
        CHECK_LOGGER_INIT();
        g_logger->callback = nullptr;
    }

    void disableLogTimestamps()
    {
        CHECK_LOGGER_INIT();
        g_logger->timestamps = false;
    }

    void setLogTag(const char* tag)
    {
        CHECK_LOGGER_INIT();
        bx::strCopy(g_logger->tag, sizeof(g_logger->tag), tag);
    }

#if BX_PLATFORM_ANDROID
    static void logPrintRawAndroid(LogType::Enum type, const char* text)
    {
        android_LogPriority pr;
        switch (type) {
        case LogType::Text:
            pr = ANDROID_LOG_INFO;
            break;
        case LogType::Verbose:
            pr = ANDROID_LOG_VERBOSE;
            break;
        case LogType::Fatal:
            pr = ANDROID_LOG_FATAL;
            break;
        case LogType::Warning:
            pr = ANDROID_LOG_WARN;
            break;
        case LogType::Debug:
            pr = ANDROID_LOG_DEBUG;
            break;

        default:
            pr = ANDROID_LOG_INFO;
            break;
        }

        __android_log_write(pr, g_logger->tag, text);
    }
#endif

    static void logPrintRaw(const char* filename, int line, LogType::Enum type, LogExtraParam::Enum extra, const char* text)
    {
        // Filter out mesages that are in exclude filter
        if (g_logger->numExcludes) {
            for (int i = 0, c = g_logger->numExcludes; i < c; i++) {
                if (g_logger->excludeList[i] == type)
                    return;
            }
        }

        // Add counter
        switch (type) {
        case LogType::Fatal:
            g_logger->numErrors++;   break;
        case LogType::Warning:
            g_logger->numWarnings++; break;
        default:                break;
        }

        switch (extra) {
        case LogExtraParam::ProgressEndFatal:
            g_logger->numErrors++;   break;
        case LogExtraParam::ProgressEndNonFatal:
            g_logger->numWarnings++; break;
        default:            break;
        }
        g_logger->numMessages++;


        // Timestamps
        char timestr[32];
        timestr[0] = 0;
        time_t t = 0;
        if (g_logger->timestamps) {
            t = time(nullptr);
            tm* timeinfo = localtime(&t);

            if (g_logger->timeFormat == LogTimeFormat::Time) {
                snprintf(timestr, sizeof(timestr), "%.2d:%.2d:%.2d",
                         timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
            } else if (g_logger->timeFormat == LogTimeFormat::DateTime) {
                snprintf(timestr, sizeof(timestr), "%.2d/%.2d/%.2d %.2d %.2d %.2d",
                         timeinfo->tm_mon, timeinfo->tm_mday, (timeinfo->tm_year + 1900) % 1000,
                         timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
            }
        }

        // File/Std streams
        if (g_logger->logFile) {
            const char* prefix = "";
            const char* post = "";
            bool formatted = false;
            if (g_logger->logFile == stdout) {
                formatted = true;
                // Choose color for the log line
#if !BX_PLATFORM_WINDOWS
                if (g_logger->colorOverride == LogColor::None) {
                    if (extra == LogExtraParam::None || extra == LogExtraParam::InProgress) {
                        switch (type) {
                        case LogType::Text:
                            prefix = TERM_RESET;        break;
                        case LogType::Verbose:
                        case LogType::Debug:
                            prefix = TERM_DIM;          break;
                        case LogType::Fatal:
                            prefix = TERM_RED_BOLD;      break;
                        case LogType::Warning:
                            prefix = TERM_YELLOW_BOLD;   break;
                        default:
                            prefix = TERM_RESET;        break;
                        }
                    } else {
                        switch (extra) {
                        case LogExtraParam::ProgressEndOk:
                            prefix = TERM_GREEN_BOLD;    break;
                        case LogExtraParam::ProgressEndFatal:
                            prefix = TERM_RED_BOLD;      break;
                        case LogExtraParam::ProgressEndNonFatal:
                            prefix = TERM_YELLOW_BOLD;   break;
                        default:
                            break;
                        }
                    }
                } else {
                    switch (g_logger->colorOverride) {
                    case LogColor::Black:
                        prefix = TERM_BLACK;                       break;
                    case LogColor::Cyan:
                        prefix = TERM_CYAN;                        break;
                    case LogColor::Gray:
                        prefix = TERM_DIM;                         break;
                    case LogColor::Green:
                        prefix = TERM_GREEN;                       break;
                    case LogColor::Magenta:
                        prefix = TERM_MAGENTA;                     break;
                    case LogColor::Red:
                        prefix = TERM_RED;                         break;
                    case LogColor::White:
                        prefix = TERM_WHITE;                       break;
                    case LogColor::Yellow:
                        prefix = TERM_YELLOW;                       break;
                    default:
                        break;
                    }
                }
#else
                if (g_logger->colorOverride == LogColor::None) {
                    if (extra == LogExtraParam::None || extra == LogExtraParam::InProgress) {
                        switch (type) {
                        case LogType::Text:
                            SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);        break;
                        case LogType::Verbose:
                        case LogType::Debug:
                            SetConsoleTextAttribute(g_logger->consoleHdl, g_logger->consoleAttrs);          break;
                        case LogType::Fatal:
                            SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_RED | FOREGROUND_INTENSITY);      break;
                        case LogType::Warning:
                            SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);   break;
                        default:
                            break;
                        }
                    } else {
                        switch (extra) {
                        case LogExtraParam::ProgressEndOk:
                            SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_GREEN | FOREGROUND_INTENSITY);    break;
                        case LogExtraParam::ProgressEndFatal:
                            SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_RED | FOREGROUND_INTENSITY);      break;
                        case LogExtraParam::ProgressEndNonFatal:
                            SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);   break;
                        default:
                            break;
                        }
                    }
                } else {
                    switch (g_logger->colorOverride) {
                    case LogColor::Black:
                        SetConsoleTextAttribute(g_logger->consoleHdl, 0);
                        break;
                    case LogColor::Cyan:
                        SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_BLUE | FOREGROUND_GREEN);
                        break;
                    case LogColor::Gray:
                        SetConsoleTextAttribute(g_logger->consoleHdl, g_logger->consoleAttrs);
                        break;
                    case LogColor::Green:
                        SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_GREEN);
                        break;
                    case LogColor::Magenta:
                        SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_RED | FOREGROUND_BLUE);
                        break;
                    case LogColor::Red:
                        SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_RED);
                        break;
                    case LogColor::White:
                        SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_RED | FOREGROUND_BLUE);
                        break;
                    case LogColor::Yellow:
                        SetConsoleTextAttribute(g_logger->consoleHdl, FOREGROUND_RED | FOREGROUND_GREEN);
                        break;
                    default:
                        break;
                    }
                }
#endif
            }

            if (extra != LogExtraParam::InProgress)
                post = "\n" TERM_RESET;
            else if (extra == LogExtraParam::InProgress)
                post = "... ";
            FILE* output = g_logger->logFile;
            if (g_logger->errFile && type == LogType::Fatal)
                output = g_logger->errFile;
            if (output) {
#if BX_PLATFORM_ANDROID
                if (output == stdout || output == stderr) {
                    if (extra == LogExtraParam::None || extra == LogExtraParam::InProgress)
                        logPrintRawAndroid(type, text);
                }
#else
                if (!g_logger->timestamps || (extra != LogExtraParam::InProgress && extra != LogExtraParam::None)) {
                    fprintf(output, "%s%s%s", prefix, text, post);
                } else {
                    fprintf(output, "[%s] %s%s%s", timestr, prefix, text, post);
                }
#endif
            }
        }

        // Callback
        if (g_logger->callback)
            g_logger->callback(filename, line, type, text, g_logger->userParam, extra, t);
    }

    void logPrintf(const char* sourceFile, int line, LogType::Enum type, const char* fmt, ...)
    {
        CHECK_LOGGER_INIT();
        char text[4096];

        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);

        logPrintRaw(sourceFile, line, type, LogExtraParam::None, text);
    }

    void logPrint(const char* sourceFile, int line, LogType::Enum type, const char* text)
    {
        CHECK_LOGGER_INIT();
        logPrintRaw(sourceFile, line, type, LogExtraParam::None, text);
    }

    void logBeginProgress(const char* sourceFile, int line, const char* fmt, ...)
    {
        CHECK_LOGGER_INIT();
        char text[4096];

        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);

        g_logger->insideProgress = true;
        logPrintRaw(sourceFile, line, LogType::Text, LogExtraParam::InProgress, text);
    }

    void logEndProgress(LogProgressResult::Enum result)
    {
        CHECK_LOGGER_INIT();
        g_logger->insideProgress = false;

        LogExtraParam::Enum extra;
        const char* text;
        switch (result) {
        case LogProgressResult::Ok:
            extra = LogExtraParam::ProgressEndOk;
            text = "[   OK   ]";
            break;

        case LogProgressResult::Fatal:
            extra = LogExtraParam::ProgressEndFatal;
            text = "[ FAILED ]";
            break;

        case LogProgressResult::NonFatal:
            extra = LogExtraParam::ProgressEndNonFatal;
            text = "[ FAILED ]";
            break;

        default:
            text = "";
            extra = LogExtraParam::None;
        }

        logPrintRaw(__FILE__, __LINE__, LogType::Text, extra, text);
    }

    void excludeFromLog(LogType::Enum type)
    {
        CHECK_LOGGER_INIT();
        if (g_logger->numExcludes == EXCLUDE_LIST_COUNT)
            return;

        for (int i = 0; i < g_logger->numExcludes; i++) {
            if (type == g_logger->excludeList[i])
                return;
        }

        g_logger->excludeList[g_logger->numExcludes++] = type;
    }

    void includeToLog(LogType::Enum type)
    {
        CHECK_LOGGER_INIT();
        for (int i = 0; i < g_logger->numExcludes; i++) {
            if (type == g_logger->excludeList[i]) {
                for (int c = i + 1; c < g_logger->numExcludes; c++)
                    g_logger->excludeList[c - 1] = g_logger->excludeList[c];
                g_logger->numExcludes--;
                break;
            }
        }
    }

    void overrideLogColor(LogColor::Enum color)
    {
        CHECK_LOGGER_INIT();
        g_logger->colorOverride = color;
    }

}
