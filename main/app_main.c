#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "esp_spiffs.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cmd_system.h"
#include "cmd_wifi.h"
#include "cmd_nvs.h"
#include "console_settings.h"
#if TC_PRODUCT_TRTC
#include "cmd_trtc_demo.h"
#else
#include "cmd_iot_demo.h"
#endif

static const char *TAG = "app_main";
#define PROMPT_STR CONFIG_IDF_TARGET

#if CONFIG_CONSOLE_STORE_HISTORY
#define MOUNT_PATH "/data"
#define HISTORY_PATH MOUNT_PATH "/history.txt"
#else
#define MOUNT_PATH "/data"
#define HISTORY_PATH NULL
#endif // CONFIG_CONSOLE_STORE_HISTORY

#define AUDIO_TEST_FILE MOUNT_PATH "/aitalk_send_audio.wav"

static void initialize_filesystem(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT_PATH,
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(err));
        return;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%zu used=%zu", total, used);

    struct stat st;
    if (stat(AUDIO_TEST_FILE, &st) == 0) {
        ESP_LOGI(TAG, "Found %s (%ld bytes)", AUDIO_TEST_FILE, (long)st.st_size);
    } else {
        ESP_LOGW(TAG, "%s not found", AUDIO_TEST_FILE);
    }
}

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/*
 * 系统监视任务：周期性打印
 *   1) 各任务 / 整体 CPU 占用率（两次快照差分，需 CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS）
 *   2) 各任务所在核、优先级、栈剩余高水位（用于定位 task_wdt / IDLE 饿死问题）
 *   3) 堆 free / 最小空闲堆（总量 + 内部 RAM + PSRAM，用于定位内存泄漏/耗尽）
 */
#define SYS_MONITOR_PERIOD_MS  10000
#define SYS_MONITOR_STACK      4096
#define SYS_MONITOR_MAX_TASKS  32

/* 上一次快照：任务句柄 -> 运行时计数，用于计算区间内的 CPU 占用增量 */
typedef struct {
    TaskHandle_t handle;
    uint32_t     runtime;
} task_rt_sample_t;

static void system_monitor_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(SYS_MONITOR_PERIOD_MS);
    static task_rt_sample_t prev[SYS_MONITOR_MAX_TASKS];
    static UBaseType_t prev_count = 0;

    ESP_LOGI(TAG, "system monitor started, period=%ums", SYS_MONITOR_PERIOD_MS);

    for (;;) {
        vTaskDelay(period);

        /* ---- 堆信息 ---- */
        const size_t free_total    = esp_get_free_heap_size();
        const size_t min_total     = esp_get_minimum_free_heap_size();
        const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t min_internal  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        const size_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGW(TAG,
                 "[sys-mon] heap: free=%u min=%u | internal free=%u min=%u | psram free=%u",
                 free_total, min_total, free_internal, min_internal, free_psram);

        /* ---- CPU 占用（区间差分）---- */
        TaskStatus_t *tasks = malloc(SYS_MONITOR_MAX_TASKS * sizeof(TaskStatus_t));
        if (!tasks) {
            ESP_LOGW(TAG, "[sys-mon] no mem for task list");
            continue;
        }
        uint32_t total_now = 0;
        UBaseType_t n = uxTaskGetSystemState(tasks, SYS_MONITOR_MAX_TASKS, &total_now);

        /* 计算区间内的总运行时增量（无符号减法可安全处理单次溢出） */
        static uint32_t prev_total = 0;
        const uint32_t total_delta = total_now - prev_total;

        ESP_LOGW(TAG, "[sys-mon] CPU usage over last %dms  (tasks=%u)",
                 SYS_MONITOR_PERIOD_MS, (unsigned)n);
        ESP_LOGW(TAG, "  %-16s %-3s %-4s %-6s %-6s %s",
                 "Task", "Cor", "Pri", "CPU%", "Stk(B)", "State");

        for (UBaseType_t i = 0; i < n; ++i) {
            /* 在上一次快照中查找同一任务，取运行时增量 */
            uint32_t last_rt = 0;
            for (UBaseType_t j = 0; j < prev_count; ++j) {
                if (prev[j].handle == tasks[i].xHandle) {
                    last_rt = prev[j].runtime;
                    break;
                }
            }
            const uint32_t rt_delta = tasks[i].ulRunTimeCounter - last_rt;
            /* 百分比放大 10 倍以保留一位小数：pct10 = delta/total * 1000 */
            uint32_t pct10 = 0;
            if (total_delta > 0) {
                pct10 = (uint32_t)(((uint64_t)rt_delta * 1000) / total_delta);
            }

#if defined(CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID)
            char core[12];
            if (tasks[i].xCoreID == tskNO_AFFINITY) {
                core[0] = '*'; core[1] = '\0';
            } else {
                snprintf(core, sizeof(core), "%d", (int)tasks[i].xCoreID);
            }
#else
            const char *core = "?";
#endif
            ESP_LOGW(TAG, "  %-16s %-3s %-4u %2u.%01u%%  %-6u %c",
                     tasks[i].pcTaskName,
                     core,
                     (unsigned)tasks[i].uxCurrentPriority,
                     (unsigned)(pct10 / 10), (unsigned)(pct10 % 10),
                     (unsigned)(tasks[i].usStackHighWaterMark * sizeof(StackType_t)),
                     (tasks[i].eCurrentState == eRunning)   ? 'R' :
                     (tasks[i].eCurrentState == eReady)     ? 'r' :
                     (tasks[i].eCurrentState == eBlocked)   ? 'B' :
                     (tasks[i].eCurrentState == eSuspended) ? 'S' : '-');
        }

        /* 保存本次快照供下轮差分 */
        prev_count = (n > SYS_MONITOR_MAX_TASKS) ? SYS_MONITOR_MAX_TASKS : n;
        for (UBaseType_t i = 0; i < prev_count; ++i) {
            prev[i].handle  = tasks[i].xHandle;
            prev[i].runtime = tasks[i].ulRunTimeCounter;
        }
        prev_total = total_now;

        free(tasks);
    }
}

void app_main(void)
{
    initialize_nvs();

#if CONFIG_CONSOLE_STORE_HISTORY
    initialize_filesystem();
    ESP_LOGI(TAG, "Command history enabled");
#else
    initialize_filesystem();
    ESP_LOGI(TAG, "Command history disabled");
#endif

    initialize_console_peripheral();
    initialize_console_library(HISTORY_PATH);

    const char *prompt = setup_prompt(PROMPT_STR ">");

    /* Register commands */
    esp_console_register_help_command();
    register_system();
    register_wifi();
    register_nvs();
#if TC_PRODUCT_TRTC
    register_trtc_demo();
#else
    register_iot_demo_commands();
#endif

    /* Auto-connect WiFi from NVS (non-blocking) */
    wifi_auto_connect_from_nvs();

    /* 系统监视：后台周期打印各任务/整体 CPU 占用 + 栈高水位 + 堆，便于定位 task_wdt/OOM（见 system_monitor_task） */
    xTaskCreate(system_monitor_task, "sys_mon", SYS_MONITOR_STACK, NULL, 5, NULL);

#if TC_PRODUCT_TRTC
    printf("\n"
           "TRTC Console. Type 'help' for available commands.\n"
           "trtc_enter / trtc_exit / trtc_audio_start / trtc_video_start ...\n");
#else
    printf("\n"
           "IoT  Console. Type 'help' for available commands.\n"
           "Use UP/DOWN arrows to navigate history. TAB to auto-complete.\n");
#endif

    if (linenoiseIsDumbMode()) {
        printf("\n"
               "Your terminal does not support escape sequences.\n"
               "Line editing and history features are disabled.\n");
    }

    /* Main loop */
    while (true) {
        char *line = linenoise(prompt);
        if (line == NULL) {
            continue;
        }

        if (strlen(line) > 0) {
            linenoiseHistoryAdd(line);
#if CONFIG_CONSOLE_STORE_HISTORY
            linenoiseHistorySave(HISTORY_PATH);
#endif
        }

        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command\n");
        } else if (err == ESP_ERR_INVALID_ARG) {
            // empty command
        } else if (err == ESP_OK && ret != ESP_OK) {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK) {
            printf("Internal error: %s\n", esp_err_to_name(err));
        }
        linenoiseFree(line);
    }

    ESP_LOGE(TAG, "Error or end-of-input, terminating console");
    esp_console_deinit();
}
