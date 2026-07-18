#include "nanov4_postprocess.h"

#include <math.h>

#define NANOV4_GRID_WIDTH (24U)
#define NANOV4_GRID_HEIGHT (24U)
#define NANOV4_GRID_CELLS (NANOV4_GRID_WIDTH * NANOV4_GRID_HEIGHT)
#define NANOV4_CHANNELS (5U)
#define NANOV4_STRIDE (4.0f)
#define NANOV4_OUTPUT_SCALE (0.071815751f)
#define NANOV4_OUTPUT_ZERO (-59)
#define NANOV4_MAX_CANDIDATES (32U)
#define NANOV4_IMAGE_MAX (95.0f)

typedef struct
{
  float x1;
  float y1;
  float x2;
  float y2;
  float score;
  uint32_t cell_index;
} NanoV4_Candidate;

typedef struct
{
  NanoV4_Candidate candidates[NANOV4_MAX_CANDIDATES];
  uint8_t visited[NANOV4_GRID_CELLS];
  uint8_t maxima[NANOV4_GRID_CELLS];
  uint16_t queue[NANOV4_GRID_CELLS];
  NanoV4_Box selftest_boxes[NANOV4_MAX_DETECTIONS];
  uint16_t reserved;
} NanoV4_Workspace;

/* NanoV4_Decode and NanoV4_SelfTest synchronously own this non-reentrant workspace. */
__align(32) static NanoV4_Workspace s_nanov4_workspace;

static float NanoV4_Dequantize(int32_t raw)
{
  return (raw - NANOV4_OUTPUT_ZERO) * NANOV4_OUTPUT_SCALE;
}

static float NanoV4_Sigmoid(float value)
{
  return 1.0f / (1.0f + expf(-value));
}

static float NanoV4_Clamp(float value, float lower, float upper)
{
  if (value < lower)
  {
    return lower;
  }
  if (value > upper)
  {
    return upper;
  }
  return value;
}

static void NanoV4_LocalMaximum(const int8_t *output)
{
  uint8_t *visited = s_nanov4_workspace.visited;
  uint8_t *maxima = s_nanov4_workspace.maxima;
  uint16_t *queue = s_nanov4_workspace.queue;
  uint32_t start;

  for (start = 0U; start < NANOV4_GRID_CELLS; ++start)
  {
    visited[start] = 0U;
    maxima[start] = 0U;
  }

  for (start = 0U; start < NANOV4_GRID_CELLS; ++start)
  {
    uint32_t head;
    uint32_t tail;
    uint32_t lowest;
    int32_t quality_raw;
    int is_maximum;

    if (visited[start] != 0U)
    {
      continue;
    }

    head = 0U;
    tail = 0U;
    lowest = start;
    quality_raw = output[start * NANOV4_CHANNELS];
    is_maximum = 1;
    queue[tail] = (uint16_t)start;
    ++tail;
    visited[start] = 1U;

    while (head < tail)
    {
      uint32_t cell = queue[head];
      uint32_t x = cell % NANOV4_GRID_WIDTH;
      uint32_t y = cell / NANOV4_GRID_WIDTH;
      int32_t ny;
      ++head;

      for (ny = (int32_t)y - 1; ny <= (int32_t)y + 1; ++ny)
      {
        int32_t nx;
        if ((ny < 0) || (ny >= (int32_t)NANOV4_GRID_HEIGHT))
        {
          continue;
        }
        for (nx = (int32_t)x - 1; nx <= (int32_t)x + 1; ++nx)
        {
          uint32_t neighbor;
          int32_t neighbor_raw;
          if ((nx < 0) || (nx >= (int32_t)NANOV4_GRID_WIDTH))
          {
            continue;
          }

          neighbor = (uint32_t)ny * NANOV4_GRID_WIDTH + (uint32_t)nx;
          neighbor_raw = output[neighbor * NANOV4_CHANNELS];
          if (neighbor_raw > quality_raw)
          {
            is_maximum = 0;
          }
          if ((neighbor_raw == quality_raw) && (visited[neighbor] == 0U))
          {
            visited[neighbor] = 1U;
            queue[tail] = (uint16_t)neighbor;
            ++tail;
            if (neighbor < lowest)
            {
              lowest = neighbor;
            }
          }
        }
      }
    }

    if (is_maximum != 0)
    {
      maxima[lowest] = 1U;
    }
  }
}

