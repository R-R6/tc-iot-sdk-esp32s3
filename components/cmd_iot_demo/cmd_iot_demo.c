// Copyright (c) 2026 Tencent. All rights reserved.

// ESP32P4 IoT demo 统一命令入口：iot_video + iot_aitalk。
// 参考 platform/bk7258/solution/ap/ap_main.c 的组织方式。

#include "cmd_iot_demo.h"

#include <string.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_demo_config.h"
#ifdef IOT_API_COVERAGE
#include "api_coverage.h"
#endif
#if BUILD_IOT_AV
#include "iot_demo.h"
#include "tc_iot_av.h"
#endif
#if BUILD_IOT_AITALK
#include "aitalk_demo.h"
#endif

static const char *TAG = "cmd_iot_demo";

// ==================== WiFi 等待 ====================

#if BUILD_IOT_AV || BUILD_IOT_AITALK
static void _wait_wifi_ready(int max_seconds) {
  ESP_LOGI(TAG, "Waiting for WiFi...");
  for (int i = 0; i < max_seconds * 2; i++) {
    esp_netif_ip_info_t ip_info;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
      ESP_LOGI(TAG, "WiFi ready, IP: " IPSTR, IP2STR(&ip_info.ip));
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  ESP_LOGW(TAG, "WiFi not connected after %ds, proceeding anyway", max_seconds);
}
#endif  // BUILD_IOT_AV || BUILD_IOT_AITALK

// ==================== iot_video demo ====================

#if BUILD_IOT_AV
static volatile bool s_iot_started = false;
static char s_iot_product_id[64] = "";
static char s_iot_device_id[64] = "";
static char s_iot_device_secret[128] = "";
static char s_iot_region[32] = "";

static void _iot_start_thread(void *arg) {
  (void)arg;
  _wait_wifi_ready(30);

  iot_demo_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.product_id = s_iot_product_id;
  cfg.device_id = s_iot_device_id;
  cfg.device_secret = s_iot_device_secret;
  cfg.region = s_iot_region;

  if (iot_demo_start(&cfg) != 0) {
    ESP_LOGE(TAG, "iot_demo_start failed");
    s_iot_started = false;
  }

  while (s_iot_started) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  vTaskDelete(NULL);
}

static struct {
  struct arg_str *product_id;
  struct arg_str *device_id;
  struct arg_str *secret;
  struct arg_str *region;
  struct arg_end *end;
} s_start_args;

static int _cmd_iot_start(int argc, char **argv) {
  if (s_iot_started) {
    ESP_LOGW(TAG, "IoT demo already started");
    return 0;
  }

  int nerrors = arg_parse(argc, argv, (void **)&s_start_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, s_start_args.end, argv[0]);
    return 1;
  }

  if (s_start_args.product_id->count > 0) {
    strlcpy(s_iot_product_id, s_start_args.product_id->sval[0], sizeof(s_iot_product_id));
  } else {
    strlcpy(s_iot_product_id, IOT_PRODUCT_ID, sizeof(s_iot_product_id));
  }

  if (s_start_args.device_id->count > 0) {
    strlcpy(s_iot_device_id, s_start_args.device_id->sval[0], sizeof(s_iot_device_id));
  } else {
    strlcpy(s_iot_device_id, IOT_DEVICE_ID, sizeof(s_iot_device_id));
  }

  if (s_start_args.secret->count > 0) {
    strlcpy(s_iot_device_secret, s_start_args.secret->sval[0], sizeof(s_iot_device_secret));
  } else {
    strlcpy(s_iot_device_secret, IOT_DEVICE_SECRET, sizeof(s_iot_device_secret));
  }

  if (s_start_args.region->count > 0) {
    strlcpy(s_iot_region, s_start_args.region->sval[0], sizeof(s_iot_region));
  } else {
    strlcpy(s_iot_region, IOT_REGION, sizeof(s_iot_region));
  }

  s_iot_started = true;
  xTaskCreate(_iot_start_thread, "iot_demo", 32 * 1024, NULL, 5, NULL);
  ESP_LOGI(TAG, "IoT demo starting in background...");
  return 0;
}

static int _cmd_iot_stop(int argc, char **argv) {
  (void)argc;
  (void)argv;
  s_iot_started = false;
  iot_demo_stop();
  ESP_LOGI(TAG, "IoT demo stopped");
  return 0;
}

// ==================== iot_call / accept / reject / hangup ====================

static struct {
  struct arg_str *user_id;
  struct arg_int *content;
  struct arg_end *end;
} s_call_args;

