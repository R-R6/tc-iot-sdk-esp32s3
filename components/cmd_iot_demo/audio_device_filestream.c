// Copyright (c) 2026 Tencent. All rights reserved.

// ESP32-P4 音频设备文件流模拟实现：
// 采集：从 SPIFFS WAV 文件读取 PCM，pacing 线程按帧率推送
// 播放：打印收到数据大小，直接丢弃

#include "audio_device.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_dev_file";

#define SEND_WAV_PATH "/data/aitalk_send_audio.wav"
#define PCM_FRAME_BYTES (16000 * 2 * 1 * 60 / 1000)  // 16kHz/16bit/mono/60ms = 1920
#define CAPTURE_TASK_STACK (4096)
#define CAPTURE_TASK_PRIO 5
#define CAPTURE_TASK_CORE 1
#define WAV_HEADER_SIZE 44

static audio_device_config_t s_config;
static audio_device_on_capture_t s_capture_cb = NULL;
static void *s_capture_user_data = NULL;
static TaskHandle_t s_capture_task = NULL;
static volatile bool s_capturing = false;

static uint64_t _get_pts_ms(void) {
  return (uint64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void _capture_task_fn(void *arg) {
  FILE *fp = fopen(SEND_WAV_PATH, "rb");
  if (!fp) {
    ESP_LOGE(TAG, "Failed to open %s", SEND_WAV_PATH);
    s_capture_task = NULL;
    vTaskDelete(NULL);
    return;
  }
  // 跳过 WAV 头
  fseek(fp, WAV_HEADER_SIZE, SEEK_SET);

  uint8_t buf[PCM_FRAME_BYTES];
  uint64_t pts_ms = 0;
  TickType_t last_wake = xTaskGetTickCount();

  while (s_capturing) {
    size_t n = fread(buf, 1, sizeof(buf), fp);
    if (n == 0) {
      ESP_LOGI(TAG, "capture EOF");
      break;
    }
    if (n < sizeof(buf)) {
      memset(buf + n, 0, sizeof(buf) - n);
    }

    if (s_capture_cb) {
      s_capture_cb(buf, (uint32_t)sizeof(buf), TC_IOT_AUDIO_CODEC_PCM,
                   pts_ms, s_capture_user_data);
    }
    pts_ms += 60;
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(60));
  }

  fclose(fp);
  s_capture_task = NULL;
  vTaskDelete(NULL);
}

int audio_device_open(const audio_device_config_t *config) {
  if (config) s_config = *config;
  ESP_LOGI(TAG, "open (filestream): sr=%lu ch=%u bits=%u dur=%lu",
           (unsigned long)s_config.sample_rate, s_config.channels,
           s_config.bits_per_sample, (unsigned long)s_config.frame_duration_ms);

  // 检查 WAV 文件是否存在
  FILE *fp = fopen(SEND_WAV_PATH, "rb");
  if (!fp) {
    ESP_LOGE(TAG, "%s not found", SEND_WAV_PATH);
    return -1;
  }
  fclose(fp);
  return 0;
}

void audio_device_close(void) {
  audio_device_stop_capture();
  ESP_LOGI(TAG, "closed");
}

int audio_device_start_capture(audio_device_on_capture_t cb, void *user_data) {
  if (s_capturing) return 0;

  s_capture_cb = cb;
  s_capture_user_data = user_data;
  s_capturing = true;

  BaseType_t ret = xTaskCreatePinnedToCore(
      _capture_task_fn, "aud_cap", CAPTURE_TASK_STACK, NULL,
      CAPTURE_TASK_PRIO, &s_capture_task, CAPTURE_TASK_CORE);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create capture task");
    s_capturing = false;
    return -1;
  }
  ESP_LOGI(TAG, "capture started");
  return 0;
}

void audio_device_stop_capture(void) {
  if (!s_capturing) return;
  s_capturing = false;
  int wait = 0;
  while (s_capture_task && wait++ < 100) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  s_capture_cb = NULL;
  s_capture_user_data = NULL;
  ESP_LOGI(TAG, "capture stopped");
}

int audio_device_write(const uint8_t *data, uint32_t size, tc_iot_audio_codec_e codec) {
  (void)data;
  (void)codec;
  static uint32_t total = 0, count = 0;
  total += size;
  count++;
  if (count % 100 == 0) {
    ESP_LOGI(TAG, "write %lu bytes (discarded), total %lu, count %lu", (unsigned long)size, (unsigned long)total, (unsigned long)count);
  }
  return 0;
}

tc_iot_audio_codec_e audio_device_default_codec(void) {
  return TC_IOT_AUDIO_CODEC_PCM;
}

bool audio_device_codec_supported(tc_iot_audio_codec_e codec) {
  return codec == TC_IOT_AUDIO_CODEC_PCM;
}

uint32_t audio_device_frame_duration_ms(tc_iot_audio_codec_e codec) {
  (void)codec;
  return 60;
}

// FreeRTOS 实现：vTaskDelay 替代 nanosleep（与 audio_device_linux.c 行为一致）
void audio_device_sleep_ms(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}
