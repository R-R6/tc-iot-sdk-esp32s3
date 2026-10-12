// Copyright (c) 2026 Tencent. All rights reserved.
//
// ESP32-S3-Korvo-2 V3.0 音频设备实现：
//   - ES7210 音频上行 + ES8311 音频下行
//   - 端到端单声道：I2S RX 仅取 slot0，采集/播放 Codec 声道数均为 1，省去上/下混开销
//   - AEC 回声消除使用 esp-sr aec_* 接口，内部工作内存置于 PSRAM
//   - S3 未接摄像头：视频相关接口保留空桩以对齐 av_device.h 契约

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "av_device.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_common.h"
#include "driver/i2s_tdm.h"
#include "esp_afe_aec.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define TAG "av_esp"

// ==================== 平台管脚宏定义 ====================
// ---- ESP32-S3-Korvo-2 V3.0 管脚 ----
#define AV_ESP_I2C_PORT I2C_NUM_0
#define AV_ESP_I2C_SDA_GPIO GPIO_NUM_17
#define AV_ESP_I2C_SCL_GPIO GPIO_NUM_18

#define AV_ESP_I2S_PORT I2S_NUM_0
#define AV_ESP_I2S_MCLK_GPIO GPIO_NUM_16
#define AV_ESP_I2S_BCLK_GPIO GPIO_NUM_9
#define AV_ESP_I2S_WS_GPIO GPIO_NUM_45
#define AV_ESP_I2S_DOUT_GPIO GPIO_NUM_8
#define AV_ESP_I2S_DIN_GPIO GPIO_NUM_10
// 端到端单声道：I2S RX 仅取 slot0，避免立体声下混开销
#define AV_ESP_I2S_TDM_SLOT_MASK (I2S_TDM_SLOT0)
#define AV_ESP_PA_GPIO GPIO_NUM_48

// ==================== 公共宏定义 ====================

// ---- 音频：Codec 与播放缓冲 ----
// 端到端单声道：采集/播放硬件 Codec 声道数均为 1，省去上/下混开销。
#define AV_ESP_CODEC_CHANNELS 1
#define AV_ESP_ES8311_OUT_VOL 60

#define AV_ESP_CAPTURE_FRAME_BYTES \
  PCM_FRAME_BYTES(MIC_SAMPLE_RATE, AV_ESP_CODEC_CHANNELS, MIC_FRAME_MS)
#define AV_ESP_PLAYOUT_FRAME_BYTES \
  PCM_FRAME_BYTES(SPK_SAMPLE_RATE, AV_ESP_CODEC_CHANNELS, SPK_FRAME_MS)

// ---- 音频采集任务 ----
#define AV_ESP_AUDIO_TASK_STACK (8 * 1024)
#define AV_ESP_AUDIO_TASK_PRIORITY 5
#define AV_ESP_AUDIO_READ_TIMEOUT_MS 100
#define AV_ESP_AUDIO_STOP_WAIT_MS 1000

// ---- ES7210 麦克风增益 ----
#define AV_ESP_ES7210_MIC_GAIN_DB 30.0f
// 端到端单声道：仅选用一路麦克风，直接产生单声道采集数据
#define AV_ESP_ES7210_MIC_SELECTED (ES7210_SEL_MIC1)

// ---- AEC 回声消除（esp-sr AFE）----
// 采集链路加入声学回声消除：以"播放出去的远端音频"为参考信号，
// 从麦克风信号中消除被采回的扬声器/Codec 回采声，避免机器人听到自己说话
// 而产生回声/自激（即使不接扬声器，DAC 回采/内部串扰也会形成回声）。
// AEC 总开关（临时验证用）：置 0 则完全不创建 AEC，麦克风信号直传上抛，
// 用于量化 AEC 到底占用多少 internal RAM、以及关掉后通话是否稳定。
// 验证完毕后应改回 1 恢复回声消除。
#ifndef AV_ESP_AEC_ENABLE
#  define AV_ESP_AEC_ENABLE 1
#endif
// 滤波器长度：推荐 4，越大 CPU 占用越高、可消除的回声拖尾越长。
#define AV_ESP_AEC_FILTER_LENGTH 4
// 参考信号环形缓冲容量（单声道样本数），约 500ms @16kHz，吸收播放突发与时序抖动。
#define AV_ESP_AEC_REFERENCE_RING_SAMPLES (MIC_SAMPLE_RATE / 2)

// ==================== 公共全局变量 ====================

// ---- 生命周期与回调 ----
static bool audio_hardware_initialized = false;
static volatile bool audio_started = false;
static volatile bool audio_task_running = false;
static TaskHandle_t audio_task_handle = NULL;
static audio_device_callback_t audio_callback = NULL;
static void *audio_callback_context = NULL;

