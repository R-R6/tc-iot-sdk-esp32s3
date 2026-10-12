/*****************************************************************************
 * Copyright (C) 2026 THL A29 Limited, a Tencent company. All rights reserved.
 *
 * Licensed under the MIT License (the "License");
 *****************************************************************************/

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "decoder/esp_audio_dec.h"
#include "decoder/esp_audio_dec_default.h"
#include "decoder/impl/esp_aac_dec.h"
#if BUILD_OPUS
#  include "decoder/impl/esp_opus_dec.h"
#endif
#include "encoder/esp_audio_enc.h"
#include "encoder/esp_audio_enc_default.h"
#include "encoder/impl/esp_aac_enc.h"
#if BUILD_OPUS
#  include "encoder/impl/esp_opus_enc.h"
#endif
#include "esp_audio_types.h"
#include "esp_log.h"
#include "hal/tc_hal_audio.h"

#define TAG "HAL_Audio_ESP"

static bool s_codec_registered = false;

static void _ensure_codec_registered(void) {
  if (!s_codec_registered) {
    esp_audio_enc_register_default();
    esp_audio_dec_register_default();
    s_codec_registered = true;
    ESP_LOGI(TAG, "Audio codecs registered");
  }
}

#if BUILD_OPUS
// --- Opus Encoder ---

typedef struct {
  esp_audio_enc_handle_t handle;
  uint32_t sample_rate;
  uint32_t channels;
} hal_opus_enc_t;

void *HAL_CreateOpusEncoder(uint32_t sample_rate, uint32_t channels, uint32_t bitrate) {
  _ensure_codec_registered();

  esp_opus_enc_config_t opus_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
  opus_cfg.sample_rate = sample_rate;
  opus_cfg.channel = channels;
  opus_cfg.bitrate = bitrate;
  opus_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;

  esp_audio_enc_config_t enc_cfg = {
      .type = ESP_AUDIO_TYPE_OPUS,
      .cfg = &opus_cfg,
      .cfg_sz = sizeof(opus_cfg),
  };

  hal_opus_enc_t *enc = calloc(1, sizeof(hal_opus_enc_t));
  if (!enc) {
    return NULL;
  }

  esp_audio_err_t ret = esp_audio_enc_open(&enc_cfg, &enc->handle);
  if (ret != ESP_AUDIO_ERR_OK || !enc->handle) {
    ESP_LOGE(TAG, "Failed to create Opus encoder");
    free(enc);
    return NULL;
  }

  enc->sample_rate = sample_rate;
  enc->channels = channels;
  return enc;
}

int32_t HAL_OpusEncode(void *encoder, const short *pcm, uint32_t frame_size, unsigned char *data,
                       uint32_t max_data_bytes) {
  if (!encoder || !pcm || !data) {
    return -1;
  }

  hal_opus_enc_t *enc = (hal_opus_enc_t *)encoder;
  uint32_t pcm_bytes = frame_size * enc->channels * sizeof(short);

  esp_audio_enc_in_frame_t in = {
      .buffer = (uint8_t *)pcm,
      .len = pcm_bytes,
  };
  esp_audio_enc_out_frame_t out = {
      .buffer = data,
      .len = max_data_bytes,
  };

  esp_audio_err_t ret = esp_audio_enc_process(enc->handle, &in, &out);
  if (ret != ESP_AUDIO_ERR_OK) {
    return -1;
  }

  return (int32_t)out.encoded_bytes;
}

void HAL_DestroyOpusEncoder(void *encoder) {
  if (!encoder) {
    return;
  }
  hal_opus_enc_t *enc = (hal_opus_enc_t *)encoder;
  if (enc->handle) {
    esp_audio_enc_close(enc->handle);
  }
  free(enc);
}

// --- Opus Decoder ---

typedef struct {
  esp_audio_dec_handle_t handle;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t frame_duration;
} hal_opus_dec_t;

