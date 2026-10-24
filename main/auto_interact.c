#include "auto_interact.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "nvs.h"

extern void ws_broadcast_binary(const char *data, size_t len);

#define TAG "AUTO_INTERACT"
#define UART_PORT_NUM           UART_NUM_1 
#define MIN_INTERVAL_US         (2 * 60 * 1000 * 1000LL) // 2分钟限制（微秒）
#define MAX_RETRY_COUNT         3

// ANSI 颜色控制字符修正
#define ANSI_RED                "\x1b[31m"
#define ANSI_GRN                "\x1b[32m"
#define ANSI_YEL                "\x1b[33m"
#define ANSI_RST                "\x1b[0m"

#define BIT_EVENT_PROMPT        (1 << 0) 
#define BIT_EVENT_KEYWORD       (1 << 1) 

typedef enum {
    AUTO_STATE_IDLE,          
    AUTO_STATE_VERIFYING,     
    AUTO_STATE_EXECUTING,     
    AUTO_STATE_MANUAL_LOCKED  
} auto_state_t;

typedef struct {
    const char* command;
    const char* expected_keyword;
    uint32_t timeout_ms;
} auto_cmd_t;

// 用户自定义命令集合
static const auto_cmd_t my_cmd_sequence[] = {
    { "version\n",   "U-Boot",      3000 }, 
    { "printenv\n",  "Environment", 5000 }, 
    { "boot\n",      "Booting",     8000 }  
};
#define CMD_COUNT (sizeof(my_cmd_sequence) / sizeof(my_cmd_sequence[0]))

// 全局变量
static TaskHandle_t s_auto_task_handle = NULL;
static EventGroupHandle_t s_auto_event_group = NULL;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile auto_state_t s_auto_state = AUTO_STATE_IDLE;
static int64_t s_last_success_time = 0;
static int s_frequent_boot_count = 0;

#define MATCH_BUF_SIZE 64
static char s_match_buf[MATCH_BUF_SIZE];
static size_t s_match_buf_len = 0;
static const char* volatile s_target_keyword = NULL; // 加上 volatile 确保多任务可见性

