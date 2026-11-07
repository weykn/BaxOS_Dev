#pragma once

#include <stdint.h>

/* The arguments of kprintf, ksprintf and dbg, every one made a uint64_t on
 * the way in, which is how the formatters read every one of them: %s's
 * pointer, %x's number, %u's (cut to 32 bits, as an unsigned). On x86-64 a
 * narrower argument reads the same anyway, from a register; on i386 it
 * takes four bytes of the stack where eight are read, and every argument
 * after it would be read askew. FMT_ARGS(a, b, c) is "(uint64_t)(a),
 * (uint64_t)(b), (uint64_t)(c)", for up to 24. */

#define FMT_U(x) ((uint64_t)(x))   /* a pointer is printed as its bits on either */
#define FMT_N(...) FMT_N_(__VA_ARGS__, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, \
                          12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define FMT_N_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, \
               _18, _19, _20, _21, _22, _23, _24, n, ...) n
#define FMT_CAT(a, b) FMT_CAT_(a, b)
#define FMT_CAT_(a, b) a##b
#define FMT_ARGS(...) FMT_CAT(FMT_A, FMT_N(__VA_ARGS__))(__VA_ARGS__)
#define FMT_A1(a) FMT_U(a)
#define FMT_A2(a, ...) FMT_U(a), FMT_A1(__VA_ARGS__)
#define FMT_A3(a, ...) FMT_U(a), FMT_A2(__VA_ARGS__)
#define FMT_A4(a, ...) FMT_U(a), FMT_A3(__VA_ARGS__)
#define FMT_A5(a, ...) FMT_U(a), FMT_A4(__VA_ARGS__)
#define FMT_A6(a, ...) FMT_U(a), FMT_A5(__VA_ARGS__)
#define FMT_A7(a, ...) FMT_U(a), FMT_A6(__VA_ARGS__)
#define FMT_A8(a, ...) FMT_U(a), FMT_A7(__VA_ARGS__)
#define FMT_A9(a, ...) FMT_U(a), FMT_A8(__VA_ARGS__)
#define FMT_A10(a, ...) FMT_U(a), FMT_A9(__VA_ARGS__)
#define FMT_A11(a, ...) FMT_U(a), FMT_A10(__VA_ARGS__)
#define FMT_A12(a, ...) FMT_U(a), FMT_A11(__VA_ARGS__)
#define FMT_A13(a, ...) FMT_U(a), FMT_A12(__VA_ARGS__)
#define FMT_A14(a, ...) FMT_U(a), FMT_A13(__VA_ARGS__)
#define FMT_A15(a, ...) FMT_U(a), FMT_A14(__VA_ARGS__)
#define FMT_A16(a, ...) FMT_U(a), FMT_A15(__VA_ARGS__)
#define FMT_A17(a, ...) FMT_U(a), FMT_A16(__VA_ARGS__)
#define FMT_A18(a, ...) FMT_U(a), FMT_A17(__VA_ARGS__)
#define FMT_A19(a, ...) FMT_U(a), FMT_A18(__VA_ARGS__)
#define FMT_A20(a, ...) FMT_U(a), FMT_A19(__VA_ARGS__)
#define FMT_A21(a, ...) FMT_U(a), FMT_A20(__VA_ARGS__)
#define FMT_A22(a, ...) FMT_U(a), FMT_A21(__VA_ARGS__)
#define FMT_A23(a, ...) FMT_U(a), FMT_A22(__VA_ARGS__)
#define FMT_A24(a, ...) FMT_U(a), FMT_A23(__VA_ARGS__)