// ---- I2C / I2S 句柄 ----
static i2c_master_bus_handle_t audio_i2c_bus = NULL;
static i2s_chan_handle_t i2s_speaker_channel = NULL;
static i2s_chan_handle_t i2s_microphone_channel = NULL;
static const audio_codec_data_if_t *i2s_data_interface = NULL;

// ---- 播放 Codec（ES8311） ----
static const audio_codec_ctrl_if_t *speaker_control_interface = NULL;
static const audio_codec_gpio_if_t *speaker_gpio_interface = NULL;
static const audio_codec_if_t *speaker_codec_interface = NULL;
static esp_codec_dev_handle_t speaker_codec_device = NULL;

// ---- 录音 Codec（ES7210） ----
static esp_codec_dev_handle_t microphone_codec_device = NULL;
static const audio_codec_if_t *microphone_codec_interface = NULL;
static const audio_codec_ctrl_if_t *microphone_control_interface = NULL;

// ---- 音频帧缓存 ----
// 端到端单声道后：采集读取缓冲即单声道帧，播放直写远端单声道，无需中转。
static int16_t capture_read_buffer[AV_ESP_CAPTURE_FRAME_BYTES / sizeof(int16_t)];
static int16_t capture_mono_buffer[MIC_FRAME_BYTES / sizeof(int16_t)];

// ---- 下行音频播放诊断 ----
static uint32_t audio_playback_received_frame_count = 0;
static uint32_t audio_playback_discard_frame_count = 0;
static uint32_t audio_playback_write_fail_frame_count = 0;

// ---- 声学回声消除（Acoustic Echo Cancellation）状态 ----
static aec_handle_t *aec_handle = NULL;
static int aec_chunk_samples = 0;
static int16_t *aec_microphone_buffer = NULL;
static int16_t *aec_reference_buffer = NULL;
static int16_t *aec_output_buffer = NULL;
static size_t aec_frame_accumulated = 0;
// 参考信号环形缓冲：播放线程写入，采集线程读取
static int16_t *aec_reference_ring_buffer = NULL;
static volatile uint32_t aec_reference_ring_buffer_head = 0;
static volatile uint32_t aec_reference_ring_buffer_tail = 0;
static portMUX_TYPE aec_reference_ring_buffer_lock = portMUX_INITIALIZER_UNLOCKED;

// ==================== 前置声明 ====================

// 音频内部实现
static esp_err_t _i2c_open(void);
static esp_err_t _i2s_open(void);  // S3=TDM
static esp_err_t _speaker_open(void);
static esp_err_t _microphone_open(void);  // S3=ES7210
static void _i2c_close(void);
static void _i2s_close(void);
static void _speaker_close(void);
static void _microphone_close(void);
static void _get_microphone_audio_stream(void *arg);

// AEC 参考信号推送
static void _aec_reference_push(const int16_t *samples, size_t count);

// 声学回声消除内部实现
static esp_err_t _aec_open(void);
static void _aec_close(void);

// ==================== 公开接口 ====================

void av_device_init(const av_device_config_t *cfg) {
  (void)cfg;
  if (audio_hardware_initialized) {
    return;
  }

  esp_err_t err = _i2c_open();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "I2C init failed: %s", esp_err_to_name(err));
    return;
  }
  err = _i2s_open();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "I2S init failed: %s", esp_err_to_name(err));
    return;
  }
  err = _speaker_open();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "play codec init failed: %s", esp_err_to_name(err));
    return;
  }
  err = _microphone_open();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "record codec init failed: %s", esp_err_to_name(err));
    return;
  }

  if (_aec_open() != ESP_OK) {
    ESP_LOGW(TAG, "AEC init failed, capture will run WITHOUT echo cancellation");
  }

  audio_hardware_initialized = true;
  ESP_LOGI(TAG,
           "initialized: i2c_sda=%d i2c_scl=%d "
           "mclk=%d bclk=%d ws=%d dout=%d din=%d pa=%d",
           (int)AV_ESP_I2C_SDA_GPIO, (int)AV_ESP_I2C_SCL_GPIO, (int)AV_ESP_I2S_MCLK_GPIO,
           (int)AV_ESP_I2S_BCLK_GPIO, (int)AV_ESP_I2S_WS_GPIO, (int)AV_ESP_I2S_DOUT_GPIO,
           (int)AV_ESP_I2S_DIN_GPIO, (int)AV_ESP_PA_GPIO);
}

void av_device_deinit(void) {
  if (audio_started) {
    audio_task_running = false;
    int wait = AV_ESP_AUDIO_STOP_WAIT_MS / 20;
    while (audio_task_handle != NULL && wait-- > 0) {
      rtos_delay_milliseconds(20);
    }
    audio_started = false;
  }
  _aec_close();
  _microphone_close();
  _speaker_close();
  _i2s_close();
  _i2c_close();
  audio_hardware_initialized = false;
  ESP_LOGI(TAG, "deinitialized");
}

