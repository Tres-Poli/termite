#include "pch.h"

#include <stdio.h>

#include "logger.h"

#if BX_PLATFORM_WINDOWS
#   define WIN32_LEAN_AND_MEAN
#   include <Windows.h>
#elif BX_PLATFORM_ANDROID
#   include <android/log.h>
#endif

#define EXCLUDE_LIST_COUNT 6

namespace tee
{
    struct Logger
    {
        FILE* logFile;
        FILE* errFile;
        LogCallbackFn callback;
        void* userParam;

        LogTimeFormat::Enum timeFormat;

        LogType::Enum excludeList[EXCLUDE_LIST_COUNT];
        int numExcludes;
        int numErrors;
        int numWarnings;
        int numMessages;
        LogColor::Enum colorOverride;

#if BX_PLATFORM_WINDOWS
        HANDLE consoleHdl;
        WORD consoleAttrs;
#endif
        char tag[32];
        bool timestamps;
        bool insideProgress;

        Logger()
        {
            logFile = nullptr;
            errFile = nullptr;
            callback = nullptr;
            userParam = nullptr;
            insideProgress = false;
            timestamps = false;
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

    static Logger gLogger;

    bool debug::setLogToFile(const char* filepath, const char* errFilepath)
    {
        disableLogToFile();

        gLogger.logFile = fopen(filepath, "wt");
        if (!gLogger.logFile)
            return false;
        if (errFilepath) {
            gLogger.errFile = fopen(errFilepath, "wt");
            if (!gLogger.errFile)
                return false;
        }

        return true;
    }

    bool debug::setLogToTerminal()
    {
        disableLogToFile();

        gLogger.logFile = stdout;
        gLogger.errFile = stderr;

#if BX_PLATFORM_WINDOWS
        gLogger.consoleHdl = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO coninfo;
        GetConsoleScreenBufferInfo(gLogger.consoleHdl, &coninfo);
        gLogger.consoleAttrs = coninfo.wAttributes;
#endif

        return true;
    }

    void debug::setLogToCallback(LogCallbackFn callback, void* userParam)
    {
        BX_ASSERT(callback);

        gLogger.callback = callback;
        gLogger.userParam = userParam;
    }

    void debug::setLogTimestamps(LogTimeFormat::Enum timeFormat)
    {
        gLogger.timestamps = true;
        gLogger.timeFormat = timeFormat;
    }

    void debug::disableLogToFile()
    {
#if BX_PLATFORM_WINDOWS
        if (gLogger.consoleHdl) {
            SetConsoleTextAttribute(gLogger.consoleHdl, gLogger.consoleAttrs);
            CloseHandle(gLogger.consoleHdl);
            gLogger.consoleHdl = nullptr;
        }
#endif

        if (gLogger.logFile && gLogger.logFile != stdout) {
            fclose(gLogger.logFile);
            gLogger.logFile = nullptr;
        }

        if (gLogger.errFile && gLogger.logFile != stderr) {
            fclose(gLogger.errFile);
            gLogger.errFile = nullptr;
        }
    }

    void debug::disableLogToCallback()
    {
        gLogger.callback = nullptr;
    }

    void debug::disableLogTimestamps()
    {
        gLogger.timestamps = false;
    }

    void debug::setLogTag(const char* tag)
    {
        bx::strCopy(gLogger.tag, sizeof(gLogger.tag), tag);
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

        __android_log_write(pr, gLogger.tag, text);
    }
#endif

    static void logPrintRaw(const char* filename, int line, LogType::Enum type, LogExtraParam::Enum extra, const char* text)
    {
        // Filter out mesages that are in exclude filter
        if (gLogger.numExcludes) {
            for (int i = 0, c = gLogger.numExcludes; i < c; i++) {
                if (gLogger.excludeList[i] == type)
                    return;
            }
        }

        // Add counter
        switch (type) {
        case LogType::Fatal:
            gLogger.numErrors++;   break;
        case LogType::Warning:
            gLogger.numWarnings++; break;
        default:                break;
        }

        switch (extra) {
        case LogExtraParam::ProgressEndFatal:
            gLogger.numErrors++;   break;
        case LogExtraParam::ProgressEndNonFatal:
            gLogger.numWarnings++; break;
        default:            break;
        }
        gLogger.numMessages++;


        // Timestamps
        char timestr[32];
        timestr[0] = 0;
        time_t t = 0;
        if (gLogger.timestamps) {
            t = time(nullptr);
            tm* timeinfo = localtime(&t);

            if (gLogger.timeFormat == LogTimeFormat::Time) {
                snprintf(timestr, sizeof(timestr), "%.2d:%.2d:%.2d",
                         timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
            } else if (gLogger.timeFormat == LogTimeFormat::DateTime) {
                snprintf(timestr, sizeof(timestr), "%.2d/%.2d/%.2d %.2d %.2d %.2d",
                         timeinfo->tm_mon, timeinfo->tm_mday, (timeinfo->tm_year + 1900) % 1000,
                         timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
            }
        }

        // File/Std streams
        if (gLogger.logFile) {
            const char* prefix = "";
            const char* post = "";
            bool formatted = false;
            if (gLogger.logFile == stdout) {
                formatted = true;
                // Choose color for the log line
#if !BX_PLATFORM_WINDOWS
                if (gLogger.colorOverride == LogColor::None) {
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
                    switch (gLogger.colorOverride) {
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
                if (gLogger.colorOverride == LogColor::None) {
                    if (extra == LogExtraParam::None || extra == LogExtraParam::InProgress) {
                        switch (type) {
                        case LogType::Text:
                            SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);        break;
                        case LogType::Verbose:
                        case LogType::Debug:
                            SetConsoleTextAttribute(gLogger.consoleHdl, gLogger.consoleAttrs);          break;
                        case LogType::Fatal:
                            SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_RED | FOREGROUND_INTENSITY);      break;
                        case LogType::Warning:
                            SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);   break;
                        default:
                            break;
                        }
                    } else {
                        switch (extra) {
                        case LogExtraParam::ProgressEndOk:
                            SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_GREEN | FOREGROUND_INTENSITY);    break;
                        case LogExtraParam::ProgressEndFatal:
                            SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_RED | FOREGROUND_INTENSITY);      break;
                        case LogExtraParam::ProgressEndNonFatal:
                            SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);   break;
                        default:
                            break;
                        }
                    }
                } else {
                    switch (gLogger.colorOverride) {
                    case LogColor::Black:
                        SetConsoleTextAttribute(gLogger.consoleHdl, 0);
                        break;
                    case LogColor::Cyan:
                        SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_BLUE | FOREGROUND_GREEN);
                        break;
                    case LogColor::Gray:
                        SetConsoleTextAttribute(gLogger.consoleHdl, gLogger.consoleAttrs);
                        break;
                    case LogColor::Green:
                        SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_GREEN);
                        break;
                    case LogColor::Magenta:
                        SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_RED | FOREGROUND_BLUE);
                        break;
                    case LogColor::Red:
                        SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_RED);
                        break;
                    case LogColor::White:
                        SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_RED | FOREGROUND_BLUE);
                        break;
                    case LogColor::Yellow:
                        SetConsoleTextAttribute(gLogger.consoleHdl, FOREGROUND_RED | FOREGROUND_GREEN);
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
            FILE* output = gLogger.logFile;
            if (gLogger.errFile && type == LogType::Fatal)
                output = gLogger.errFile;
            if (output) {
#if BX_PLATFORM_ANDROID
                if (output == stdout || output == stderr) {
                    if (extra == LogExtraParam::None || extra == LogExtraParam::InProgress)
                        logPrintRawAndroid(type, text);
                }
#else
                if (!gLogger.timestamps || (extra != LogExtraParam::InProgress && extra != LogExtraParam::None)) {
                    fprintf(output, "%s%s%s", prefix, text, post);
                } else {
                    fprintf(output, "[%s] %s%s%s", timestr, prefix, text, post);
                }
#endif
            }
        }

        // Callback
        if (gLogger.callback)
            gLogger.callback(filename, line, type, text, gLogger.userParam, extra, t);
    }

    void debug::printf(const char* sourceFile, int line, LogType::Enum type, const char* fmt, ...)
    {
        char text[4096];

        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);

        logPrintRaw(sourceFile, line, type, LogExtraParam::None, text);
    }

    void debug::print(const char* sourceFile, int line, LogType::Enum type, const char* text)
    {
        logPrintRaw(sourceFile, line, type, LogExtraParam::None, text);
    }

    void debug::beginProgress(const char* sourceFile, int line, const char* fmt, ...)
    {
        char text[4096];

        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);

        gLogger.insideProgress = true;
        logPrintRaw(sourceFile, line, LogType::Text, LogExtraParam::InProgress, text);
    }

    void debug::endProgress(LogProgressResult::Enum result)
    {
        gLogger.insideProgress = false;

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

    void debug::excludeFromLog(LogType::Enum type)
    {
        if (gLogger.numExcludes == EXCLUDE_LIST_COUNT)
            return;

        for (int i = 0; i < gLogger.numExcludes; i++) {
            if (type == gLogger.excludeList[i])
                return;
        }

        gLogger.excludeList[gLogger.numExcludes++] = type;
    }

    void debug::includeToLog(LogType::Enum type)
    {
        for (int i = 0; i < gLogger.numExcludes; i++) {
            if (type == gLogger.excludeList[i]) {
                for (int c = i + 1; c < gLogger.numExcludes; c++)
                    gLogger.excludeList[c - 1] = gLogger.excludeList[c];
                gLogger.numExcludes--;
                break;
            }
        }
    }

    void debug::overrideLogColor(LogColor::Enum color)
    {
        gLogger.colorOverride = color;
    }

}
