// Copyright (c) 2026 Tencent. All rights reserved.

#ifndef PLATFORM_COMPAT_H_
#define PLATFORM_COMPAT_H_

#ifdef PLATFORM_FREERTOS
#  include "freertos/FreeRTOS.h"
#  include "freertos/task.h"

#  ifndef rtos_delay_milliseconds
#    define rtos_delay_milliseconds(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#  endif

#  ifndef rtos_get_time
#    define rtos_get_time() ((long long)(xTaskGetTickCount() * portTICK_PERIOD_MS))
#  endif
#endif

#endif  // PLATFORM_COMPAT_H_
