// Copyright (c) 2026 Tencent. All rights reserved.

// 统一音视频设备文件流模拟实现：
// 音频采集：从 SPIFFS WAV 读 PCM，20ms pacing 回调
// 视频采集：使用 video_source（内嵌 H264 帧索引表），40ms pacing 循环回调
// 播放：打日志丢弃

#include <stdio.h>
#include <string.h>

#include "av_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "video_source.h"

static const char *TAG = "av_dev_filestream";

#define SEND_WAV_PATH "/data/aitalk_send_audio.wav"
#define WAV_HEADER_SIZE 44
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_FRAME_MS 20
#define AUDIO_FRAME_BYTES (AUDIO_SAMPLE_RATE * 2 * AUDIO_FRAME_MS / 1000)  // 640 bytes
#define AV_VIDEO_FRAME_MS 40                                               // 25fps

#define TASK_STACK_SIZE (4096)
#define AUDIO_TASK_PRIO 5
#define VIDEO_TASK_PRIO 5

static audio_device_callback_t s_audio_cb = NULL;
static void *s_audio_user_data = NULL;
static video_device_callback_t s_video_cb = NULL;
static void *s_video_user_data = NULL;

static TaskHandle_t s_audio_task = NULL;
static TaskHandle_t s_video_task = NULL;
static volatile bool s_audio_running = false;
static volatile bool s_video_running = false;
static video_source_t *s_video_src = NULL;

static void _audio_task_fn(void *arg) {
  FILE *fp = fopen(SEND_WAV_PATH, "rb");
  if (!fp) {
    ESP_LOGE(TAG, "Failed to open %s", SEND_WAV_PATH);
    s_audio_task = NULL;
    vTaskDelete(NULL);
    return;
  }
  fseek(fp, WAV_HEADER_SIZE, SEEK_SET);

  uint8_t buf[AUDIO_FRAME_BYTES];
  TickType_t last_wake = xTaskGetTickCount();

  while (s_audio_running) {
    size_t n = fread(buf, 1, sizeof(buf), fp);
    if (n == 0) {
      ESP_LOGI(TAG, "audio EOF");
      break;
    }
    if (n < sizeof(buf)) {
      memset(buf + n, 0, sizeof(buf) - n);
    }
    if (s_audio_cb) {
      s_audio_cb(buf, sizeof(buf), s_audio_user_data);
    }
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(AUDIO_FRAME_MS));
  }

  fclose(fp);
  s_audio_task = NULL;
  vTaskDelete(NULL);
}

static void _video_task_fn(void *arg) {
  if (!s_video_src) {
    s_video_src = video_source_create();
  }
  if (!s_video_src) {
    ESP_LOGE(TAG, "Failed to create video source");
    s_video_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  TickType_t last_wake = xTaskGetTickCount();

  while (s_video_running) {
    const uint8_t *data = NULL;
    uint32_t size = 0;
    bool is_key = false;
    uint64_t pts_ms = 0;

    if (video_source_get_frame(s_video_src, &data, &size, &is_key, &pts_ms) == 0) {
      if (s_video_cb) {
        s_video_cb(data, size, is_key, TC_IOT_VIDEO_CODEC_H264, s_video_user_data);
      }
    }
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(AV_VIDEO_FRAME_MS));
  }

  s_video_task = NULL;
  vTaskDelete(NULL);
}

void av_device_init(const av_device_config_t *cfg) {
  (void)cfg;
  ESP_LOGI(TAG, "init (filestream)");
}

void av_device_deinit(void) {
  av_device_stop_audio(s_audio_cb, s_audio_user_data);
  av_device_stop_video(s_video_cb, s_video_user_data);
  if (s_video_src) {
    video_source_destroy(s_video_src);
    s_video_src = NULL;
  }
  ESP_LOGI(TAG, "deinit");
}

int av_device_start_audio(const char *audio_file_path, audio_device_callback_t callback, void *user_data) {
  (void)audio_file_path;
  if (s_audio_running) return 0;
  s_audio_cb = callback;
  s_audio_user_data = user_data;
  s_audio_running = true;

  BaseType_t ret =
      xTaskCreatePinnedToCore(_audio_task_fn, "av_aud", TASK_STACK_SIZE, NULL, AUDIO_TASK_PRIO, &s_audio_task, 1);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create audio task");
    s_audio_running = false;
    return -1;
  }
  ESP_LOGI(TAG, "audio started");
  return 0;
}

void av_device_stop_audio(audio_device_callback_t callback, void *user_data) {
  (void)callback;
  (void)user_data;
  if (!s_audio_running) return;
  s_audio_running = false;
  int wait = 0;
  while (s_audio_task && wait++ < 100) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  s_audio_cb = NULL;
  s_audio_user_data = NULL;
  ESP_LOGI(TAG, "audio stopped");
}

int av_device_start_video(const char *video_file_path, video_device_callback_t callback, void *user_data) {
  (void)video_file_path;
  if (s_video_running) return 0;
  s_video_cb = callback;
  s_video_user_data = user_data;
  s_video_running = true;

  BaseType_t ret =
      xTaskCreatePinnedToCore(_video_task_fn, "av_vid", TASK_STACK_SIZE, NULL, VIDEO_TASK_PRIO, &s_video_task, 1);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create video task");
    s_video_running = false;
    return -1;
  }
  ESP_LOGI(TAG, "video started");
  return 0;
}

void av_device_stop_video(video_device_callback_t callback, void *user_data) {
  (void)callback;
  (void)user_data;
  if (!s_video_running) return;
  s_video_running = false;
  int wait = 0;
  while (s_video_task && wait++ < 100) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  s_video_cb = NULL;
  s_video_user_data = NULL;
  ESP_LOGI(TAG, "video stopped");
}

void av_device_playout_audio(const uint8_t *data, uint32_t len) {
  (void)data;
  ESP_LOGD(TAG, "playout_audio %lu bytes (discarded)", (unsigned long)len);
}

void av_device_playout_video(const uint8_t *data, uint32_t len) {
  (void)data;
  ESP_LOGD(TAG, "playout_video %lu bytes (discarded)", (unsigned long)len);
}

// esp32当前不渲染远端视频；保留 stub 以对齐 Linux 接口。
void av_device_playout_mjpg(const uint8_t *data, uint32_t len) {
  (void)data;
  (void)len;
  ESP_LOGD(TAG, "playout_mjpg %lu bytes (discarded)", (unsigned long)len);
}


void av_device_request_idr(void) { ESP_LOGI(TAG, "request_idr (no-op in filestream mode)"); }

int av_device_get_event_picture(uint32_t event_id, uint8_t *buf, uint32_t buf_size, uint32_t *out_len) {
  (void)event_id;
  (void)buf;
  (void)buf_size;
  if (out_len) {
    *out_len = 0;
  }
  return -1;
}

// FreeRTOS 实现：用 tick 计 ms（与 av_device_linux.c 行为一致，用于 av_sender 的 pts_ms）
uint64_t av_device_now_ms(void) { return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }
