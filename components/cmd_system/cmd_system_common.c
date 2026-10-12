/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Console example — various system commands

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cmd_system.h"
#include "sdkconfig.h"

#ifdef CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
#define WITH_TASKS_INFO 1
#endif

static const char *TAG = "cmd_system_common";

static void register_free(void);
static void register_heap(void);
static void register_version(void);
static void register_restart(void);
#if WITH_TASKS_INFO
static void register_tasks(void);
#endif
static void register_log_level(void);

void register_system_common(void)
{
    register_free();
    register_heap();
    register_version();
    register_restart();
#if WITH_TASKS_INFO
    register_tasks();
#endif
    register_log_level();
}


/* 'version' command */
static int get_version(int argc, char **argv)
{
    const char *model;
    esp_chip_info_t info;
    uint32_t flash_size;
    esp_chip_info(&info);

    switch(info.model) {
        case CHIP_ESP32:
            model = "ESP32";
            break;
        case CHIP_ESP32S2:
            model = "ESP32-S2";
            break;
        case CHIP_ESP32S3:
            model = "ESP32-S3";
            break;
        case CHIP_ESP32C3:
            model = "ESP32-C3";
            break;
        case CHIP_ESP32H2:
            model = "ESP32-H2";
            break;
        case CHIP_ESP32C2:
            model = "ESP32-C2";
            break;
        case CHIP_ESP32P4:
            model = "ESP32-P4";
            break;
        case CHIP_ESP32C5:
            model = "ESP32-C5";
            break;
        default:
            model = "Unknown";
            break;
    }

    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return 1;
    }
    printf("IDF Version:%s\r\n", esp_get_idf_version());
    printf("Chip info:\r\n");
    printf("\tmodel:%s\r\n", model);
    printf("\tcores:%d\r\n", info.cores);
    printf("\tfeature:%s%s%s%s%"PRIu32"%s\r\n",
           info.features & CHIP_FEATURE_WIFI_BGN ? "/802.11bgn" : "",
           info.features & CHIP_FEATURE_BLE ? "/BLE" : "",
           info.features & CHIP_FEATURE_BT ? "/BT" : "",
           info.features & CHIP_FEATURE_EMB_FLASH ? "/Embedded-Flash:" : "/External-Flash:",
           flash_size / (1024 * 1024), " MB");
    printf("\trevision number:%d\r\n", info.revision);
    return 0;
}