int av_device_start_audio(const char *audio_file_path, audio_device_callback_t callback,
                          void *user_data) {
  if (!audio_hardware_initialized || !callback) {
    return -1;
  }
  if (!microphone_codec_device || !i2s_microphone_channel) {
    ESP_LOGE(TAG, "mic not ready");
    return -1;
  }
  if (audio_callback && audio_callback == callback && audio_callback_context == user_data) {
    return 0;
  }
  if (audio_callback) {
    ESP_LOGE(TAG, "audio listener already occupied");
    return -1;
  }

  audio_callback = callback;
  audio_callback_context = user_data;

  if (!audio_started) {
    audio_task_running = true;
    audio_started = true;
    // 任务栈从 PSRAM 分配，避免占用紧张的内部 SRAM（需 sdkconfig 允许外部栈）。
    // 采集任务不在中断/flash-cache-disable 上下文，可安全使用外部内存栈。
    // 注意：以此方式创建的任务退出时必须用 vTaskDeleteWithCaps 释放栈。
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        _get_microphone_audio_stream, "av_esp_mic", AV_ESP_AUDIO_TASK_STACK, NULL,
        AV_ESP_AUDIO_TASK_PRIORITY, &audio_task_handle, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
      audio_task_running = false;
      audio_started = false;
      audio_task_handle = NULL;
      audio_callback = NULL;
      audio_callback_context = NULL;
      ESP_LOGE(TAG, "failed to create audio task");
      return -1;
    }
    ESP_LOGI(TAG, "audio capture started: raw_ch=%d out_frame=%d bytes", AV_ESP_CODEC_CHANNELS,
             MIC_FRAME_BYTES);
  }

  return 0;
}

void av_device_stop_audio(audio_device_callback_t callback, void *user_data) {
  if (!audio_callback || audio_callback != callback || audio_callback_context != user_data) {
    return;
  }

  audio_callback = NULL;
  audio_callback_context = NULL;

  if (audio_started) {
    audio_task_running = false;
    int wait = AV_ESP_AUDIO_STOP_WAIT_MS / 20;
    while (audio_task_handle != NULL && wait-- > 0) {
      rtos_delay_milliseconds(20);
    }
    audio_started = false;
    ESP_LOGI(TAG, "audio capture stopped");
  }
}

// 将远端单声道 PCM 推入 AEC 参考环形缓冲，作为回声消除参考信号。
// 由播放函数在 esp_codec_dev_write 前紧邻调用，使参考信号贴近实际播放时刻。
static void _aec_reference_push(const int16_t *samples, size_t count) {
  if (!aec_reference_ring_buffer || !samples || count == 0) {
    return;
  }
  portENTER_CRITICAL(&aec_reference_ring_buffer_lock);
  for (size_t i = 0; i < count; ++i) {
    uint32_t next_index = (aec_reference_ring_buffer_head + 1) % AV_ESP_AEC_REFERENCE_RING_SAMPLES;
    if (next_index == aec_reference_ring_buffer_tail) {
      aec_reference_ring_buffer_tail =
          (aec_reference_ring_buffer_tail + 1) % AV_ESP_AEC_REFERENCE_RING_SAMPLES;
    }
    aec_reference_ring_buffer[aec_reference_ring_buffer_head] = samples[i];
    aec_reference_ring_buffer_head = next_index;
  }
  portEXIT_CRITICAL(&aec_reference_ring_buffer_lock);
}

void av_device_playout_audio(const uint8_t *data, uint32_t len) {
  static bool s_playout_first_logged = false;

  audio_playback_received_frame_count++;

  if (!audio_hardware_initialized || !speaker_codec_device || !data || len == 0) {
    audio_playback_discard_frame_count++;
    return;
  }

  if (!s_playout_first_logged) {
    ESP_LOGI(TAG, "[PLY] first frame: len=%u", (unsigned)len);
    s_playout_first_logged = true;
  } else if (audio_playback_received_frame_count % 2000 == 0) {
    ESP_LOGI(TAG, "[PLY] count=%u discard=%u fail=%u",
             (unsigned)audio_playback_received_frame_count,
             (unsigned)audio_playback_discard_frame_count,
             (unsigned)audio_playback_write_fail_frame_count);
  }

  // 远端下行已是单声道 S16LE，直接同步写硬件，无需 L/R 上混。
  const uint32_t aligned = len & ~(uint32_t)(sizeof(int16_t) - 1);  // 对齐到样本
  if (aligned == 0) {
    return;
  }

  // 紧邻 esp_codec_dev_write 推送 AEC 参考，使参考信号贴近实际播放时刻。
  _aec_reference_push((const int16_t *)(const void *)data, aligned / sizeof(int16_t));

  int ret = esp_codec_dev_write(speaker_codec_device, (void *)data, (int)aligned);
  if (ret != ESP_CODEC_DEV_OK) {
    audio_playback_write_fail_frame_count++;
    ESP_LOGW(TAG, "[PLY] write fail #%u: ret=%d bytes=%u",
             (unsigned)audio_playback_write_fail_frame_count, ret, (unsigned)aligned);
  }
}

