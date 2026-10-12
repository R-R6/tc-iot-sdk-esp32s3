// Copyright (c) 2026 Tencent. All rights reserved.

#ifndef SRC_DEMO_MINI_AV_TEST_VIDEO_SOURCE_H_
#define SRC_DEMO_MINI_AV_TEST_VIDEO_SOURCE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct video_source_t video_source_t;

/**
 * @brief 创建视频源（从编译期内嵌的 H264 数组读取帧）
 */
video_source_t *video_source_create(void);

/**
 * @brief 销毁视频源
 */
void video_source_destroy(video_source_t *src);

/**
 * @brief 获取下一帧 H264 数据
 * @param src       视频源句柄
 * @param data      [out] 帧数据指针（指向内部静态数组，不需要释放）
 * @param size      [out] 帧数据大小
 * @param is_key    [out] 是否为关键帧
 * @param pts_ms    [out] 时间戳（毫秒）
 * @return 0 成功，-1 失败
 */
int video_source_get_frame(video_source_t *src, const uint8_t **data, uint32_t *size, bool *is_key, uint64_t *pts_ms);

#ifdef __cplusplus
}
#endif

#endif  // SRC_DEMO_MINI_AV_TEST_VIDEO_SOURCE_H_
