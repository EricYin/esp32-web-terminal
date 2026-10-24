#include "auto_interact.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/uart.h"

extern void ws_broadcast_binary(const char *data, size_t len);

#define TAG "AUTO_INTERACT"
#define UART_PORT_NUM         UART_NUM_1 
#define MIN_INTERVAL_US       (2 * 60 * 1000 * 1000LL) 
#define MAX_RETRY_COUNT       3

#define ANSI_RED   "\x1b[31m"
#define ANSI_GRN   "\x1b[32m"
#define ANSI_RST   "\x1b[0m"

#define NOTIFY_BIT_RAW_DATA  (1 << 0)  

typedef enum {
    AUTO_STATE_IDLE,
    AUTO_STATE_WAIT_VERSION_REPLY,
    AUTO_STATE_EXECUTING,
    AUTO_STATE_MANUAL_LOCKED
} auto_state_t;

typedef struct {
    const char* command;
    const char* expected_keyword;
    uint32_t timeout_ms;
} auto_cmd_t;

// 自定义命令集合
static const auto_cmd_t my_cmd_sequence[] = {
    { "printenv\n", "Environment", 5000 },
    { "boot\n",     "Booting",     8000 }
};
#define CMD_COUNT (sizeof(my_cmd_sequence) / sizeof(my_cmd_sequence[0]))

static TaskHandle_t s_auto_task_handle = NULL;
static volatile auto_state_t s_auto_state = AUTO_STATE_IDLE;
static int64_t s_last_trigger_time = 0; 
static int s_frequent_boot_count = 0;

// 【修复：加上 volatile 修饰，防止多任务数据不一致】
#define RAW_RING_BUF_SIZE 1024
static volatile uint8_t s_ring_buf[RAW_RING_BUF_SIZE];
static volatile size_t s_ring_head = 0;
static volatile size_t s_ring_tail = 0;

#define MATCH_BUF_SIZE 128
static char s_match_buf[MATCH_BUF_SIZE];
static size_t s_match_buf_len = 0;

#define PROMPT_BUF_SIZE 16
static char s_prompt_buf[PROMPT_BUF_SIZE];
static size_t s_prompt_buf_len = 0;

static const char* s_target_keyword = NULL;
static volatile bool s_keyword_matched = false;
static volatile bool s_prompt_matched = false;

// 【修复：正确显示 [384] 数组长度，绝不再让大模型吞掉字符】
static void report_auto_status(bool is_ok, const char* msg) {
    if (!msg) return;
    char format_buf[384]; 
    if (is_ok) {
        snprintf(format_buf, sizeof(format_buf), "\r\n" ANSI_GRN "[ESP32-AUTO-OK] %s" ANSI_RST "\r\n", msg);
        ESP_LOGI(TAG, "%s", msg);
    } else {
        snprintf(format_buf, sizeof(format_buf), "\r\n" ANSI_RED "[ESP32-AUTO-ERR] %s" ANSI_RST "\r\n", msg);
        ESP_LOGE(TAG, "%s", msg);
    }
    ws_broadcast_binary(format_buf, strlen(format_buf));
}

static void reset_match_windows(void) {
    memset(s_match_buf, 0, sizeof(s_match_buf));
    s_match_buf_len = 0;
    memset(s_prompt_buf, 0, sizeof(s_prompt_buf));
    s_prompt_buf_len = 0;
    s_keyword_matched = false;
    s_prompt_matched = false;
}

static void process_incoming_char(char c) {
    if (s_prompt_buf_len >= PROMPT_BUF_SIZE - 1) {
        memmove(s_prompt_buf, s_prompt_buf + 1, PROMPT_BUF_SIZE - 2);
        s_prompt_buf_len = PROMPT_BUF_SIZE - 2;
    }
    s_prompt_buf[s_prompt_buf_len++] = c;
    s_prompt_buf[s_prompt_buf_len] = '\0';
    if (strstr(s_prompt_buf, "uboot >") != NULL) {
        s_prompt_matched = true;
        memset(s_prompt_buf, 0, sizeof(s_prompt_buf));
        s_prompt_buf_len = 0;
    }

    if (s_auto_state == AUTO_STATE_WAIT_VERSION_REPLY || s_auto_state == AUTO_STATE_EXECUTING) {
        if (s_target_keyword) {
            if (s_match_buf_len >= MATCH_BUF_SIZE - 1) {
                memmove(s_match_buf, s_match_buf + 1, MATCH_BUF_SIZE - 2);
                s_match_buf_len = MATCH_BUF_SIZE - 2;
            }
            s_match_buf[s_match_buf_len++] = c;
            s_match_buf[s_match_buf_len] = '\0';
            if (strstr(s_match_buf, s_target_keyword) != NULL) {
                s_keyword_matched = true;
            }
        }
    }
}

static bool wait_for_matches(const char* keyword, uint32_t timeout_ms) {
    if (!keyword) return false;
    s_target_keyword = keyword;
    s_keyword_matched = false;
    s_prompt_matched = false;
    reset_match_windows();

    TickType_t start_ticks = xTaskGetTickCount();
    uint32_t notify_val = 0;

    xTaskNotifyStateClear(NULL);

    while ((xTaskGetTickCount() - start_ticks) < pdMS_TO_TICKS(timeout_ms)) {
        while (s_ring_head != s_ring_tail) {
            char c = (char)s_ring_buf[s_ring_tail];
            s_ring_tail = (s_ring_tail + 1) % RAW_RING_BUF_SIZE;
            process_incoming_char(c);

            if (s_keyword_matched && s_prompt_matched) {
                s_target_keyword = NULL;
                return true;
            }
        }

        TickType_t elapsed = xTaskGetTickCount() - start_ticks;
        if (elapsed >= pdMS_TO_TICKS(timeout_ms)) break;
        TickType_t remaining = pdMS_TO_TICKS(timeout_ms) - elapsed;

        xTaskNotifyWait(ULONG_MAX, ULONG_MAX, &notify_val, remaining);
    }

    s_target_keyword = NULL;
    return false;
}