static void register_version(void)
{
    const esp_console_cmd_t cmd = {
        .command = "version",
        .help = "Get version of chip and SDK",
        .hint = NULL,
        .func = &get_version,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/** 'restart' command restarts the program */

static int restart(int argc, char **argv)
{
    ESP_LOGI(TAG, "Restarting");
    esp_restart();
}

static void register_restart(void)
{
    const esp_console_cmd_t cmd = {
        .command = "restart",
        .help = "Software reset of the chip",
        .hint = NULL,
        .func = &restart,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/** 'free' command prints available heap memory */

static void print_caps_info(const char *name, uint32_t caps)
{
    size_t total    = heap_caps_get_total_size(caps);
    size_t free_b   = heap_caps_get_free_size(caps);
    size_t used     = (total > free_b) ? (total - free_b) : 0;
    size_t min_free = heap_caps_get_minimum_free_size(caps);
    size_t largest  = heap_caps_get_largest_free_block(caps);
    float used_pct  = total ? (used * 100.0f / total) : 0.0f;

    printf("[%s]\n", name);
    printf("  Total       : %8u bytes (%7.2f KB)\n", (unsigned)total,    total    / 1024.0f);
    printf("  Used        : %8u bytes (%7.2f KB, %.2f%%)\n", (unsigned)used, used / 1024.0f, used_pct);
    printf("  Free        : %8u bytes (%7.2f KB)\n", (unsigned)free_b,   free_b   / 1024.0f);
    printf("  Min Free    : %8u bytes (%7.2f KB)  (历史最低水位)\n", (unsigned)min_free, min_free / 1024.0f);
    printf("  Largest Blk : %8u bytes (%7.2f KB)\n", (unsigned)largest,  largest  / 1024.0f);
}

static int free_mem(int argc, char **argv)
{
    print_caps_info("Internal RAM (DRAM)", MALLOC_CAP_INTERNAL);
#if CONFIG_SPIRAM
    print_caps_info("External PSRAM",      MALLOC_CAP_SPIRAM);
#else
    printf("[External PSRAM] not enabled (CONFIG_SPIRAM=n)\n");
#endif
    return 0;
}

static void register_free(void)
{
    const esp_console_cmd_t cmd = {
        .command = "free",
        .help = "Get the current size of free heap memory",
        .hint = NULL,
        .func = &free_mem,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

/* 'heap' command prints minimum heap size */
static int heap_size(int argc, char **argv)
{
    uint32_t heap_size = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    printf("min heap size: %"PRIu32"\n", heap_size);
    return 0;
}

static void register_heap(void)
{
    const esp_console_cmd_t heap_cmd = {
        .command = "heap",
        .help = "Get minimum size of free heap memory that was available during program execution",
        .hint = NULL,
        .func = &heap_size,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&heap_cmd) );

}

/** 'tasks' command prints the list of tasks and related information */
#if WITH_TASKS_INFO

static char task_state_to_char(eTaskState s)
{
    switch (s) {
        case eRunning:   return 'X';
        case eReady:     return 'R';
        case eBlocked:   return 'B';
        case eSuspended: return 'S';
        case eDeleted:   return 'D';
        default:         return '?';
    }
}

// 采样窗口（毫秒），用于差分计算实时 CPU 使用率
#define TASKS_CPU_SAMPLE_MS 500

static int tasks_info(int argc, char **argv)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    UBaseType_t n1 = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *s1 = malloc(n1 * sizeof(TaskStatus_t));
    TaskStatus_t *s2 = malloc(n1 * sizeof(TaskStatus_t));
    if (!s1 || !s2) {
        ESP_LOGE(TAG, "alloc failed");
        free(s1); free(s2);
        return 1;
    }

    uint32_t rt1 = 0, rt2 = 0;
    UBaseType_t cnt1 = uxTaskGetSystemState(s1, n1, &rt1);
    vTaskDelay(pdMS_TO_TICKS(TASKS_CPU_SAMPLE_MS));
    UBaseType_t cnt2 = uxTaskGetSystemState(s2, n1, &rt2);

    uint32_t total_delta = rt2 - rt1;
    uint32_t denom = total_delta * portNUM_PROCESSORS;
#else
    UBaseType_t cnt2 = uxTaskGetNumberOfTasks();
    TaskStatus_t *s2 = malloc(cnt2 * sizeof(TaskStatus_t));
    if (!s2) { ESP_LOGE(TAG, "alloc failed"); return 1; }
    cnt2 = uxTaskGetSystemState(s2, cnt2, NULL);
    uint32_t denom = 0;
#endif

    printf("%-16s %-6s %-4s %-6s %-5s"
#ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
           " %-4s"
#endif
           " %12s %7s\n",
           "Task Name", "Status", "Prio", "HWM", "Task#",
#ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
           "Aff",
#endif
           "RtDelta", "CPU%");

    for (UBaseType_t i = 0; i < cnt2; i++) {
        TaskStatus_t *t = &s2[i];
        uint32_t delta = t->ulRunTimeCounter;
        char cpu_buf[16];

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        // 在第一次采样中按 xHandle 找到对应任务，计算差分
        uint32_t prev = 0;
        for (UBaseType_t j = 0; j < cnt1; j++) {
            if (s1[j].xHandle == t->xHandle) {
                prev = s1[j].ulRunTimeCounter;
                break;
            }
        }
        delta = (t->ulRunTimeCounter >= prev) ? (t->ulRunTimeCounter - prev) : t->ulRunTimeCounter;

        if (denom > 0) {
            uint32_t permille = (uint32_t)(((uint64_t)delta * 10000ULL) / denom);
            if (permille > 9999) permille = 9999; // 限幅，避免百分比异常溢出
            snprintf(cpu_buf, sizeof(cpu_buf), "%u.%02u%%",
                     (unsigned)(permille / 100), (unsigned)(permille % 100));
        } else {
            snprintf(cpu_buf, sizeof(cpu_buf), "n/a");
        }
#else
        snprintf(cpu_buf, sizeof(cpu_buf), "n/a");
#endif

#ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        char aff[12];
        BaseType_t core = t->xCoreID;
        if (core == tskNO_AFFINITY) {
            snprintf(aff, sizeof(aff), "ANY");
        } else {
            snprintf(aff, sizeof(aff), "%d", (int)core);
        }
#endif
        printf("%-16s %-6c %-4u %-6u %-5u"
#ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
               " %-4s"
#endif
               " %12" PRIu32 " %7s\n",
               t->pcTaskName,
               task_state_to_char(t->eCurrentState),
               (unsigned)t->uxCurrentPriority,
               (unsigned)t->usStackHighWaterMark,
               (unsigned)t->xTaskNumber,
#ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
               aff,
#endif
               delta,
               cpu_buf);
    }

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    free(s1);
#else
    fputs("[CPU%] Enable CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS in menuconfig.\n", stdout);
#endif
    free(s2);
    return 0;
}

static void register_tasks(void)
{
    const esp_console_cmd_t cmd = {
        .command = "tasks",
        .help = "Get information about running tasks",
        .hint = NULL,
        .func = &tasks_info,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}

#endif // WITH_TASKS_INFO

/** log_level command changes log level via esp_log_level_set */

static struct {
    struct arg_str *tag;
    struct arg_str *level;
    struct arg_end *end;
} log_level_args;

static const char* s_log_level_names[] = {
    "none",
    "error",
    "warn",
    "info",
    "debug",
    "verbose"
};

static int log_level(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &log_level_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, log_level_args.end, argv[0]);
        return 1;
    }
    assert(log_level_args.tag->count == 1);
    assert(log_level_args.level->count == 1);
    const char* tag = log_level_args.tag->sval[0];
    const char* level_str = log_level_args.level->sval[0];
    esp_log_level_t level;
    size_t level_len = strlen(level_str);
    for (level = ESP_LOG_NONE; level <= ESP_LOG_VERBOSE; level++) {
        if (memcmp(level_str, s_log_level_names[level], level_len) == 0) {
            break;
        }
    }
    if (level > ESP_LOG_VERBOSE) {
        printf("Invalid log level '%s', choose from none|error|warn|info|debug|verbose\n", level_str);
        return 1;
    }
    if (level > CONFIG_LOG_MAXIMUM_LEVEL) {
        printf("Can't set log level to %s, max level limited in menuconfig to %s. "
               "Please increase CONFIG_LOG_MAXIMUM_LEVEL in menuconfig.\n",
               s_log_level_names[level], s_log_level_names[CONFIG_LOG_MAXIMUM_LEVEL]);
        return 1;
    }
    esp_log_level_set(tag, level);
    return 0;
}

static void register_log_level(void)
{
    log_level_args.tag = arg_str1(NULL, NULL, "<tag|*>", "Log tag to set the level for, or * to set for all tags");
    log_level_args.level = arg_str1(NULL, NULL, "<none|error|warn|info|debug|verbose>", "Log level to set. Abbreviated words are accepted.");
    log_level_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "log_level",
        .help = "Set log level for all tags or a specific tag.",
        .hint = NULL,
        .func = &log_level,
        .argtable = &log_level_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}
