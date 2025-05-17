/**
 * @file log.c
 * @brief Auxspace Logging Framework (makes it sound cooler than "fancy printf")
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <aurora/log.h>

const char *log_level_to_string(log_level_t level) {
    switch (level) {
        case LOG_ERROR:   return "ERROR";
        case LOG_WARNING: return "WARNING";
        case LOG_INFO:    return "INFO";
        case LOG_DEBUG:   return "DEBUG";
        case LOG_TRACE:   return "TRACE";
        default:          return "UNKNOWN";
    }
}

const char *log_level_to_color(log_level_t level) {
    switch (level) {
        case LOG_ERROR:   return "\033[1;31m";  // Bright red
        case LOG_WARNING: return "\033[1;33m";  // Bright yellow
        case LOG_INFO:    return "\033[1;34m";  // Bright blue
        case LOG_DEBUG:   return "\033[1;32m";  // Bright green
        case LOG_TRACE:   return "\033[1;35m";  // Bright magenta
        default:          return "\033[0m";     // Reset
    }
}

void log_message(log_level_t level, const char *file, int line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    // Get current time for logging
    const char *time_str = CURRENT_TIME();
    // Convert log level to string
    const char *level_str = log_level_to_string(level);
    // Convert log level to color
    const char *level_color = log_level_to_color(level);
    // Reset color after the log message
    const char *reset_color = "\033[0m";

    /* Only print extended info on error, warning and trace. */
    switch (level) {
        case LOG_ERROR:
        case LOG_WARNING:
        case LOG_TRACE:
            printf("[%s] [%s%s%s] %s:%d: ", time_str, level_color, level_str, reset_color, file, line);
            break;
        default:
            printf("[%s] [%s%s%s]: ", time_str, level_color, level_str, reset_color);
    }

    // Print the formatted message
    vprintf(fmt, args);
    va_end(args);

    size_t len = strlen(fmt);
    if (len == 0 || fmt[len - 1] != '\n') {
        // Append newline if it doesn't already end with one
        printf("\n");
    }
}

void hexdump(const void *data, size_t size) {
    const unsigned char *ptr = (const unsigned char *)data;
    size_t i, j;

    log_debug("Requested hex dump for 0x%016llx - 0x%016llx.",
              (uintptr_t) data, ((uintptr_t) data) + size);
    for (i = 0; i < size; i += 16) {
        // Print offset/address
        printf("%08zx: ", i);

        // Print hex bytes
        for (j = 0; j < 16; ++j) {
            if (i + j < size) {
                printf("%02x ", ptr[i + j]);
            } else {
                printf("   ");
            }
        }

        // Print ASCII representation
        printf(" |");
        for (j = 0; j < 16; ++j) {
            if (i + j < size) {
                unsigned char c = ptr[i + j];
                printf("%c", (c >= 32 && c <= 126) ? c : '.');
            } else {
                printf(" ");
            }
        }
        printf("|\n");
    }
}