HALOpusDecoder *HAL_CreateOpusDecoder(uint32_t sample_rate, uint32_t channels,
                                      uint32_t frame_duration) {
  _ensure_codec_registered();

  // 毫秒转 esp_opus_dec 枚举值
  esp_opus_dec_frame_duration_t dur_enum;
  uint32_t dur_ms = (frame_duration > 0) ? frame_duration : 20;
  switch (dur_ms) {
    case 2:
      dur_enum = ESP_OPUS_DEC_FRAME_DURATION_2_5_MS;
      break;
    case 5:
      dur_enum = ESP_OPUS_DEC_FRAME_DURATION_5_MS;
      break;
    case 10:
      dur_enum = ESP_OPUS_DEC_FRAME_DURATION_10_MS;
      break;
    case 20:
      dur_enum = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
      break;
    case 40:
      dur_enum = ESP_OPUS_DEC_FRAME_DURATION_40_MS;
      break;
    case 60:
      dur_enum = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
      break;
    case 80:
      dur_enum = ESP_OPUS_DEC_FRAME_DURATION_80_MS;
      break;
    case 100:
      dur_enum = ESP_OPUS_DEC_FRAME_DURATION_100_MS;
      break;
    case 120:
      dur_enum = ESP_OPUS_DEC_FRAME_DURATION_120_MS;
      break;
    default:
      dur_enum = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
      break;
  }

  esp_opus_dec_cfg_t opus_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
  opus_cfg.sample_rate = sample_rate;
  opus_cfg.channel = channels;
  opus_cfg.frame_duration = dur_enum;

  esp_audio_dec_cfg_t dec_cfg = {
      .type = ESP_AUDIO_TYPE_OPUS,
      .cfg = &opus_cfg,
      .cfg_sz = sizeof(opus_cfg),
  };

  hal_opus_dec_t *dec = calloc(1, sizeof(hal_opus_dec_t));
  if (!dec) {
    return NULL;
  }

  esp_audio_err_t ret = esp_audio_dec_open(&dec_cfg, &dec->handle);
  if (ret != ESP_AUDIO_ERR_OK || !dec->handle) {
    ESP_LOGE(TAG, "Failed to create Opus decoder");
    free(dec);
    return NULL;
  }

  dec->sample_rate = sample_rate;
  dec->channels = channels;
  dec->frame_duration = dur_ms;
  ESP_LOGI(TAG, "Opus decoder created: sr=%lu ch=%lu dur=%lu(enum=%d)",
           (unsigned long)dec->sample_rate, (unsigned long)dec->channels, (unsigned long)dur_ms,
           (int)dur_enum);
  return (HALOpusDecoder *)dec;
}

HALAudioDecodeError HAL_OpusDecode(HALOpusDecoder *decoder, const uint8_t *opus_data,
                                   size_t opus_data_len, uint8_t *pcm_data, size_t *pcm_data_len) {
  if (!decoder || !opus_data || !pcm_data || !pcm_data_len) {
    return HAL_DECODE_FAILED;
  }

  hal_opus_dec_t *dec = (hal_opus_dec_t *)decoder;

  esp_audio_dec_in_raw_t in = {
      .buffer = (uint8_t *)opus_data,
      .len = (uint32_t)opus_data_len,
  };
  esp_audio_dec_out_frame_t out = {
      .buffer = pcm_data,
      .len = (uint32_t)*pcm_data_len,
  };

  esp_audio_err_t ret = esp_audio_dec_process(dec->handle, &in, &out);
  if (ret != ESP_AUDIO_ERR_OK) {
    if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
      *pcm_data_len = out.needed_size;
      return HAL_DECODE_BUFFER_NOT_ENOUGH;
    }
    return HAL_DECODE_FAILED;
  }

  *pcm_data_len = out.decoded_size;
  return HAL_DECODE_OK;
}

