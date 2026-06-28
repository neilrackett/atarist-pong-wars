/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FASTCPY_H
#define FASTCPY_H

#include <stddef.h>

/* Optimized copy using Blitter if available, falling back to memcpy */
void *fastcpy(void *dst, const void *src, size_t n);

#endif