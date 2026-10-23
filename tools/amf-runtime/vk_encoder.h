/*
 * Copyright 2026 WinBoat contributors
 * SPDX-License-Identifier: MIT
 */

/* The AMF runtime's encoder: H.264/HEVC on Vulkan Video (vk_encoder.c) */

#pragma once
#include <stddef.h>

enum { VK_ENCODER_RC_CQP, VK_ENCODER_RC_CBR, VK_ENCODER_RC_VBR };

typedef struct {
   int hevc, tenBit;
   int width, height;
   /* AMF's values: H.264 profile_idc (66/77/100...), HEVC 1 main / 2 main 10; levels as the codecs number them
    * (H.264 10*level, HEVC 30*level; 0: the encoder's maximum) */
   int profile, level, highTier, cabac;
   int idrPeriod;
   int rc, qpI, qpP;
   long long bitrate, peak, vbv;
   int fpsNum, fpsDen;
} vk_encoder_config;

/* a picture: its planes in host memory (NV12/P010: Y and interleaved UV; YUV420P: Y, U, V) */
typedef struct {
   int width, height;
   const unsigned char *planes[3];
   int pitches[3];
   int planeCount;
   int forceIdr;
} vk_encoder_picture;

typedef struct vk_encoder vk_encoder;

int vk_encoder_supported(int hevc);
vk_encoder *vk_encoder_create(const vk_encoder_config *config);
void vk_encoder_destroy(vk_encoder *e);
/* the parameter sets (Annex B), valid while the encoder lives */
const unsigned char *vk_encoder_headers(vk_encoder *e, size_t *size);
/* encodes a picture: its bitstream (Annex B, parameter sets in front of an IDR), valid until the next call */
const unsigned char *vk_encoder_encode(vk_encoder *e, const vk_encoder_picture *pic, size_t *size, int *idr);