// S3-Korvo-2 未接摄像头：视频接口保留空桩以对齐 av_device.h 契约。
int av_device_start_video(const char *video_file_path, video_device_callback_t callback,
                          void *user_data) {
  (void)video_file_path;
  (void)callback;
  (void)user_data;
  return 0;
}

void av_device_stop_video(video_device_callback_t callback, void *user_data) {
  (void)callback;
  (void)user_data;
}

// esp32目前没有h264硬件编码器，不实现视频下行
void av_device_playout_video(const uint8_t *data, uint32_t len) {
  (void)data;
  (void)len;
}

// esp32当前不渲染远端视频；保留 stub 以对齐 Linux 接口。
void av_device_playout_mjpg(const uint8_t *data, uint32_t len) {
  (void)data;
  (void)len;
}

void av_device_request_idr(void) {}

uint64_t av_device_now_ms(void) {
  return rtos_get_time();
}

// MP4 解封装：硬件平台不支持，返回空桩
uint64_t av_device_get_mp4_duration_ms(const char *mp4_file_path) {
  (void)mp4_file_path;
  return 0;
}

uint64_t av_device_seek_mp4_ms(uint64_t target_ms) {
  (void)target_ms;
  return 0;
}

// 事件抓拍图（云存 demo 用）：S3-Korvo-2 未接摄像头，无抓拍链路。
// 与 av_device_beken_amp_av.c 保持一致：显式失败，cloud_storage_demo 按 has_pic=false 处理。
int av_device_get_event_picture(uint32_t event_id, uint8_t *buf, uint32_t buf_size,
                                uint32_t *out_len) {
  (void)event_id;
  (void)buf;
  (void)buf_size;
  if (out_len) {
    *out_len = 0;
  }
  return -1;
}