static int _cmd_iot_call(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&s_call_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, s_call_args.end, argv[0]);
    return 1;
  }
  tc_iot_media_content_e media = TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO;
  if (s_call_args.content->count > 0 && s_call_args.content->ival[0] == 1) {
    media = TC_IOT_MEDIA_CONTENT_AUDIO;
  }
  return iot_demo_call(s_call_args.user_id->sval[0], media, TC_IOT_VIDEO_QUALITY_SD);
}

static struct {
  struct arg_str *user_id;
  struct arg_end *end;
} s_user_args;

static int _cmd_iot_accept(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&s_user_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, s_user_args.end, argv[0]);
    return 1;
  }
  return iot_demo_accept(s_user_args.user_id->sval[0], TC_IOT_MEDIA_CONTENT_AUDIO_VIDEO, TC_IOT_VIDEO_QUALITY_SD);
}

static int _cmd_iot_reject(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&s_user_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, s_user_args.end, argv[0]);
    return 1;
  }
  return iot_demo_reject(s_user_args.user_id->sval[0]);
}

static int _cmd_iot_hangup(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&s_user_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, s_user_args.end, argv[0]);
    return 1;
  }
  return iot_demo_hangup(s_user_args.user_id->sval[0]);
}
#endif  // BUILD_IOT_AV

// ==================== iot_aitalk ====================

#if BUILD_IOT_AITALK
static volatile bool s_aitalk_started = false;
static char s_aitalk_product_id[64] = "";
static char s_aitalk_device_id[64] = "";
static char s_aitalk_device_secret[128] = "";
static char s_aitalk_region[32] = "";
static char s_aitalk_bot_id[32] = "";

