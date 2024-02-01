/*
 * Adapted from: QOI - The "Quite OK Image" format for fast, lossless image
 *   compression; see https://phoboslab.org
 *
 * Copyright (c) 2021, Dominic Szablewski - https://phoboslab.org
 * SPDX-License-Identifier: MIT
 */

#ifndef _QOI_H
#define _QOI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
  extern "C" {
#endif

#define QOI_SRGB   0	/* sRGB (gamma corrected RGB & linear alpha channel) */
#define QOI_LINEAR 1	/* all channels linear */

typedef struct {
  /* file starts with the 4-byte identifier "qoif" */
  uint32_t width;   	/* stored in Big Endian in the file */
  uint32_t height;  	/* stored in Big Endian in the file */
  uint8_t channels; 	/* 3 = RGB, 4 = RGBA */
  uint8_t colorspace;	/* QOI_SRGB or QOI_LINEAR */
} QOI_DESC;

void *qoi_encode(const void *data, const QOI_DESC *desc, size_t *out_len);
void *qoi_decode(const void *data, size_t size, QOI_DESC *desc, int channels);
void *qoi_free(void *data);

#ifdef __cplusplus
}
#endif
#endif /* _QOI_H */

