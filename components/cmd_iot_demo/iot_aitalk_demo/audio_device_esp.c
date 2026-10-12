// Copyright (c) 2026 Tencent. All rights reserved.

#include <string.h>

#include "audio_device.h"
#include "av_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aitalk_aud_esp";

// av_device_esp.c 的采集帧长（MIC_FRAME_MS，见 iot_demo_config.h）
#define AITALK_ESP_FRAME_MS MIC_FRAME_MS

static audio_device_config_t s_config;
static bool s_opened = false;
static audio_device_on_capture_t s_capture_cb = NULL;
static void *s_capture_user_data = NULL;

// av_device 侧回调原型为 (data, len, user_data)；
// aitalk 侧需要 (data, size, codec, pts_ms, user_data)，此处做适配转发。
static void _on_microphone_frame(const uint8_t *data, uint32_t len, void *user_data) {
  (void)user_data;
  audio_device_on_capture_t cb = s_capture_cb;
  if (cb && data && len > 0) {
    cb(data, len, TC_IOT_AUDIO_CODEC_PCM, av_device_now_ms(), s_capture_user_data);
  }
}

// ==================== 生命周期 ====================
int audio_device_open(const audio_device_config_t *config) {
  if (config) {
    s_config = *config;
  }
  if (s_opened) {
    return 0;
  }

  // av_device_init 内部幂等（已初始化则直接返回），且摄像头初始化失败仅告警不阻断，
  // 因此 aitalk-only（无视频）场景可安全调用。
  av_device_init(NULL);

  s_capture_cb = NULL;
  s_capture_user_data = NULL;
  s_opened = true;
  ESP_LOGI(TAG, "opened: sr=%lu ch=%u bits=%u frame=%ums (hw path)",
           (unsigned long)s_config.sample_rate, (unsigned)s_config.channels,
           (unsigned)s_config.bits_per_sample, (unsigned)AITALK_ESP_FRAME_MS);
  return 0;
}

void audio_device_close(void) {
  if (!s_opened) {
    return;
  }
  audio_device_stop_capture();
  // 与 beken 各实现行为一致：close 释放采集/播放硬件。
  // 注意这会同时释放 av_device 的视频链路，故不建议与视频通话同时使用。
  av_device_deinit();
  s_opened = false;
  ESP_LOGI(TAG, "closed");
}

// ==================== 采集 ====================
int audio_device_start_capture(audio_device_on_capture_t cb, void *user_data) {
  if (!cb) {
    return -1;
  }
  if (!s_opened) {
    ESP_LOGE(TAG, "device not opened");
    return -1;
  }

  s_capture_cb = cb;
  s_capture_user_data = user_data;

  // av_device_start_audio 同时只允许一个监听者，已被占用或硬件未就绪时返回 -1。
  if (av_device_start_audio(NULL, _on_microphone_frame, NULL) != 0) {
    s_capture_cb = NULL;
    s_capture_user_data = NULL;
    ESP_LOGE(TAG, "start capture failed (mic busy or hardware not ready)");
    return -1;
  }
  ESP_LOGI(TAG, "capture started");
  return 0;
}

void audio_device_stop_capture(void) {
  if (!s_capture_cb) {
    return;
  }
  s_capture_cb = NULL;
  s_capture_user_data = NULL;
  av_device_stop_audio(_on_microphone_frame, NULL);
  ESP_LOGI(TAG, "capture stopped");
}

// ==================== 播放 ====================
int audio_device_write(const uint8_t *data, uint32_t size, tc_iot_audio_codec_e codec) {
  if (!s_opened || !data || size == 0) {
    return -1;
  }
  // av_device_playout_audio 只接受单声道 S16LE PCM，无解码能力。
  if (codec != TC_IOT_AUDIO_CODEC_PCM) {
    ESP_LOGW(TAG, "unsupported playback codec %d (PCM only)", (int)codec);
    return -1;
  }
  av_device_playout_audio(data, size);
  return 0;
}

// ==================== 编码配置查询 ====================
tc_iot_audio_codec_e audio_device_default_codec(void) {
  return TC_IOT_AUDIO_CODEC_AAC;
}

bool audio_device_codec_supported(tc_iot_audio_codec_e codec) {
  return codec == TC_IOT_AUDIO_CODEC_PCM;
}

uint32_t audio_device_frame_duration_ms(tc_iot_audio_codec_e codec) {
  (void)codec;
  return AITALK_ESP_FRAME_MS;
}

void audio_device_sleep_ms(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}
