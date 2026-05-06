#ifndef CRT_H
#define CRT_H

#include "system.h"

/* -------------------------------------------------------------------------
   1. FUNCTION PROTOTYPES (Global Symbols)
   ------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

/* Standard CRT symbols (Required by Compiler) */
void *memset(void* dst_ptr, int byte_value, size_t count);
void *memcpy(void* dst_ptr, const void* src_ptr, size_t count);
void *memmove(void* dst_ptr, const void* src_ptr, size_t count);
int   memcmp(const void* buffer_a, const void* buffer_b, size_t count);
size_t strlen(const char* string_ptr);

void *crt_memset(void *dst_ptr, int byte_value, size_t count);

#ifdef _MSC_VER
    long long __alldiv(long long value_x, long long value_y);
    unsigned long long __aulldiv(unsigned long long value_x, unsigned long long value_y);
#endif

#ifdef __cplusplus
}
#endif

/* -------------------------------------------------------------------------
   2. INLINED UTILITIES (Prefixed)
   ------------------------------------------------------------------------- */

/** @brief Custom inlined string length returning signed int */
SYSTEM_INLINE int crt_str_len(const char* string_ptr) { 
    const char* cursor = string_ptr; 
    if (!cursor) return 0;
    while (*cursor) { ++cursor; }
    return (int)(cursor - string_ptr); 
}

SYSTEM_INLINE char* crt_u32_to_str_rev(char* buffer_end, uint32_t value) {
    char* cursor = buffer_end;
    *--cursor = '\0';
    if (value == 0) { *--cursor = '0'; return cursor; }
    while (value) { 
        *--cursor = (char)('0' + (value % 10)); 
        value /= 10; 
    }
    return cursor;
}

/* -------------------------------------------------------------------------
   3. IMPLEMENTATION (Define CRT_IMPLEMENTATION in ONE .c file)
   ------------------------------------------------------------------------- */
#ifdef CRT_IMPLEMENTATION

/* Fills 'count' bytes of 'dst_ptr' with 'byte_value'. Returns 'dst_ptr'. */
void *memset(void *dst_ptr, int byte_value, size_t count) {
    unsigned char *cursor = (unsigned char*)dst_ptr;
    unsigned char value = (unsigned char)byte_value;
    while (count--) { *cursor++ = value; }
    return dst_ptr;
}

/* Copies 'count' bytes from 'src_ptr' to 'dst_ptr'. Buffers MUST NOT overlap. */
void *memcpy(void *dst_ptr, const void *src_ptr, size_t count) {
    unsigned char *dst_cursor = (unsigned char*)dst_ptr;
    const unsigned char *src_cursor = (const unsigned char*)src_ptr;
    while (count--) { *dst_cursor++ = *src_cursor++; }
    return dst_ptr;
}

/* Copies 'count' bytes from 'src_ptr' to 'dst_ptr'. Safely handles overlapping regions. */
void *memmove(void *dst_ptr, const void *src_ptr, size_t count) {
    unsigned char *d = (unsigned char*)dst_ptr;
    const unsigned char *s = (const unsigned char*)src_ptr;
    /* Determine if we need to copy forward or backward based on memory addresses */
    if (d < s) {
        while (count--) { *d++ = *s++; }
    } else {
        d += count; s += count;
        while (count--) { *--d = *--s; }
    }
    return dst_ptr;
}

/* Compares 'count' bytes of two buffers. Returns 0 if equal. */
int memcmp(const void *buffer_a, const void *buffer_b, size_t count) {
    const unsigned char *a = (const unsigned char*)buffer_a;
    const unsigned char *b = (const unsigned char*)buffer_b;
    while (count--) {
        if (*a != *b) return (int)*a - (int)*b;
        a++; b++;
    }
    return 0;
}

/* Measures string length by scanning for the null (\0) terminator. */
size_t strlen(const char *string_ptr) {
    const char *cursor = string_ptr;
    while (*cursor) { ++cursor; }
    return (size_t)(cursor - string_ptr);
}

/**
 * @brief Fills a memory block using 32-bit words for increased performance.
 * @name mem_fill
 */
void *crt_memset(void *dst_ptr, int byte_value, size_t count) {
    unsigned char *cursor = (unsigned char*)dst_ptr;
    uint32_t value32;
    uint8_t  v = (uint8_t)byte_value;

    /* Create a 32-bit pattern: 0xVVVVVVVV */
    value32 = (uint32_t)v | ((uint32_t)v << 8) | ((uint32_t)v << 16) | ((uint32_t)v << 24);

    /* 1. Handle unaligned start (Fill byte-by-byte until 4-byte aligned) */
    while (count > 0 && ((uintptr_t)cursor & 3) != 0) {
        *cursor++ = v;
        count--;
    }

    /* 2. Fill the bulk of memory 4 bytes at a time */
    {
        uint32_t *d32 = (uint32_t*)cursor;
        size_t words = count >> 2; /* Divide by 4 */
        while (words--) {
            *d32++ = value32;
        }
        cursor = (unsigned char*)d32;
        count &= 3; /* Remainder: count % 4 */
    }

    /* 3. Handle trailing bytes */
    while (count--) {
        *cursor++ = v;
    }

    return dst_ptr;
}

#ifdef _MSC_VER
    long long __alldiv(long long x, long long y) { return x / y; }
    unsigned long long __aulldiv(unsigned long long x, unsigned long long y) { return x / y; }
#endif

#endif /* CRT_IMPLEMENTATION */
#endif /* CRT_H */