// ==================== 公共音频实现 ====================
// I2C 总线初始化
static esp_err_t _i2c_open(void) {
  if (audio_i2c_bus) {
    return ESP_OK;
  }

  i2c_master_bus_config_t i2c_cfg = {
      .i2c_port = AV_ESP_I2C_PORT,
      .sda_io_num = AV_ESP_I2C_SDA_GPIO,
      .scl_io_num = AV_ESP_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t err = i2c_new_master_bus(&i2c_cfg, &audio_i2c_bus);
  if (err != ESP_OK) {
    audio_i2c_bus = NULL;
  }
  return err;
}

// 播放 Codec 初始化（ES8311 DAC）
static esp_err_t _speaker_open(void) {
  if (speaker_codec_device) {
    return ESP_OK;
  }
  if (!i2s_data_interface || !audio_i2c_bus) {
    return ESP_ERR_INVALID_STATE;
  }

  audio_codec_i2c_cfg_t i2c_cfg = {
      .port = AV_ESP_I2C_PORT,
      .addr = ES8311_CODEC_DEFAULT_ADDR,
      .bus_handle = audio_i2c_bus,
  };
  speaker_control_interface = audio_codec_new_i2c_ctrl(&i2c_cfg);
  ESP_RETURN_ON_FALSE(speaker_control_interface, ESP_ERR_NO_MEM, TAG, "play ctrl_if alloc fail");

  speaker_gpio_interface = audio_codec_new_gpio();
  ESP_RETURN_ON_FALSE(speaker_gpio_interface, ESP_ERR_NO_MEM, TAG, "gpio_if alloc fail");

  es8311_codec_cfg_t es8311_cfg = {
      .ctrl_if = speaker_control_interface,
      .gpio_if = speaker_gpio_interface,
      .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
      .master_mode = false,
      .use_mclk = (AV_ESP_I2S_MCLK_GPIO >= 0),
      .pa_pin = AV_ESP_PA_GPIO,
      .pa_reverted = false,
      .no_dac_ref = true,
      .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
  };
  speaker_codec_interface = es8311_codec_new(&es8311_cfg);
  ESP_RETURN_ON_FALSE(speaker_codec_interface, ESP_ERR_NO_MEM, TAG, "play codec_if alloc fail");

  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_OUT,
      .codec_if = speaker_codec_interface,
      .data_if = i2s_data_interface,
  };
  speaker_codec_device = esp_codec_dev_new(&dev_cfg);
  ESP_RETURN_ON_FALSE(speaker_codec_device, ESP_ERR_NO_MEM, TAG, "play codec_dev alloc fail");

  esp_codec_dev_sample_info_t sample_cfg = {
      .bits_per_sample = 16,
      .channel = AV_ESP_CODEC_CHANNELS,
      .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
      .sample_rate = SPK_SAMPLE_RATE,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  ESP_RETURN_ON_FALSE(esp_codec_dev_open(speaker_codec_device, &sample_cfg) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, TAG, "play codec_dev open fail");
  ESP_RETURN_ON_FALSE(
      esp_codec_dev_set_out_vol(speaker_codec_device, AV_ESP_ES8311_OUT_VOL) == ESP_CODEC_DEV_OK,
      ESP_FAIL, TAG, "set output vol fail");

  ESP_LOGI(TAG, "ES8311 speaker codec opened: sr=%d ch=%d vol=%d", SPK_SAMPLE_RATE,
           AV_ESP_CODEC_CHANNELS, AV_ESP_ES8311_OUT_VOL);
  return ESP_OK;
}

// ---- I2C 总线反初始化 ----
static void _i2c_close(void) {
  if (audio_i2c_bus) {
    i2c_del_master_bus(audio_i2c_bus);
    audio_i2c_bus = NULL;
    ESP_LOGI(TAG, "i2c closed");
  }
}

// ---- 录音 Codec 反初始化 ----
static void _microphone_close(void) {
  if (microphone_codec_device) {
    esp_codec_dev_close(microphone_codec_device);
    esp_codec_dev_delete(microphone_codec_device);
    microphone_codec_device = NULL;
  }
  if (microphone_codec_interface) {
    audio_codec_delete_codec_if(microphone_codec_interface);
    microphone_codec_interface = NULL;
  }
  if (microphone_control_interface) {
    audio_codec_delete_ctrl_if(microphone_control_interface);
    microphone_control_interface = NULL;
  }
  ESP_LOGI(TAG, "record codec closed");
}

// ---- 播放 Codec 反初始化 ----
static void _speaker_close(void) {
  if (speaker_codec_device) {
    esp_codec_dev_close(speaker_codec_device);
    esp_codec_dev_delete(speaker_codec_device);
    speaker_codec_device = NULL;
  }
  if (speaker_codec_interface) {
    audio_codec_delete_codec_if(speaker_codec_interface);
    speaker_codec_interface = NULL;
  }
  if (speaker_control_interface) {
    audio_codec_delete_ctrl_if(speaker_control_interface);
    speaker_control_interface = NULL;
  }
  if (speaker_gpio_interface) {
    audio_codec_delete_gpio_if(speaker_gpio_interface);
    speaker_gpio_interface = NULL;
  }
  ESP_LOGI(TAG, "play codec closed");
}

// ---- I2S 通道反初始化 ----
static void _i2s_close(void) {
  if (i2s_data_interface) {
    audio_codec_delete_data_if(i2s_data_interface);
    i2s_data_interface = NULL;
  }
  if (i2s_microphone_channel) {
    i2s_del_channel(i2s_microphone_channel);
    i2s_microphone_channel = NULL;
  }
  if (i2s_speaker_channel) {
    i2s_del_channel(i2s_speaker_channel);
    i2s_speaker_channel = NULL;
  }
  ESP_LOGI(TAG, "i2s closed");
}

// ==================== AEC 回声消除实现 ====================

// ---- 创建回声消除器实例与相关缓冲 ----
static esp_err_t _aec_open(void) {
#if !AV_ESP_AEC_ENABLE
  // 临时关闭 AEC 验证：不创建任何回声消除资源，aec_handle 保持 NULL，
  // 采集任务将自动走"下混直传"分支。
  ESP_LOGW(TAG, "AEC DISABLED (AV_ESP_AEC_ENABLE=0): mic passthrough, no echo cancellation");
  return ESP_OK;
#else
  if (aec_handle) {
    return ESP_OK;
  }

  // AEC 内部工作内存（约 71KB）改由 PSRAM 分配，避免耗尽紧张的内部 SRAM。
  // 否则通话进房时 lwip/TRTC 创建 socket 会因 internal RAM 不足而 OOM 崩溃。
  aec_config_t aec_cfg = {
      .mic_num = 1,
      .ref_num = 1,
      .out_num = 1,
      .filter_length = AV_ESP_AEC_FILTER_LENGTH,
      .sample_rate = MIC_SAMPLE_RATE,
      .caps = MALLOC_CAP_SPIRAM,
      // 降规格：LOW_COST 相比 HIGH_PERF 内部 RAM/CPU 占用更低，
      // 语音通话场景回声消除质量足够；配合 filter_length=4 进一步压缩占用。
      .mode = AEC_MODE_VOIP_LOW_COST,
      .nlp_level = AEC_NLP_LEVEL_AGGR,
  };
  aec_handle = aec_create_from_config(&aec_cfg);
  if (!aec_handle) {
    ESP_LOGE(TAG, "aec_create_from_config failed");
    return ESP_FAIL;
  }

  aec_chunk_samples = aec_get_chunksize(aec_handle);
  if (aec_chunk_samples <= 0) {
    ESP_LOGE(TAG, "invalid AEC chunk=%d", aec_chunk_samples);
    _aec_close();
    return ESP_FAIL;
  }

  // 采集缓冲按 20ms 单声道分配，AEC chunk 不能超过它，否则单次读取会溢出。
  if ((size_t)aec_chunk_samples * AV_ESP_CODEC_CHANNELS * sizeof(int16_t) >
      sizeof(capture_read_buffer)) {
    ESP_LOGE(TAG, "AEC chunk %d too large for capture buffer", aec_chunk_samples);
    _aec_close();
    return ESP_FAIL;
  }

  // AFE 要求 16 字节对齐的输入/输出缓冲
  aec_microphone_buffer =
      heap_caps_aligned_calloc(16, (size_t)aec_chunk_samples, sizeof(int16_t), MALLOC_CAP_INTERNAL);
  aec_reference_buffer =
      heap_caps_aligned_calloc(16, (size_t)aec_chunk_samples, sizeof(int16_t), MALLOC_CAP_INTERNAL);
  aec_output_buffer =
      heap_caps_aligned_calloc(16, (size_t)aec_chunk_samples, sizeof(int16_t), MALLOC_CAP_INTERNAL);
  // 参考信号环形缓冲仅供软件读写（非 DMA/中断），放 PSRAM 以省内部 SRAM(~16KB)。
  aec_reference_ring_buffer =
      heap_caps_calloc(AV_ESP_AEC_REFERENCE_RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!aec_microphone_buffer || !aec_reference_buffer || !aec_output_buffer ||
      !aec_reference_ring_buffer) {
    ESP_LOGE(TAG, "AEC buffer alloc failed");
    _aec_close();
    return ESP_ERR_NO_MEM;
  }

  aec_reference_ring_buffer_head = 0;
  aec_reference_ring_buffer_tail = 0;
  aec_frame_accumulated = 0;

  ESP_LOGI(TAG, "AEC ready(PSRAM): filter=%d chunk=%d (%d ms)", AV_ESP_AEC_FILTER_LENGTH,
           aec_chunk_samples, aec_chunk_samples * 1000 / MIC_SAMPLE_RATE);
  return ESP_OK;
#endif  // AV_ESP_AEC_ENABLE
}

// ---- 释放回声消除器实例与相关缓冲 ----
static void _aec_close(void) {
  if (aec_handle) {
    aec_destroy(aec_handle);
    aec_handle = NULL;
  }
  if (aec_microphone_buffer) {
    heap_caps_free(aec_microphone_buffer);
    aec_microphone_buffer = NULL;
  }
  if (aec_reference_buffer) {
    heap_caps_free(aec_reference_buffer);
    aec_reference_buffer = NULL;
  }
  if (aec_output_buffer) {
    heap_caps_free(aec_output_buffer);
    aec_output_buffer = NULL;
  }
  if (aec_reference_ring_buffer) {
    heap_caps_free(aec_reference_ring_buffer);
    aec_reference_ring_buffer = NULL;
  }
  aec_chunk_samples = 0;
  aec_frame_accumulated = 0;
  ESP_LOGI(TAG, "AEC closed");
}

// MIC 采集任务：从 I2S RX 读取单声道帧；
// 若回声消除器可用则经回声消除后再上抛，否则直接上抛。
static void _get_microphone_audio_stream(void *arg) {
  (void)arg;

  uint32_t frame_count = 0;
  const size_t output_frame_samples = MIC_FRAME_BYTES / sizeof(int16_t);

  if (aec_handle) {
    const int chunk_samples = aec_chunk_samples;
    const size_t read_bytes = (size_t)chunk_samples * AV_ESP_CODEC_CHANNELS * sizeof(int16_t);
    aec_frame_accumulated = 0;

    while (audio_task_running) {
      size_t bytes_read = 0;
      esp_err_t err = i2s_channel_read(i2s_microphone_channel, aec_microphone_buffer, read_bytes,
                                       &bytes_read, AV_ESP_AUDIO_READ_TIMEOUT_MS);
      if (!audio_task_running) {
        break;
      }
      if (err == ESP_ERR_TIMEOUT) {
        continue;
      }
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s read failed: %s", esp_err_to_name(err));
        rtos_delay_milliseconds(10);
        continue;
      }
      if (bytes_read != read_bytes) {
        ESP_LOGW(TAG, "drop partial AEC frame: %u/%u", (unsigned)bytes_read, (unsigned)read_bytes);
        continue;
      }

      // 端到端单声道：I2S 直接读出单声道近端麦克风信号，无需下混。
      // 取出与本帧对应的播放参考信号。
      // 按环形边界拆成 1~2 段 memcpy，避免逐样本取模，缩短临界区(关中断)时长；
      // 可用样本不足时(欠载)在尾部补零，语义与逐样本版本完全一致。
      portENTER_CRITICAL(&aec_reference_ring_buffer_lock);
      {
        const uint32_t ring = AV_ESP_AEC_REFERENCE_RING_SAMPLES;
        const uint32_t tail = aec_reference_ring_buffer_tail;
        const uint32_t avail = (aec_reference_ring_buffer_head + ring - tail) % ring;
        const uint32_t want = (uint32_t)chunk_samples;
        const uint32_t n_copy = (avail < want) ? avail : want;
        uint32_t first = ring - tail;  // tail 到环尾的连续样本数
        if (first > n_copy) {
          first = n_copy;
        }
        if (first > 0) {
          memcpy(aec_reference_buffer, &aec_reference_ring_buffer[tail], first * sizeof(int16_t));
        }
        if (n_copy > first) {  // 跨环回绕的第二段
          memcpy(&aec_reference_buffer[first], &aec_reference_ring_buffer[0],
                 (n_copy - first) * sizeof(int16_t));
        }
        aec_reference_ring_buffer_tail = (tail + n_copy) % ring;
        if (n_copy < want) {  // 欠载：剩余部分补零
          memset(&aec_reference_buffer[n_copy], 0, (want - n_copy) * sizeof(int16_t));
        }
      }
      portEXIT_CRITICAL(&aec_reference_ring_buffer_lock);
      // 回声消除：mic/ref 分离输入，输出为去回声后的近端单声道
      aec_process(aec_handle, aec_microphone_buffer, aec_reference_buffer, aec_output_buffer);
      const int output_samples = chunk_samples;

      // 按 20ms 组装上抛帧，保持与上层 MIC_FRAME_MS 口径一致
      for (int i = 0; i < output_samples; ++i) {
        capture_mono_buffer[aec_frame_accumulated++] = aec_output_buffer[i];
        if (aec_frame_accumulated >= output_frame_samples) {
          audio_device_callback_t cb = audio_callback;
          void *user_data = audio_callback_context;
          if (cb && audio_started) {
            cb((const uint8_t *)capture_mono_buffer, MIC_FRAME_BYTES, user_data);
            frame_count++;
            if (frame_count == 1 || frame_count % 500 == 0) {
              ESP_LOGI(TAG, "captured AEC frames=%u", (unsigned)frame_count);
            }
          }
          aec_frame_accumulated = 0;
        }
      }
    }

    audio_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
    return;
  }

  // AEC 不可用，单声道直采直传 ----
  while (audio_task_running) {
    size_t bytes_read = 0;
    esp_err_t err =
        i2s_channel_read(i2s_microphone_channel, capture_mono_buffer, AV_ESP_CAPTURE_FRAME_BYTES,
                         &bytes_read, AV_ESP_AUDIO_READ_TIMEOUT_MS);
    if (!audio_task_running) {
      break;
    }
    if (err == ESP_ERR_TIMEOUT) {
      continue;
    }
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "i2s read failed: %s", esp_err_to_name(err));
      rtos_delay_milliseconds(10);
      continue;
    }
    if (bytes_read != AV_ESP_CAPTURE_FRAME_BYTES) {
      ESP_LOGW(TAG, "drop partial audio frame: %u/%u", (unsigned)bytes_read,
               (unsigned)AV_ESP_CAPTURE_FRAME_BYTES);
      continue;
    }

    // 端到端单声道：I2S 直接读出单声道数据，无需下混。

    audio_device_callback_t cb = audio_callback;
    void *user_data = audio_callback_context;
    if (cb && audio_started) {
      cb((const uint8_t *)capture_mono_buffer, MIC_FRAME_BYTES, user_data);
      frame_count++;
      if (frame_count == 1 || frame_count % 500 == 0) {
        ESP_LOGI(TAG, "captured audio frames=%u", (unsigned)frame_count);
      }
    }
  }

  audio_task_handle = NULL;
  vTaskDeleteWithCaps(NULL);
}

