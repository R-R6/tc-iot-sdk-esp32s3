// Copyright (c) 2026 Tencent. All rights reserved.

#include "video_source.h"

#include <stdlib.h>
#include <string.h>

/* 引入编译期内嵌的 H264 视频数据 */
#include "video_size160x90_gop50_fps25.h"

/*==============================================================================
 * 视频帧索引（从 video_size160x90_gop50_fps25_index.txt 硬编码）
 * 格式：seq, type('I'=key/'P'=inter), offset, pts_ms, size
 *============================================================================*/
typedef struct {
  uint32_t offset;
  uint32_t size;
  uint32_t pts_ms;
  bool is_key;
} video_frame_index_t;

static const video_frame_index_t kFrameIndex[] = {
    {0, 3261, 0, true},         {3261, 416, 40, false},     {3677, 754, 80, false},    {4431, 878, 120, false},
    {5309, 668, 160, false},    {5977, 989, 200, false},    {6966, 665, 240, false},   {7631, 844, 280, false},
    {8475, 566, 320, false},    {9041, 732, 360, false},    {9773, 285, 400, false},   {10058, 431, 440, false},
    {10489, 673, 480, false},   {11162, 537, 520, false},   {11699, 499, 560, false},  {12198, 759, 600, false},
    {12957, 593, 640, false},   {13550, 537, 680, false},   {14087, 1082, 720, false}, {15169, 544, 760, false},
    {15713, 622, 800, false},   {16335, 774, 840, false},   {17109, 489, 880, false},  {17598, 387, 920, false},
    {17985, 560, 960, false},   {18545, 531, 1000, false},  {19076, 669, 1040, false}, {19745, 538, 1080, false},
    {20283, 714, 1120, false},  {20997, 569, 1160, false},  {21566, 831, 1200, false}, {22397, 830, 1240, false},
    {23227, 597, 1280, false},  {23824, 492, 1320, false},  {24316, 643, 1360, false}, {24959, 443, 1400, false},
    {25402, 508, 1440, false},  {25910, 738, 1480, false},  {26648, 377, 1520, false}, {27025, 387, 1560, false},
    {27412, 1781, 1600, false}, {29193, 1107, 1640, false}, {30300, 800, 1680, false}, {31100, 822, 1720, false},
    {31922, 999, 1760, false},  {32921, 607, 1800, false},  {33528, 282, 1840, false}, {33810, 332, 1880, false},
    {34142, 278, 1920, false},  {34420, 272, 1960, false},  {34692, 2783, 2000, true}, {37475, 454, 2040, false},
    {37929, 619, 2080, false},  {38548, 1155, 2120, false}, {39703, 663, 2160, false}, {40366, 750, 2200, false},
    {41116, 1416, 2240, false}, {42532, 79, 2280, false},   {42611, 67, 2320, false},  {42678, 63, 2360, false},
    {42741, 92, 2400, false},   {42833, 335, 2440, false},  {43168, 402, 2480, false}, {43570, 451, 2520, false},
    {44021, 648, 2560, false},  {44669, 276, 2600, false},  {44945, 413, 2640, false}, {45358, 694, 2680, false},
    {46052, 1062, 2720, false}, {47114, 822, 2760, false},  {47936, 980, 2800, false}, {48916, 668, 2840, false},
    {49584, 632, 2880, false},  {50216, 376, 2920, false},  {50592, 265, 2960, false}, {50857, 324, 3000, false},
    {51181, 529, 3040, false},  {51710, 496, 3080, false},  {52206, 524, 3120, false}, {52730, 561, 3160, false},
    {53291, 785, 3200, false},  {54076, 558, 3240, false},  {54634, 678, 3280, false}, {55312, 871, 3320, false},
    {56183, 543, 3360, false},  {56726, 555, 3400, false},  {57281, 635, 3440, false}, {57916, 254, 3480, false},
    {58170, 233, 3520, false},  {58403, 503, 3560, false},  {58906, 527, 3600, false}, {59433, 744, 3640, false},
    {60177, 609, 3680, false},  {60786, 2286, 3720, false}, {63072, 402, 3760, false}, {63474, 262, 3800, false},
    {63736, 270, 3840, false},  {64006, 337, 3880, false},  {64343, 419, 3920, false}, {64762, 420, 3960, false},
};

static const int kFrameCount = sizeof(kFrameIndex) / sizeof(kFrameIndex[0]);

/* 最后一帧的 pts + 帧间隔 = 一轮总时长 */
static const uint64_t kLoopDurationMs = 4000;

struct video_source_t {
  int current_frame;
  uint64_t pts_offset; /* 循环时累加的时间偏移 */
};

video_source_t *video_source_create(void) {
  video_source_t *src = (video_source_t *)calloc(1, sizeof(video_source_t));
  if (!src) {
    return NULL;
  }
  src->current_frame = 0;
  src->pts_offset = 0;
  return src;
}

void video_source_destroy(video_source_t *src) { free(src); }

int video_source_get_frame(video_source_t *src, const uint8_t **data, uint32_t *size, bool *is_key, uint64_t *pts_ms) {
  if (!src || !data || !size || !is_key || !pts_ms) {
    return -1;
  }

  const video_frame_index_t *idx = &kFrameIndex[src->current_frame];
  *data = video_data + idx->offset;
  *size = idx->size;
  *is_key = idx->is_key;
  *pts_ms = idx->pts_ms + src->pts_offset;

  src->current_frame++;
  if (src->current_frame >= kFrameCount) {
    /* 循环播放 */
    src->current_frame = 0;
    src->pts_offset += kLoopDurationMs;
  }

  return 0;
}
