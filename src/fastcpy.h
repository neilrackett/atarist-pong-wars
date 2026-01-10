#ifndef FASTCPY_H
#define FASTCPY_H

#include <stddef.h>

/* Optimized copy using Blitter if available, falling back to memcpy */
void *fastcpy(void *dst, const void *src, size_t n);

#endif