// ==================== I2S 初始化（TDM） ====================

static esp_err_t _i2s_open(void) {
  if (i2s_speaker_channel || i2s_microphone_channel) {
    return ESP_OK;
  }

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AV_ESP_I2S_PORT, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 6;
  chan_cfg.dma_frame_num = MIC_SAMPLE_RATE * MIC_FRAME_MS / 1000;
  chan_cfg.auto_clear = true;

  esp_err_t err = i2s_new_channel(&chan_cfg, &i2s_speaker_channel, &i2s_microphone_channel);
  if (err != ESP_OK) {
    i2s_speaker_channel = NULL;
    i2s_microphone_channel = NULL;
    return err;
  }

  // ESP32-S3: I2S TDM 模式
  i2s_tdm_config_t tdm_cfg = {
      .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, AV_ESP_I2S_TDM_SLOT_MASK),
      .clk_cfg =
          {
              .clk_src = I2S_CLK_SRC_DEFAULT,
              .sample_rate_hz = MIC_SAMPLE_RATE,
              .mclk_multiple = I2S_MCLK_MULTIPLE_256,
          },
      .gpio_cfg =
          {
              .mclk = AV_ESP_I2S_MCLK_GPIO,
              .bclk = AV_ESP_I2S_BCLK_GPIO,
              .ws = AV_ESP_I2S_WS_GPIO,
              .dout = AV_ESP_I2S_DOUT_GPIO,
              .din = AV_ESP_I2S_DIN_GPIO,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };

  if (i2s_speaker_channel) {
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(i2s_speaker_channel, &tdm_cfg), TAG,
                        "tx tdm init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(i2s_speaker_channel), TAG, "tx enable");
  }
  if (i2s_microphone_channel) {
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(i2s_microphone_channel, &tdm_cfg), TAG,
                        "rx tdm init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(i2s_microphone_channel), TAG, "rx enable");
  }

  audio_codec_i2s_cfg_t i2s_cfg = {
      .port = AV_ESP_I2S_PORT,
      .rx_handle = i2s_microphone_channel,
      .tx_handle = i2s_speaker_channel,
  };
  i2s_data_interface = audio_codec_new_i2s_data(&i2s_cfg);
  ESP_RETURN_ON_FALSE(i2s_data_interface, ESP_ERR_NO_MEM, TAG, "i2s data_if alloc");

  return ESP_OK;
}