void HAL_DestroyOpusDecoder(HALOpusDecoder *decoder) {
  if (!decoder) {
    return;
  }
  hal_opus_dec_t *dec = (hal_opus_dec_t *)decoder;
  if (dec->handle) {
    esp_audio_dec_close(dec->handle);
  }
  free(dec);
}
#else
void *HAL_CreateOpusEncoder(uint32_t sample_rate, uint32_t channels, uint32_t bitrate) {
  (void)sample_rate;
  (void)channels;
  (void)bitrate;
  ESP_LOGW(TAG, "HAL_CreateOpusEncoder: blocked BUILD_OPUS=0");
  return NULL;
}

int32_t HAL_OpusEncode(void *encoder, const short *pcm, uint32_t frame_size, unsigned char *data,
                       uint32_t max_data_bytes) {
  (void)encoder;
  (void)pcm;
  (void)frame_size;
  (void)data;
  (void)max_data_bytes;
  ESP_LOGW(TAG, "HAL_OpusEncode: blocked BUILD_OPUS=0");
  return -1;
}

void HAL_DestroyOpusEncoder(void *encoder) {
  (void)encoder;
}

HALOpusDecoder *HAL_CreateOpusDecoder(uint32_t sample_rate, uint32_t channels,
                                      uint32_t frame_duration) {
  (void)sample_rate;
  (void)channels;
  (void)frame_duration;
  ESP_LOGW(TAG, "HAL_CreateOpusDecoder: blocked BUILD_OPUS=0");
  return NULL;
}

HALAudioDecodeError HAL_OpusDecode(HALOpusDecoder *decoder, const uint8_t *opus_data,
                                   size_t opus_data_len, uint8_t *pcm_data, size_t *pcm_data_len) {
  (void)decoder;
  (void)opus_data;
  (void)opus_data_len;
  (void)pcm_data;
  if (pcm_data_len) {
    *pcm_data_len = 0;
  }
  ESP_LOGW(TAG, "HAL_OpusDecode: blocked BUILD_OPUS=0");
  return HAL_DECODE_FAILED;
}

void HAL_DestroyOpusDecoder(HALOpusDecoder *decoder) {
  (void)decoder;
}
#endif

//=============================================================================
// AAC Decoder
//=============================================================================

// --- AAC Encoder ---

typedef struct {
  esp_audio_enc_handle_t handle;
  uint32_t sample_rate;
  uint32_t channels;
} hal_aac_enc_t;

HALAacEncoder *HAL_CreateAacEncoder(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps,
                                    uint32_t *p_in_size, uint32_t *p_out_size) {
  _ensure_codec_registered();

  esp_aac_enc_config_t aac_cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
  aac_cfg.sample_rate = sample_rate;
  aac_cfg.channel = channels;
  aac_cfg.bitrate = bitrate_bps;
  aac_cfg.adts_used = false;

  esp_audio_enc_config_t enc_cfg = {
      .type = ESP_AUDIO_TYPE_AAC,
      .cfg = &aac_cfg,
      .cfg_sz = sizeof(aac_cfg),
  };

  // 查询帧大小
  esp_audio_enc_frame_info_t frame_info = {0};
  esp_audio_enc_get_frame_info_by_cfg(&enc_cfg, &frame_info);
  if (p_in_size) {
    *p_in_size = frame_info.in_frame_size;
  }
  if (p_out_size) {
    *p_out_size = frame_info.out_frame_size;
  }

  hal_aac_enc_t *enc = calloc(1, sizeof(hal_aac_enc_t));
  if (!enc) {
    return NULL;
  }

  esp_audio_err_t ret = esp_audio_enc_open(&enc_cfg, &enc->handle);
  if (ret != ESP_AUDIO_ERR_OK || !enc->handle) {
    ESP_LOGE(TAG, "Failed to create AAC encoder: %d", ret);
    free(enc);
    if (p_in_size) {
      *p_in_size = 0;
    }
    if (p_out_size) {
      *p_out_size = 0;
    }
    return NULL;
  }

  enc->sample_rate = sample_rate;
  enc->channels = channels;
  ESP_LOGI(TAG, "AAC encoder created: sr=%lu ch=%lu br=%lu", sample_rate, channels, bitrate_bps);
  return (HALAacEncoder *)enc;
}

