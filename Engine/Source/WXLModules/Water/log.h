#pragma once

#include <cstdarg>

enum class LogLevel : int { Error = 0, Info = 1, Debug = 2 };

void LogOpen(const char* path);
void LogSetLevel(int level);
bool LogEnabled(LogLevel level);
void LogWrite(LogLevel level, const char* fmt, ...);

#define VF_LOG_ERROR(...) LogWrite(LogLevel::Error, __VA_ARGS__)
#define VF_LOG_INFO(...) LogWrite(LogLevel::Info, __VA_ARGS__)
#define VF_LOG_DEBUG(...) LogWrite(LogLevel::Debug, __VA_ARGS__)