static void NanoV4_InsertCandidate(NanoV4_Candidate *candidates,
                                   uint32_t *count,
                                   const NanoV4_Candidate *candidate)
{
  uint32_t position = 0U;
  uint32_t last;
  uint32_t i;

  while (position < *count)
  {
    if ((candidate->score > candidates[position].score) ||
        ((candidate->score == candidates[position].score) &&
         (candidate->cell_index < candidates[position].cell_index)))
    {
      break;
    }
    ++position;
  }

  if ((*count == NANOV4_MAX_CANDIDATES) &&
      (position == NANOV4_MAX_CANDIDATES))
  {
    return;
  }

  if (*count < NANOV4_MAX_CANDIDATES)
  {
    last = *count;
    ++(*count);
  }
  else
  {
    last = NANOV4_MAX_CANDIDATES - 1U;
  }

  for (i = last; i > position; --i)
  {
    candidates[i] = candidates[i - 1U];
  }
  candidates[position] = *candidate;
}

static float NanoV4_Iou(const NanoV4_Candidate *a,
                        const NanoV4_Candidate *b)
{
  float ix1 = (a->x1 > b->x1) ? a->x1 : b->x1;
  float iy1 = (a->y1 > b->y1) ? a->y1 : b->y1;
  float ix2 = (a->x2 < b->x2) ? a->x2 : b->x2;
  float iy2 = (a->y2 < b->y2) ? a->y2 : b->y2;
  float iw = ix2 - ix1;
  float ih = iy2 - iy1;
  float intersection;
  float area_a;
  float area_b;

  if ((iw <= 0.0f) || (ih <= 0.0f))
  {
    return 0.0f;
  }
  intersection = iw * ih;
  area_a = (a->x2 - a->x1) * (a->y2 - a->y1);
  area_b = (b->x2 - b->x1) * (b->y2 - b->y1);
  return intersection / (area_a + area_b - intersection);
}

static uint32_t NanoV4_Nms(const NanoV4_Candidate *candidates,
                           uint32_t candidate_count,
                           NanoV4_Box *boxes,
                           uint32_t limit,
                           float nms_iou)
{
  uint32_t selected[NANOV4_MAX_DETECTIONS];
  uint32_t selected_count = 0U;
  uint32_t i;

  for (i = 0U; (i < candidate_count) && (selected_count < limit); ++i)
  {
    uint32_t j;
    int keep = 1;
    for (j = 0U; j < selected_count; ++j)
    {
      if (NanoV4_Iou(&candidates[i], &candidates[selected[j]]) > nms_iou)
      {
        keep = 0;
        break;
      }
    }

    if (keep != 0)
    {
      float rounded_score = candidates[i].score * 100.0f + 0.5f;
      selected[selected_count] = i;
      boxes[selected_count].x1 = (int16_t)(candidates[i].x1 + 0.5f);
      boxes[selected_count].y1 = (int16_t)(candidates[i].y1 + 0.5f);
      boxes[selected_count].x2 = (int16_t)(candidates[i].x2 + 0.5f);
      boxes[selected_count].y2 = (int16_t)(candidates[i].y2 + 0.5f);
      rounded_score = NanoV4_Clamp(rounded_score, 0.0f, 100.0f);
      boxes[selected_count].score_x100 = (uint16_t)rounded_score;
      ++selected_count;
    }
  }
  return selected_count;
}

