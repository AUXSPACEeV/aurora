/**
* @file macros.h
* @brief Compiler macros
*
* Author: Maximilian Stephan @ Auxspace e.V.
* Copyright (C) 2025 Auxspace e.V.
*/

#define __swab16(x) __fswab16(x)
#define __swab32(x) __fswab32(x)
#define __swab64(x) __fswab64(x)

#ifndef likely
#define likely(expr) __builtin_expect(!!(expr), 1)
#endif

#ifndef unlikely
#define unlikely(expr) __builtin_expect(!!(expr), 0)
#endif

#ifndef BIT
#define BIT(x)  ((uint32_t)(1 << (x)))
#endif

#ifndef cpu_to_be32
#define cpu_to_be32(x)  ((uint32_t)__swab32((x)))
#endif

/* [] END OF FILE */