// ==================== 录音 Codec（ES7210） ====================

static esp_err_t _microphone_open(void) {
  if (microphone_codec_device) {
    return ESP_OK;
  }

  audio_codec_i2c_cfg_t i2c_cfg = {
      .port = AV_ESP_I2C_PORT,
      .addr = ES7210_CODEC_DEFAULT_ADDR,
      .bus_handle = audio_i2c_bus,
  };
  microphone_control_interface = audio_codec_new_i2c_ctrl(&i2c_cfg);
  ESP_RETURN_ON_FALSE(microphone_control_interface, ESP_ERR_NO_MEM, TAG,
                      "record ctrl_if alloc fail");

  es7210_codec_cfg_t es7210_cfg = {
      .ctrl_if = microphone_control_interface,
      .master_mode = false,
      .mic_selected = AV_ESP_ES7210_MIC_SELECTED,
      .mclk_src = ES7210_MCLK_FROM_PAD,
      .mclk_div = I2S_MCLK_MULTIPLE_256,
  };
  microphone_codec_interface = es7210_codec_new(&es7210_cfg);
  ESP_RETURN_ON_FALSE(microphone_codec_interface, ESP_ERR_NO_MEM, TAG,
                      "record codec_if alloc fail");

  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_IN,
      .codec_if = microphone_codec_interface,
      .data_if = i2s_data_interface,
  };
  microphone_codec_device = esp_codec_dev_new(&dev_cfg);
  ESP_RETURN_ON_FALSE(microphone_codec_device, ESP_ERR_NO_MEM, TAG, "record codec_dev alloc fail");

  esp_codec_dev_sample_info_t sample_cfg = {
      .bits_per_sample = 16,
      .channel = AV_ESP_CODEC_CHANNELS,
      .channel_mask = AV_ESP_ES7210_MIC_SELECTED,
      .sample_rate = MIC_SAMPLE_RATE,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  ESP_RETURN_ON_FALSE(esp_codec_dev_open(microphone_codec_device, &sample_cfg) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, TAG, "record codec_dev open fail");
  ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(microphone_codec_device,
                                                AV_ESP_ES7210_MIC_GAIN_DB) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, TAG, "set mic gain fail");

  ESP_LOGI(TAG, "ES7210 mic codec opened: sr=%d ch=%d", MIC_SAMPLE_RATE, AV_ESP_CODEC_CHANNELS);
  return ESP_OK;
}