uint32_t NanoV4_Decode(const int8_t *output,
                       NanoV4_Box *boxes,
                       uint32_t capacity,
                       float score_threshold,
                       float nms_iou)
{
  NanoV4_Candidate *candidates = s_nanov4_workspace.candidates;
  uint8_t *maxima = s_nanov4_workspace.maxima;
  uint32_t candidate_count = 0U;
  uint32_t limit;
  uint32_t y;

  if ((output == 0) || (boxes == 0) || (capacity == 0U) ||
      (score_threshold != score_threshold) || (nms_iou != nms_iou) ||
      (score_threshold < 0.0f) || (score_threshold > 1.0f) ||
      (nms_iou < 0.0f) || (nms_iou > 1.0f))
  {
    return 0U;
  }

  NanoV4_LocalMaximum(output);

  for (y = 0U; y < NANOV4_GRID_HEIGHT; ++y)
  {
    uint32_t x;
    for (x = 0U; x < NANOV4_GRID_WIDTH; ++x)
    {
      uint32_t base = ((y * NANOV4_GRID_WIDTH + x) * NANOV4_CHANNELS);
      uint32_t cell_index = y * NANOV4_GRID_WIDTH + x;
      float quality = NanoV4_Dequantize(output[base + 0U]);
      float score = NanoV4_Sigmoid(quality);
      float left;
      float top;
      float right;
      float bottom;
      float center_x;
      float center_y;
      NanoV4_Candidate candidate;

      if ((score < score_threshold) || (maxima[cell_index] == 0U))
      {
        continue;
      }

      left = NanoV4_Dequantize(output[base + 1U]);
      top = NanoV4_Dequantize(output[base + 2U]);
      right = NanoV4_Dequantize(output[base + 3U]);
      bottom = NanoV4_Dequantize(output[base + 4U]);
      left = NanoV4_Clamp(left, -2.0f, 24.0f);
      top = NanoV4_Clamp(top, -2.0f, 24.0f);
      right = NanoV4_Clamp(right, -2.0f, 24.0f);
      bottom = NanoV4_Clamp(bottom, -2.0f, 24.0f);
      center_x = ((float)x + 0.5f) * NANOV4_STRIDE;
      center_y = ((float)y + 0.5f) * NANOV4_STRIDE;

      candidate.x1 = NanoV4_Clamp(center_x - left * NANOV4_STRIDE,
                                  0.0f, NANOV4_IMAGE_MAX);
      candidate.y1 = NanoV4_Clamp(center_y - top * NANOV4_STRIDE,
                                  0.0f, NANOV4_IMAGE_MAX);
      candidate.x2 = NanoV4_Clamp(center_x + right * NANOV4_STRIDE,
                                  0.0f, NANOV4_IMAGE_MAX);
      candidate.y2 = NanoV4_Clamp(center_y + bottom * NANOV4_STRIDE,
                                  0.0f, NANOV4_IMAGE_MAX);
      candidate.score = score;
      candidate.cell_index = cell_index;

      if (((candidate.x2 - candidate.x1) <= 1.0f) ||
          ((candidate.y2 - candidate.y1) <= 1.0f))
      {
        continue;
      }
      NanoV4_InsertCandidate(candidates, &candidate_count, &candidate);
    }
  }

  limit = (capacity < NANOV4_MAX_DETECTIONS) ? capacity : NANOV4_MAX_DETECTIONS;
  return NanoV4_Nms(candidates, candidate_count, boxes, limit, nms_iou);
}

