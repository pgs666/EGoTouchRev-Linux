#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define U8_MAX UINT8_MAX
#define U16_MAX UINT16_MAX
#define SZ_64K 65536
#define __maybe_unused __attribute__((unused))
#define CONFIG_DYNAMIC_DEBUG 0
#define IS_ENABLED(option) 0
#define pr_warn(...) ((void)0)
#define pr_info(...) ((void)0)

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min_t(type, a, b) ((type)min((type)(a), (type)(b)))
#define max_t(type, a, b) ((type)max((type)(a), (type)(b)))
#define clamp_t(type, value, low, high) \
	((type)min((type)(high), max((type)(low), (type)(value))))
#define clamp_val(value, low, high) min((high), max((low), (value)))
#define le16_to_cpup(ptr) (*(ptr))
#define swap(a, b) do { __typeof__(a) tmp = (a); (a) = (b); (b) = tmp; } while (0)
