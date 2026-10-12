#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Register WiFi commands: join, wifi_status, wifi_clear
void register_wifi(void);

// Auto-connect using saved NVS credentials (non-blocking)
void wifi_auto_connect_from_nvs(void);

#ifdef __cplusplus
}
#endif