static void _iot_aitalk_thread(void *arg) {
  (void)arg;
  _wait_wifi_ready(30);

  aitalk_demo_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.product_id = s_aitalk_product_id;
  cfg.device_id = s_aitalk_device_id;
  cfg.device_secret = s_aitalk_device_secret;
  cfg.region = s_aitalk_region;
  cfg.bot_id = s_aitalk_bot_id;

  if (aitalk_demo_start(&cfg) != 0) {
    ESP_LOGE(TAG, "aitalk_demo_start failed");
    s_aitalk_started = false;
    vTaskDelete(NULL);
    return;
  }

  while (s_aitalk_started) {
    if (aitalk_demo_has_error()) {
      ESP_LOGW(TAG, "AITalk error detected, stopping");
      aitalk_demo_stop();
      s_aitalk_started = false;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  vTaskDelete(NULL);
}

static struct {
  struct arg_str *product_id;
  struct arg_str *device_id;
  struct arg_str *secret;
  struct arg_str *region;
  struct arg_str *bot_id;
  struct arg_end *end;
} s_aitalk_args;

static int _cmd_iot_aitalk(int argc, char **argv) {
  if (s_aitalk_started) {
    ESP_LOGW(TAG, "AITalk already started");
    return 0;
  }

  int nerrors = arg_parse(argc, argv, (void **)&s_aitalk_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, s_aitalk_args.end, argv[0]);
    return 1;
  }

  if (s_aitalk_args.bot_id->count > 0) {
    strlcpy(s_aitalk_bot_id, s_aitalk_args.bot_id->sval[0], sizeof(s_aitalk_bot_id));
  } else {
    strlcpy(s_aitalk_bot_id, IOT_AITALK_BOT_ID, sizeof(s_aitalk_bot_id));
  }

  if (s_aitalk_args.product_id->count > 0) {
    strlcpy(s_aitalk_product_id, s_aitalk_args.product_id->sval[0], sizeof(s_aitalk_product_id));
  } else {
    strlcpy(s_aitalk_product_id, IOT_PRODUCT_ID, sizeof(s_aitalk_product_id));
  }

  if (s_aitalk_args.device_id->count > 0) {
    strlcpy(s_aitalk_device_id, s_aitalk_args.device_id->sval[0], sizeof(s_aitalk_device_id));
  } else {
    strlcpy(s_aitalk_device_id, IOT_DEVICE_ID, sizeof(s_aitalk_device_id));
  }

  if (s_aitalk_args.secret->count > 0) {
    strlcpy(s_aitalk_device_secret, s_aitalk_args.secret->sval[0], sizeof(s_aitalk_device_secret));
  } else {
    strlcpy(s_aitalk_device_secret, IOT_DEVICE_SECRET, sizeof(s_aitalk_device_secret));
  }

  if (s_aitalk_args.region->count > 0) {
    strlcpy(s_aitalk_region, s_aitalk_args.region->sval[0], sizeof(s_aitalk_region));
  } else {
    strlcpy(s_aitalk_region, IOT_REGION, sizeof(s_aitalk_region));
  }

  s_aitalk_started = true;
  xTaskCreate(_iot_aitalk_thread, "iot_aitalk", 32 * 1024, NULL, 5, NULL);
  ESP_LOGI(TAG, "AITalk starting in background (bot=%s)...", s_aitalk_bot_id);
  return 0;
}

static int _cmd_iot_aitalk_stop(int argc, char **argv) {
  (void)argc;
  (void)argv;
  s_aitalk_started = false;
  aitalk_demo_stop();
  ESP_LOGI(TAG, "AITalk stopped");
  return 0;
}
#endif  // BUILD_IOT_AITALK

// ==================== 命令注册 ====================

void register_iot_demo_commands(void) {
#ifdef IOT_API_COVERAGE
  iot_api_coverage_reference_all();
#endif
#if BUILD_IOT_AV
  // iot_start
  s_start_args.product_id = arg_str0("p", NULL, "<pid>", "Product ID (default: config)");
  s_start_args.device_id = arg_str0("d", NULL, "<did>", "Device ID (default: config)");
  s_start_args.secret = arg_str0("s", NULL, "<secret>", "Device secret (default: config)");
  s_start_args.region = arg_str0("r", NULL, "<region>", IOT_REGION_HELP);
  s_start_args.end = arg_end(4);

  const esp_console_cmd_t start_cmd = {
      .command = "iot_start",
      .help = "Start IoT video demo in background",
      .hint = NULL,
      .func = &_cmd_iot_start,
      .argtable = &s_start_args,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&start_cmd));

  // iot_stop
  const esp_console_cmd_t stop_cmd = {
      .command = "iot_stop",
      .help = "Stop IoT video demo",
      .hint = NULL,
      .func = &_cmd_iot_stop,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));

  // iot_call
  s_call_args.user_id = arg_str1("u", NULL, "<user_id>", "Target user ID");
  s_call_args.content = arg_int0("c", NULL, "<1=audio>", "Media content");
  s_call_args.end = arg_end(2);

  const esp_console_cmd_t call_cmd = {
      .command = "iot_call",
      .help = "Call a user",
      .hint = NULL,
      .func = &_cmd_iot_call,
      .argtable = &s_call_args,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&call_cmd));

  // iot_accept / reject / hangup
  s_user_args.user_id = arg_str1("u", NULL, "<user_id>", "User ID");
  s_user_args.end = arg_end(1);

  const esp_console_cmd_t accept_cmd = {
      .command = "iot_accept",
      .help = "Accept incoming call",
      .hint = NULL,
      .func = &_cmd_iot_accept,
      .argtable = &s_user_args,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&accept_cmd));

  const esp_console_cmd_t reject_cmd = {
      .command = "iot_reject",
      .help = "Reject incoming call",
      .hint = NULL,
      .func = &_cmd_iot_reject,
      .argtable = &s_user_args,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&reject_cmd));

  const esp_console_cmd_t hangup_cmd = {
      .command = "iot_hangup",
      .help = "Hangup active call",
      .hint = NULL,
      .func = &_cmd_iot_hangup,
      .argtable = &s_user_args,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&hangup_cmd));
#endif  // BUILD_IOT_AV

#if BUILD_IOT_AITALK
  // iot_aitalk
  s_aitalk_args.product_id = arg_str0("p", NULL, "<pid>", "Product ID (default: config)");
  s_aitalk_args.device_id = arg_str0("d", NULL, "<did>", "Device ID (default: config)");
  s_aitalk_args.secret = arg_str0("s", NULL, "<secret>", "Device secret (default: config)");
  s_aitalk_args.region = arg_str0("r", NULL, "<region>", IOT_REGION_HELP);
  s_aitalk_args.bot_id = arg_str0("b", NULL, "<bot_id>", "Bot ID (default: config)");
  s_aitalk_args.end = arg_end(5);

  const esp_console_cmd_t aitalk_cmd = {
      .command = "iot_aitalk",
      .help = "Start AITalk in background",
      .hint = NULL,
      .func = &_cmd_iot_aitalk,
      .argtable = &s_aitalk_args,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&aitalk_cmd));

  // iot_aitalk_stop
  const esp_console_cmd_t aitalk_stop_cmd = {
      .command = "iot_aitalk_stop",
      .help = "Stop AITalk",
      .hint = NULL,
      .func = &_cmd_iot_aitalk_stop,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&aitalk_stop_cmd));
#endif  // BUILD_IOT_AITALK
}
