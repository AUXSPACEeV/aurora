/**
 * @file log.h
 * @brief Logging header
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */
#pragma once

#include <generated/autoconf.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"


typedef enum log_level {
    LOG_TRACE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
} log_level_t;

/**
 * @brief Convert log level to string representation.
 *
 * @param level The log level.
 * @return The string representation of the log level.
 */
const char *log_level_to_string(log_level_t level);

/**
 * @brief Convert log level to ANSI color code.
 *
 * @param level The log level.
 * @return The color code for the log level.
 */
const char *log_level_to_color(log_level_t level);

void log_message(log_level_t level, const char *file, int line, const char *fmt, ...);

/**
 * @brief perform a memory dump with every byte's value being printed in hex
 * 
 * @param data: pointer to data
 * @param size: length (number of bytes) of the data to dump
 */
void hexdump(const void *data, size_t size);

/**
 * @brief Get the current time as a formatted string
 */
#define CURRENT_TIME()   ({ uint64_t t = to_us_since_boot(get_absolute_time()); char buffer[26]; snprintf(buffer, sizeof(buffer), "%lld", t); buffer; })

#ifdef CONFIG_AURORA_TRACING
/**
 * @brief Log a message with level "trace"
 * 
 * @param fmt: String with format escape sequences
 * @param args: arguments for formatted string
 */
#define log_trace(fmt, ...)    log_message(LOG_TRACE, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
#define log_trace(fmt, ...)    {}
#endif /* CONFIG_AURORA_TRACING */


#ifdef CONFIG_AURORA_DEBUG_BUILD
/**
 * @brief Log a message with level "debug"
 * 
 * @param fmt: String with format escape sequences
 * @param args: arguments for formatted string
 */
#define log_debug(fmt, ...)    log_message(LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
#define log_debug(fmt, ...)    {}
#endif /* CONFIG_AURORA_DEBUG_BUILD */

/**
 * @brief Log a message with level "info"
 * 
 * @param fmt: String with format escape sequences
 * @param args: arguments for formatted string
 */
#define log_info(fmt, ...)     log_message(LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief Log a message with level "warning"
 * 
 * @param fmt: String with format escape sequences
 * @param args: arguments for formatted string
 */
#define log_warning(fmt, ...)  log_message(LOG_WARNING, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief Log a message with level "error"
 * 
 * @param fmt: String with format escape sequences
 * @param args: arguments for formatted string
 */
#define log_error(fmt, ...)    log_message(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* [] END OF FILE */