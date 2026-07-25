#pragma once

#include "common/log/logger.h"

#ifndef LOG_MAIN_TRACE_AT
#define LOG_MAIN_TRACE_AT(...) LOG_TRACE(__VA_ARGS__)
#endif

#ifndef LOG_MAIN_DEBUG_AT
#define LOG_MAIN_DEBUG_AT(...) LOG_DEBUG(__VA_ARGS__)
#endif

#ifndef LOG_MAIN_INFO_AT
#define LOG_MAIN_INFO_AT(...) LOG_INFO(__VA_ARGS__)
#endif

#ifndef LOG_MAIN_WARN_AT
#define LOG_MAIN_WARN_AT(...) LOG_WARN(__VA_ARGS__)
#endif

#ifndef LOG_MAIN_ERROR_AT
#define LOG_MAIN_ERROR_AT(...) LOG_ERROR(__VA_ARGS__)
#endif

#ifndef LOG_MAIN_CRITICAL_AT
#define LOG_MAIN_CRITICAL_AT(...) LOG_CRITICAL(__VA_ARGS__)
#endif
