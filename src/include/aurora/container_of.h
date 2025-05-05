/**
 * @file container_of.h
 * @brief Container library from the Linux Kernel
 *
 * Author: Linux Kernel Developers
 * Derived from:
 *  github.com/torvalds/linux/blob/master/include/linux/container_of.h
 */

/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

#include <stddef.h>

#define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))

/*
 * __unqual_scalar_typeof(x) - Declare an unqualified scalar type, leaving
 *                             non-scalar types unchanged.
 */
/*
 * Prefer C11 _Generic for better compile-times and simpler code. Note: 'char'
 * is not type-compatible with 'signed char', and we define a separate case.
 */
#define __scalar_type_to_expr_cases(type)                               \
                unsigned type:  (unsigned type)0,                       \
                signed type:    (signed type)0

#define __unqual_scalar_typeof(x) typeof(                               \
                _Generic((x),                                           \
                         char:  (char)0,                                \
                         __scalar_type_to_expr_cases(char),             \
                         __scalar_type_to_expr_cases(short),            \
                         __scalar_type_to_expr_cases(int),              \
                         __scalar_type_to_expr_cases(long),             \
                         __scalar_type_to_expr_cases(long long),        \
                         default: (x)))

/* Is this type a native word size -- useful for atomic operations */
#define __native_word(t) \
        (sizeof(t) == sizeof(char) || sizeof(t) == sizeof(short) || \
         sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))


#define typeof_member(T, m)    typeof(((T*)0)->m)

/**
 * @brief cast a member of a structure out to the containing structure
 *
 * @param ptr: the pointer to the member.
 * @param type: the type of the container struct this is embedded in.
 * @param member: the name of the member within the struct.
 *
 * @warning: any const qualifier of @ptr is lost.
 */
#define container_of(ptr, type, member) ({                \
    void *__mptr = (void *)(ptr);                    \
    static_assert(__same_type(*(ptr), ((type *)0)->member) ||    \
              __same_type(*(ptr), void),            \
              "pointer type mismatch in container_of()");    \
    ((type *)(__mptr - offsetof(type, member))); })

/**
 * @brief cast a member of a structure out to the containing structure and
 * preserve the const-ness of the pointer
 *
 * @param ptr: the pointer to the member
 * @param type: the type of the container struct this is embedded in.
 * @param member: the name of the member within the struct.
 */
#define container_of_const(ptr, type, member) \
    _Generic(ptr, \
        const typeof(*(ptr)) *: ((const type *)container_of(ptr, type, member)), \
        default: ((type *)container_of(ptr, type, member)) \
    )

/* [] END OF FILE */