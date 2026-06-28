/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <osbind.h>
#include <string.h>
#include <stddef.h>
#include "fastcpy.h"

/* --- Blitter -------------------------------------------------------- */

static int has_blitter = -1;

static int compute_has_blitter(void)
{
  long mode = Blitmode(-1); /* XBIOS #64 */
  return (mode & 1) != 0;   /* bit 0 = blitter hardware present */
}

/* Blitter can only access ST-RAM (below 16MB) */
#define IS_ST_RAM(p) ((unsigned long)(p) < 0x01000000UL)

void *fastcpy(void *dst, const void *src, size_t n)
{
  if (has_blitter == -1)
  {
    has_blitter = compute_has_blitter();
  }

  // Use blitter if available, word-aligned, and buffers are in ST-RAM
  if (n >= 256 && has_blitter && ((((long)src | (long)dst | n) & 1) == 0) &&
      IS_ST_RAM(src) && IS_ST_RAM(dst))
  {
    const size_t CHUNK_SIZE = 4096; /* Process in chunks to allow interrupts */
    size_t remaining = n;
    unsigned char *s = (unsigned char *)src;
    unsigned char *d = (unsigned char *)dst;

    while (remaining > 0)
    {
      size_t chunk = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
      short old_sr;

      /* Disable interrupts to prevent interference during Blitter setup */
      __asm__ volatile(
          "move.w %%sr, %0\n\t"
          "ori.w #0x0700, %%sr"
          : "=d"(old_sr)
          :
          : "cc");

      // Set X/Y increments to 2 (bytes) for consecutive words
      *(volatile short *)0xFF8A20 = 2; // Src X Inc
      *(volatile short *)0xFF8A22 = 2; // Src Y Inc
      *(volatile short *)0xFF8A2E = 2; // Dst X Inc
      *(volatile short *)0xFF8A30 = 2; // Dst Y Inc

      // Set Endmasks to 0xFFFF (no masking)
      *(volatile short *)0xFF8A28 = 0xFFFF;
      *(volatile short *)0xFF8A2A = 0xFFFF;
      *(volatile short *)0xFF8A2C = 0xFFFF;

      // Set Source and Destination addresses
      *(volatile long *)0xFF8A24 = (long)s; // ADR_A
      *(volatile long *)0xFF8A32 = (long)d; // ADR_D

      // Set X Count (words per line) and Y Count (lines)
      *(volatile short *)0xFF8A36 = (short)(chunk >> 1);
      *(volatile short *)0xFF8A38 = 1;

      // Set HOP (Source) and OP (Source)
      *(volatile char *)0xFF8A3A = 2; // HOP: Source
      *(volatile char *)0xFF8A3B = 3; // OP: Source

      *(volatile char *)0xFF8A3C = 0xC0; // start
      while (*(volatile char *)0xFF8A3C & 0x80)
        ;

      /* Restore interrupts */
      __asm__ volatile(
          "move.w %0, %%sr"
          :
          : "d"(old_sr)
          : "cc");

      remaining -= chunk;
      s += chunk;
      d += chunk;
    }

    return dst;
  }

  return memcpy(dst, src, n); // fallback
}