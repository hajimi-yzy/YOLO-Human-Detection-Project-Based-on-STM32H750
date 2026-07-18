#ifndef NANOV4_POSTPROCESS_H
#define NANOV4_POSTPROCESS_H

#include <stdint.h>

#define NANOV4_MAX_DETECTIONS 5U
#define NANOV4_OUTPUT_ELEMENTS (24U * 24U * 5U)

typedef struct
{
  int16_t x1;
  int16_t y1;
  int16_t x2;
  int16_t y2;
  uint16_t score_x100;
} NanoV4_Box;

/*
 * Uses module-owned scratch storage and is not reentrant. The output buffer
 * must provide at least NANOV4_OUTPUT_ELEMENTS readable int8_t elements.
 */
uint32_t NanoV4_Decode(const int8_t *output,
                       NanoV4_Box *boxes,
                       uint32_t capacity,
                       float score_threshold,
                       float nms_iou);

/*
 * Calls NanoV4_Decode and shares its non-reentrant module workspace. It
 * destructively overwrites workspace[0..NANOV4_OUTPUT_ELEMENTS-1] and is intended
 * before first inference. Callers must not use the synthetic tensor after any return.
 */
int NanoV4_SelfTest(int8_t *workspace, uint32_t element_count);

#endif