static void nvs_save_boot_stats(int count, int64_t last_time) {
    nvs_handle_t my_handle;
    if (nvs_open("auto_inter", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_i32(my_handle, "boot_cnt", count);
        nvs_set_i64(my_handle, "last_time", last_time);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

static void nvs_load_boot_stats(int *count, int64_t *last_time) {
    nvs_handle_t my_handle;
    if (nvs_open("auto_inter", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_i32(my_handle, "boot_cnt", count);
        nvs_get_i64(my_handle, "last_time", last_time);
        nvs_close(my_handle);
    }
}

static void set_auto_state(auto_state_t new_state) {
    taskENTER_CRITICAL(&s_state_mux);
    s_auto_state = new_state;
    taskEXIT_CRITICAL(&s_state_mux);
}

static void report_auto_status(bool is_ok, const char* msg) {
    char format_buf[256];
    if (is_ok) {
        snprintf(format_buf, sizeof(format_buf), "\r\n" ANSI_GRN "[ESP32-AUTO-OK] %s" ANSI_RST "\r\n", msg);
    } else {
        snprintf(format_buf, sizeof(format_buf), "\r\n" ANSI_RED "[ESP32-AUTO-ERR] %s" ANSI_RST "\r\n", msg);
    }
    printf("%s", format_buf); 
    ws_broadcast_binary(format_buf, strlen(format_buf)); 
}

// 核心匹配过滤器（注意：此函数不可在中断服务程序 ISR 中直接调用！）
static void process_char_for_match(char c) {
    if (s_match_buf_len >= MATCH_BUF_SIZE - 1) {
        memmove(s_match_buf, s_match_buf + 1, s_match_buf_len - 1);
        s_match_buf_len--;
    }
    s_match_buf[s_match_buf_len++] = c;
    s_match_buf[s_match_buf_len] = '\0';

    // 需求3：限制提示符识别的时机。必须在空闲状态或等待序列收尾提示符时
    if (s_auto_state == AUTO_STATE_IDLE || s_target_keyword == NULL) {
        if (strstr(s_match_buf, "uboot >") != NULL) {
            s_match_buf[0] = '\0';
            s_match_buf_len = 0;
            xEventGroupSetBits(s_auto_event_group, BIT_EVENT_PROMPT); // 修改为标准任务级 API
            return;
        }
    }

    // 动态盯防当前步骤的自定义预期关键字
    if (s_target_keyword && strstr(s_match_buf, s_target_keyword) != NULL) {
        s_target_keyword = NULL; 
        xEventGroupSetBits(s_auto_event_group, BIT_EVENT_KEYWORD); // 修改为标准任务级 API
    }
}

static void auto_interact_task(void *pvParameters) {
    EventBits_t bits;
    while(1) {
        // 1. 阻塞等待第一个 uboot > 提示符唤醒
        bits = xEventGroupWaitBits(s_auto_event_group, BIT_EVENT_PROMPT, pdTRUE, pdFALSE, portMAX_DELAY);
        
        if (s_auto_state == AUTO_STATE_MANUAL_LOCKED) {
            continue; 
        }

        // 需求2 & 5：跨周期安全防刷网
        int64_t current_time = esp_timer_get_time();
        if (s_last_success_time > 0 && (current_time - s_last_success_time) < MIN_INTERVAL_US) {
            s_frequent_boot_count++;
            nvs_save_boot_stats(s_frequent_boot_count, current_time);
            if (s_frequent_boot_count >= MAX_RETRY_COUNT) {
                set_auto_state(AUTO_STATE_MANUAL_LOCKED);
                report_auto_status(false, "!!! CRITICAL LOCKOUT: Fast Bootloop detected >= 3 times! Disabling automation. MANUAL INTERVENTION REQUIRED! ");
                continue;
            }
        } else {
            s_frequent_boot_count = 0;
            nvs_save_boot_stats(s_frequent_boot_count, current_time);
        }

        bool current_round_success = false;
        int current_uboot_fail_count = 0;

        // 同一次 U-Boot 内的重试循环
        while (current_uboot_fail_count < MAX_RETRY_COUNT) {
            bool seq_ok = true;
            set_auto_state(AUTO_STATE_VERIFYING);
            
            // 确保环境干净
            uart_flush(UART_PORT_NUM);
            xEventGroupClearBits(s_auto_event_group, BIT_EVENT_KEYWORD | BIT_EVENT_PROMPT);
            
            // 逐条串行执行并校验命令链
            for (int i = 0; i < CMD_COUNT; i++) {
                if (i == 1) {
                    set_auto_state(AUTO_STATE_EXECUTING); 
                }
                
                s_target_keyword = my_cmd_sequence[i].expected_keyword;
                char cmd_log[128];
                snprintf(cmd_log, sizeof(cmd_log), "Sending Command [%d/%d]: %s", i + 1, CMD_COUNT, my_cmd_sequence[i].command);
                report_auto_status(true, cmd_log);
                
                uart_write_bytes(UART_PORT_NUM, my_cmd_sequence[i].command, strlen(my_cmd_sequence[i].command));
                TickType_t timeout_ticks = pdMS_TO_TICKS(my_cmd_sequence[i].timeout_ms);

                // 时序步进：等待关键字回显
                bits = xEventGroupWaitBits(s_auto_event_group, BIT_EVENT_KEYWORD, pdTRUE, pdFALSE, timeout_ticks);
                if (!(bits & BIT_EVENT_KEYWORD)) {
                    report_auto_status(false, "-> ERROR: Expected keyword missing or command execution failed!");
                    seq_ok = false;
                    break; 
                }

                // 时序步进：关键字通过后，再等待产生的提示符
                bits = xEventGroupWaitBits(s_auto_event_group, BIT_EVENT_PROMPT, pdTRUE, pdFALSE, timeout_ticks);
                if (!(bits & BIT_EVENT_PROMPT)) {
                    // 需求4：如果卡住，不退出大循环，不产生Ctrl+C，彻底挂起当前任务保留现场
                    report_auto_status(false, "-> ERROR: Command hanged! No new U-Boot prompt returned. Suspending for manual check.");
                    seq_ok = false;
                    break;
                }
            }

            if (seq_ok) {
                current_round_success = true;
                s_last_success_time = esp_timer_get_time();
                nvs_save_boot_stats(0, s_last_success_time); // 成功，计数清零
                report_auto_status(true, "All commands executed and output verified successfully!");
                s_target_keyword = NULL; // 完美通关，清除目标关键字防污染
                set_auto_state(AUTO_STATE_IDLE);
                break; 
            } else {
                current_uboot_fail_count++;
                
                // 【核心修复】无论何种失败情况，优先将目标关键字和缓冲区清空，防止死锁
                s_target_keyword = NULL;
                s_match_buf_len = 0;
                s_match_buf[0] = '\0';

                // 需求3：首条 version 命令双检失败（可能在 Linux 里面或非预期状态）
                if (s_auto_state == AUTO_STATE_VERIFYING) {
                    report_auto_status(false, "Verification failed (Not in trusted U-Boot environment). Abandoning sequence.");
                    set_auto_state(AUTO_STATE_IDLE); 
                    break; // 优雅退出，绝不盲目执行后续命令
                }

                // 需求4：命令报错了，但仍能拿到提示符（即未挂起），进行 Ctrl+C 恢复并重试
                if (current_uboot_fail_count < MAX_RETRY_COUNT) {
                    char err_log[128];
                    snprintf(err_log, sizeof(err_log), "Execution failed but prompt alive. Issuing Ctrl+C recovery (%d/3)...", current_uboot_fail_count);
                    report_auto_status(false, err_log);
                    uart_write_bytes(UART_PORT_NUM, "\x03\n", 2); 
                    vTaskDelay(pdMS_TO_TICKS(1500)); 
                }
            }
        }

        // 需求4：用尽了 3 次重试机会，引发锁死
        if (!current_round_success && current_uboot_fail_count >= MAX_RETRY_COUNT) {
            set_auto_state(AUTO_STATE_MANUAL_LOCKED);
            report_auto_status(false, "!!! LOCKOUT: 3 execution attempts failed. Suspending automation. MANUAL INTERVENTION REQUIRED!");
        }
    }
}

void auto_interact_feed_char(char c) {
    if (!s_auto_task_handle) return;
    process_char_for_match(c);
}

void auto_interact_web_input_hook(void) {
    if (s_auto_state == AUTO_STATE_MANUAL_LOCKED) {
        set_auto_state(AUTO_STATE_IDLE);
        s_frequent_boot_count = 0;
        s_last_success_time = 0;
        s_target_keyword = NULL;
        s_match_buf_len = 0;
        s_match_buf[0] = '\0';
        nvs_save_boot_stats(0, 0); 
        
        char clear_msg[128];
        snprintf(clear_msg, sizeof(clear_msg), "\r\n" ANSI_YEL "[MANUAL-RELEASE] Administrator typed in console. Automation system re-armed." ANSI_RST "\r\n");
        printf("%s", clear_msg);
        ws_broadcast_binary(clear_msg, strlen(clear_msg));
    }
}

void auto_interact_init(void) {
    // 需求5：防断电级 Bootloop，恢复 NVS 状态
    nvs_load_boot_stats(&s_frequent_boot_count, &s_last_success_time);
    s_auto_event_group = xEventGroupCreate();
    
    if (s_frequent_boot_count >= MAX_RETRY_COUNT) {
        s_auto_state = AUTO_STATE_MANUAL_LOCKED;
        printf("\r\n" ANSI_RED "[ESP32-AUTO-INIT] Active Lockout detected from NVS memory! Automation PAUSED." ANSI_RST "\r\n");
    } else {
        s_auto_state = AUTO_STATE_IDLE;
    }

    xTaskCreate(auto_interact_task, "auto_interact_task", 4096, NULL, 5, &s_auto_task_handle);
    ESP_LOGI(TAG, "Interactive Automation watchdog initialized successfully.");
}