static void auto_interact_task(void *pvParameters) {
    uint32_t notify_val;
    while(1) {
        s_auto_state = AUTO_STATE_IDLE;
        s_prompt_matched = false;
        
        while (!s_prompt_matched) {
            if (s_ring_head != s_ring_tail) {
                char c = (char)s_ring_buf[s_ring_tail];
                s_ring_tail = (s_ring_tail + 1) % RAW_RING_BUF_SIZE;
                process_incoming_char(c);
            } else {
                xTaskNotifyWait(ULONG_MAX, ULONG_MAX, &notify_val, portMAX_DELAY);
            }
        }

        if (s_auto_state == AUTO_STATE_MANUAL_LOCKED) {
            continue; 
        }

        // 【修复：将两分钟检测放到最前端，只要高频触碰提示符就直接计数】
        int64_t current_time = esp_timer_get_time();
        if (s_last_trigger_time > 0 && (current_time - s_last_trigger_time) < MIN_INTERVAL_US) {
            s_frequent_boot_count++;
            if (s_frequent_boot_count >= MAX_RETRY_COUNT) {
                s_auto_state = AUTO_STATE_MANUAL_LOCKED;
                report_auto_status(false, "!!! CRITICAL LOCKOUT: Fast Bootloop detected 3 times! Disabling automation.");
                continue;
            }
        } else {
            s_frequent_boot_count = 0; 
        }
        s_last_trigger_time = current_time; 

        bool current_round_success = false;
        int current_uboot_fail_count = 0;

        while (current_uboot_fail_count < MAX_RETRY_COUNT) {
            s_auto_state = AUTO_STATE_WAIT_VERSION_REPLY;
            report_auto_status(true, "Verifying U-Boot state via 'version' command...");
            
            uart_write_bytes(UART_PORT_NUM, "version\n", 8);
            if (!wait_for_matches("U-Boot", 3000)) {
                current_uboot_fail_count++;
                report_auto_status(false, "U-Boot verification failed or timed out.");
                uart_write_bytes(UART_PORT_NUM, "\x03\n", 2); 
                vTaskDelay(pdMS_TO_TICKS(1500));
                continue;
            }

            s_auto_state = AUTO_STATE_EXECUTING;
            bool seq_ok = true;
            
            for (size_t i = 0; i < CMD_COUNT; i++) {
                if (!my_cmd_sequence[i].command || !my_cmd_sequence[i].expected_keyword) {
                    seq_ok = false; break;
                }

                // 【修复：正确显示 [64] 数组长度，绝不再让大模型吞掉字符】
                char log_msg[64];
                snprintf(log_msg, sizeof(log_msg), "Sending sequence [%d/%d]", (int)i+1, (int)CMD_COUNT);
                report_auto_status(true, log_msg);

                uart_write_bytes(UART_PORT_NUM, my_cmd_sequence[i].command, strlen(my_cmd_sequence[i].command));
                
                if (!wait_for_matches(my_cmd_sequence[i].expected_keyword, my_cmd_sequence[i].timeout_ms)) {
                    report_auto_status(false, "Command execution failed or timed out. Stopping sequence.");
                    seq_ok = false; 
                    break; 
                }
            }

            if (seq_ok) {
                current_round_success = true;
                report_auto_status(true, "All commands executed and verified successfully!");
                break; 
            } else {
                current_uboot_fail_count++;
                if (current_uboot_fail_count < MAX_RETRY_COUNT) {
                    uart_write_bytes(UART_PORT_NUM, "\x03\n", 2); 
                    vTaskDelay(pdMS_TO_TICKS(1500));
                }
            }
        }

        if (!current_round_success) {
            s_auto_state = AUTO_STATE_MANUAL_LOCKED;
            report_auto_status(false, "!!! LOCKOUT: Automation suspended. MANUAL INTERVENTION REQUIRED!");
        }
    }
}

void auto_interact_feed_buffer(const char* buf, size_t len) {
    if (!s_auto_task_handle || !buf || len == 0) return;

    for (size_t i = 0; i < len; i++) {
        size_t next_head = (s_ring_head + 1) % RAW_RING_BUF_SIZE;
        if (next_head == s_ring_tail) {
            s_ring_tail = (s_ring_tail + 1) % RAW_RING_BUF_SIZE;
        }
        s_ring_buf[s_ring_head] = (uint8_t)buf[i];
        s_ring_head = next_head;
    }
    xTaskNotify(s_auto_task_handle, NOTIFY_BIT_RAW_DATA, eSetBits);
}

void auto_interact_web_input_hook(void) {
    if (s_auto_state == AUTO_STATE_MANUAL_LOCKED) {
        s_auto_state = AUTO_STATE_IDLE;
        s_frequent_boot_count = 0;
        s_last_trigger_time = 0;
        s_target_keyword = NULL;
        reset_match_windows();
        s_ring_head = 0;
        s_ring_tail = 0;
        report_auto_status(true, "[MANUAL-RELEASE] Administrator intervened. Automation system re-armed.");
    }
}

void auto_interact_init(void) {
    xTaskCreate(auto_interact_task, "auto_interact_task", 4096, NULL, 5, &s_auto_task_handle);
    ESP_LOGI(TAG, "Safe High-Performance Automation engine initialized.");
}