int NanoV4_SelfTest(int8_t *workspace, uint32_t element_count)
{
  NanoV4_Box *selftest_boxes = s_nanov4_workspace.selftest_boxes;
  uint32_t capacity_count;
  uint32_t nms_count;
  uint32_t index;
  uint32_t cell;

  if ((workspace == 0) || (element_count < NANOV4_OUTPUT_ELEMENTS))
  {
    return 0;
  }

  for (index = 0U; index < NANOV4_OUTPUT_ELEMENTS; ++index)
  {
    workspace[index] = -128;
  }
  for (cell = 0U; cell < NANOV4_GRID_CELLS; ++cell)
  {
    uint32_t base = cell * NANOV4_CHANNELS;
    workspace[base + 1U] = -45;
    workspace[base + 2U] = -45;
    workspace[base + 3U] = -45;
    workspace[base + 4U] = -45;
  }

  workspace[(0U * NANOV4_GRID_WIDTH + 0U) * NANOV4_CHANNELS] = 10;
  workspace[(1U * NANOV4_GRID_WIDTH + 0U) * NANOV4_CHANNELS] = 10;
  workspace[(2U * NANOV4_GRID_WIDTH + 1U) * NANOV4_CHANNELS] = 10;
  workspace[(1U * NANOV4_GRID_WIDTH + 2U) * NANOV4_CHANNELS] = 10;
  workspace[(0U * NANOV4_GRID_WIDTH + 2U) * NANOV4_CHANNELS] = 10;
  workspace[1U] = 127;
  workspace[2U] = 127;
  workspace[3U] = 127;
  workspace[4U] = 127;

  nms_count = NanoV4_Decode(workspace, selftest_boxes,
                            NANOV4_MAX_DETECTIONS, 0.50f, 0.50f);
  if ((nms_count != 1U) ||
      (selftest_boxes[0U].x1 != 0) || (selftest_boxes[0U].y1 != 0) ||
      (selftest_boxes[0U].x2 != 55) || (selftest_boxes[0U].y2 != 55) ||
      (selftest_boxes[0U].score_x100 != 99U))
  {
    return 0;
  }

  for (index = 0U; index < NANOV4_OUTPUT_ELEMENTS; ++index)
  {
    workspace[index] = -128;
  }
  for (cell = 0U; cell < NANOV4_GRID_CELLS; ++cell)
  {
    uint32_t base = cell * NANOV4_CHANNELS;
    workspace[base + 1U] = -45;
    workspace[base + 2U] = -45;
    workspace[base + 3U] = -45;
    workspace[base + 4U] = -45;
  }

  cell = 5U * NANOV4_GRID_WIDTH + 5U;
  workspace[cell * NANOV4_CHANNELS + 0U] = 10;
  workspace[cell * NANOV4_CHANNELS + 1U] = 0;
  workspace[cell * NANOV4_CHANNELS + 2U] = 0;
  workspace[cell * NANOV4_CHANNELS + 3U] = 0;
  workspace[cell * NANOV4_CHANNELS + 4U] = 0;
  cell = 5U * NANOV4_GRID_WIDTH + 7U;
  workspace[cell * NANOV4_CHANNELS + 0U] = 9;
  workspace[cell * NANOV4_CHANNELS + 1U] = 0;
  workspace[cell * NANOV4_CHANNELS + 2U] = 0;
  workspace[cell * NANOV4_CHANNELS + 3U] = 0;
  workspace[cell * NANOV4_CHANNELS + 4U] = 0;
  cell = 20U * NANOV4_GRID_WIDTH + 20U;
  workspace[cell * NANOV4_CHANNELS + 0U] = 8;

  capacity_count = NanoV4_Decode(workspace, selftest_boxes, 1U, 0.50f, 1.0f);
  if ((capacity_count != 1U) ||
      (selftest_boxes[0U].x1 != 5) || (selftest_boxes[0U].y1 != 5) ||
      (selftest_boxes[0U].x2 != 39) || (selftest_boxes[0U].y2 != 39) ||
      (selftest_boxes[0U].score_x100 != 99U))
  {
    return 0;
  }

  nms_count = NanoV4_Decode(workspace, selftest_boxes,
                            NANOV4_MAX_DETECTIONS, 0.50f, 0.50f);
  if ((nms_count != 2U) ||
      (selftest_boxes[0U].x1 != 5) || (selftest_boxes[0U].y1 != 5) ||
      (selftest_boxes[0U].x2 != 39) || (selftest_boxes[0U].y2 != 39) ||
      (selftest_boxes[0U].score_x100 != 99U) ||
      (selftest_boxes[1U].x1 != 78) || (selftest_boxes[1U].y1 != 78) ||
      (selftest_boxes[1U].x2 != 86) || (selftest_boxes[1U].y2 != 86) ||
      (selftest_boxes[1U].score_x100 != 99U))
  {
    return 0;
  }

  return 1;
}