int32_t HAL_AacEncode(HALAacEncoder *encoder, const uint8_t *pcm_data, size_t pcm_len,
                      uint8_t *aac_data, size_t aac_len) {
  if (!encoder || !pcm_data || !aac_data) {
    return -1;
  }

  hal_aac_enc_t *enc = (hal_aac_enc_t *)encoder;

  esp_audio_enc_in_frame_t in = {
      .buffer = (uint8_t *)pcm_data,
      .len = pcm_len,
  };
  esp_audio_enc_out_frame_t out = {
      .buffer = aac_data,
      .len = aac_len,
  };

  esp_audio_err_t ret = esp_audio_enc_process(enc->handle, &in, &out);
  if (ret != ESP_AUDIO_ERR_OK) {
    return -1;
  }

  return (int32_t)out.encoded_bytes;
}

int32_t HAL_AacEncoderSetBitrate(HALAacEncoder *encoder, uint32_t bitrate_bps) {
  if (!encoder) {
    return -1;
  }
  hal_aac_enc_t *enc = (hal_aac_enc_t *)encoder;
  return (int32_t)esp_audio_enc_set_bitrate(enc->handle, bitrate_bps);
}

void HAL_DestroyAacEncoder(HALAacEncoder *encoder) {
  if (!encoder) {
    return;
  }
  hal_aac_enc_t *enc = (hal_aac_enc_t *)encoder;
  if (enc->handle) {
    esp_audio_enc_close(enc->handle);
  }
  free(enc);
}

// --- AAC Decoder ---

struct _HALAacDecoder {
  esp_audio_dec_handle_t decoder;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t frame_duration;  // ms
  uint32_t frame_size;      // samples per output frame
  unsigned char *internal_buffer;
  uint32_t buffer_size;  // internal buffer capacity (bytes)
  uint32_t buffer_len;   // current valid data length (bytes)
};

HALAacDecoder *HAL_CreateAacDecoder(uint32_t sample_rate, uint32_t channels,
                                    uint32_t frame_duration) {
  _ensure_codec_registered();

  HALAacDecoder *ctx = (HALAacDecoder *)calloc(1, sizeof(HALAacDecoder));
  if (ctx == NULL) {
    ESP_LOGE(TAG, "AAC decoder: calloc failed");
    return NULL;
  }

  esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
  aac_cfg.sample_rate = sample_rate;
  aac_cfg.channel = (uint8_t)channels;
  aac_cfg.no_adts_header = true;

  esp_audio_dec_cfg_t dec_cfg = {
      .type = ESP_AUDIO_TYPE_AAC,
      .cfg = &aac_cfg,
      .cfg_sz = sizeof(aac_cfg),
  };

  esp_audio_err_t ret = esp_audio_dec_open(&dec_cfg, &ctx->decoder);
  if (ret != ESP_AUDIO_ERR_OK || !ctx->decoder) {
    ESP_LOGE(TAG, "Failed to create AAC decoder: %d", ret);
    free(ctx);
    return NULL;
  }

  ctx->sample_rate = sample_rate;
  ctx->channels = channels;
  ctx->frame_duration = frame_duration;

  if (frame_duration > 0) {
    ctx->frame_size = sample_rate / 1000 * frame_duration;
  } else {
    ctx->frame_size = 1024;
  }

  ctx->buffer_size = 1024 * 2 * channels * sizeof(int16_t);
  ctx->internal_buffer = (unsigned char *)malloc(ctx->buffer_size);
  if (ctx->internal_buffer == NULL) {
    ESP_LOGE(TAG, "AAC decoder: internal_buffer malloc failed");
    esp_audio_dec_close(ctx->decoder);
    free(ctx);
    return NULL;
  }
  ctx->buffer_len = 0;

  ESP_LOGI(TAG, "AAC decoder created: sr=%lu ch=%lu dur=%lu frame_size=%lu",
           (unsigned long)sample_rate, (unsigned long)channels, (unsigned long)frame_duration,
           (unsigned long)ctx->frame_size);
  return ctx;
}

