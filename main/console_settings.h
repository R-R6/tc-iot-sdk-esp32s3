#pragma once

/**
 * @brief Initialize console peripheral (UART/USB_OTG/USB_JTAG based on sdkconfig)
 */
void initialize_console_peripheral(void);

/**
 * @brief Initialize linenoise and esp_console
 * @param history_path Path to store command history, or NULL to disable
 */
void initialize_console_library(const char *history_path);

/**
 * @brief Setup console prompt with optional color
 * @param prompt_str Prompt string, e.g. "esp32p4>"
 * @return Pointer to formatted prompt
 */
char *setup_prompt(const char *prompt_str);
