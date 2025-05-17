/**
* @file compiler.h
* @brief Compiler macros
*
* Author: Maximilian Stephan @ Auxspace e.V.
* Copyright (C) 2025 Auxspace e.V.
*/

#ifndef likely
#define likely(expr) __builtin_expect(!!(expr), 1)
#endif

#ifndef unlikely
#define unlikely(expr) __builtin_expect(!!(expr), 0)
#endif