void HAL_DestroyAacDecoder(HALAacDecoder *decoder) {
  if (decoder != NULL) {
    if (decoder->decoder) {
      esp_audio_dec_close(decoder->decoder);
    }
    if (decoder->internal_buffer) {
      free(decoder->internal_buffer);
    }
    free(decoder);
    ESP_LOGI(TAG, "AAC decoder destroyed");
  }
}

HALAudioDecodeError HAL_AacDecode(HALAacDecoder *decoder, const uint8_t *aac_data,
                                  size_t aac_data_len, uint8_t *pcm_data, size_t *pcm_data_len) {
  if (decoder == NULL || aac_data == NULL || pcm_data == NULL || pcm_data_len == NULL) {
    return HAL_DECODE_FAILED;
  }

  size_t output_bytes = decoder->frame_size * decoder->channels * sizeof(int16_t);

  esp_audio_dec_in_raw_t in = {
      .buffer = (uint8_t *)aac_data,
      .len = (uint32_t)aac_data_len,
  };
  esp_audio_dec_out_frame_t out = {
      .buffer = decoder->internal_buffer + decoder->buffer_len,
      .len = decoder->buffer_size - decoder->buffer_len,
  };

  esp_audio_err_t ret = esp_audio_dec_process(decoder->decoder, &in, &out);
  if (ret != ESP_AUDIO_ERR_OK) {
    if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
      *pcm_data_len = out.needed_size;
      return HAL_DECODE_BUFFER_NOT_ENOUGH;
    }
    return HAL_DECODE_FAILED;
  }

  decoder->buffer_len += out.decoded_size;

  if (decoder->buffer_len >= output_bytes) {
    memcpy(pcm_data, decoder->internal_buffer, output_bytes);
    decoder->buffer_len -= output_bytes;
    if (decoder->buffer_len > 0) {
      memmove(decoder->internal_buffer, decoder->internal_buffer + output_bytes,
              decoder->buffer_len);
    }
    *pcm_data_len = output_bytes;
  } else {
    size_t available = decoder->buffer_len;
    memcpy(pcm_data, decoder->internal_buffer, available);
    decoder->buffer_len = 0;
    *pcm_data_len = available;
  }

  return HAL_DECODE_OK;
}

// --- G722 Codec (stub - not supported on ESP platform) ---

HALG722Decoder *HAL_CreateG722Decoder(uint32_t sample_rate, uint32_t channels,
                                      uint32_t frame_duration) {
  (void)sample_rate;
  (void)channels;
  (void)frame_duration;
  ESP_LOGW(TAG, "G722 decoder not implemented on this platform");
  return NULL;
}

HALAudioDecodeError HAL_G722Decode(HALG722Decoder *decoder, const uint8_t *g722_data,
                                   size_t g722_data_len, uint8_t *pcm_data, size_t *pcm_data_len) {
  (void)decoder;
  (void)g722_data;
  (void)g722_data_len;
  (void)pcm_data;
  (void)pcm_data_len;
  return HAL_DECODE_FAILED;
}

void HAL_DestroyG722Decoder(HALG722Decoder *decoder) {
  (void)decoder;
}

HALG722Encoder *HAL_CreateG722Encoder(uint32_t sample_rate, uint32_t channels, uint32_t bitrate) {
  (void)sample_rate;
  (void)channels;
  (void)bitrate;
  ESP_LOGW(TAG, "G722 encoder not implemented on this platform");
  return NULL;
}

int32_t HAL_G722Encode(HALG722Encoder *encoder, const int16_t *pcm, uint32_t frame_size,
                       uint8_t *data, uint32_t max_data_bytes) {
  (void)encoder;
  (void)pcm;
  (void)frame_size;
  (void)data;
  (void)max_data_bytes;
  return -1;
}

void HAL_DestroyG722Encoder(HALG722Encoder *encoder) {
  (void)encoder;
}
