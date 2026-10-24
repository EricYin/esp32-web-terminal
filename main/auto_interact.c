#include "auto_interact.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/uart.h"

// 引入原固件中已实现的网页端数据广播函数（从 web_server.h）
// 这样我们的彩色日志可以直接推送到用户的网页屏幕上
extern void ws_broadcast_binary(const char *data, size_t len);

#define TAG "AUTO_INTERACT"
#define UART_PORT_NUM         UART_NUM_1 // 请根据项目具体绑定的SBC串口号微调 (一般为UART_NUM_1)
#define MIN_INTERVAL_US       (2 * 60 * 1000 * 1000LL) // 2分钟限制
#define MAX_RETRY_COUNT       3

// ANSI 颜色控制字符
#define ANSI_RED   "\x1b[31m"
#define ANSI_GRN   "\x1b[32m"
#define ANSI_RST   "\x1b[0m"

// 状态机内部状态
typedef enum {
    AUTO_STATE_IDLE,
    AUTO_STATE_WAIT_VERSION_REPLY,
    AUTO_STATE_EXECUTING,
    AUTO_STATE_MANUAL_LOCKED
} auto_state_t;

// 绑定命令与其成功预期的输出关键字
typedef struct {
    const char* command;
    const char* expected_keyword;
    uint32_t timeout_ms;
} auto_cmd_t;

// 【用户自定义命令集合】：按需在这里修改您的初始化命令序列
static const auto_cmd_t my_cmd_sequence[] = {
    { "printenv\n", "Environment", 5000 },
    { "boot\n",     "Booting",     8000 }
};
#define CMD_COUNT (sizeof(my_cmd_sequence) / sizeof(my_cmd_sequence[0]))

// 全局状态变量
static TaskHandle_t s_auto_task_handle = NULL;
static volatile auto_state_t s_auto_state = AUTO_STATE_IDLE;
static int64_t s_last_success_time = 0;
static int s_frequent_boot_count = 0;

// 滑动窗口匹配缓冲区
#define MATCH_BUF_SIZE 128
static char s_match_buf[MATCH_BUF_SIZE];
static size_t s_match_buf_len = 0;
static const char* s_target_keyword = NULL;

// 内部函数：向本地日志和网页端双向输出带有颜色的状态
static void report_auto_status(bool is_ok, const char* msg) {
    char format_buf[256];
    if (is_ok) {
        snprintf(format_buf, sizeof(format_buf), "\r\n" ANSI_GRN "[ESP32-AUTO-OK] %s" ANSI_RST "\r\n", msg);
        ESP_LOGI(TAG, "%s", msg);
    } else {
        snprintf(format_buf, sizeof(format_buf), "\r\n" ANSI_RED "[ESP32-AUTO-ERR] %s" ANSI_RST "\r\n", msg);
        ESP_LOGE(TAG, "%s", msg);
    }
    // 广播到网页端 Web Terminal 展现
    ws_broadcast_binary(format_buf, strlen(format_buf));
}

// 内部工具：清空滑动窗口
static void reset_match_buf(void) {
    memset(s_match_buf, 0, sizeof(s_match_buf));
    s_match_buf_len = 0;
}

// 内部工具：向滑动窗口推送并比对指定关键字
static bool check_buffer_keyword(char c, const char* keyword) {
    if (!keyword) return false;
    if (s_match_buf_len >= MATCH_BUF_SIZE - 1) {
        memmove(s_match_buf, s_match_buf + 1, s_match_buf_len - 1);
        s_match_buf_len--;
    }
    s_match_buf[s_match_buf_len++] = c;
    s_match_buf[s_match_buf_len] = '\0';
    return (strstr(s_match_buf, keyword) != NULL);
}

// 核心自动化处理任务任务
static void auto_interact_task(void *pvParameters) {
    uint32_t notify_val;
    while(1) {
        // 阻塞等待底层的第一个 "uboot >" 信号唤醒
        if (xTaskNotifyWait(0, ULONG_MAX, &notify_val, portTICK_PERIOD_MS) == pdTRUE) {
            
            if (s_auto_state == AUTO_STATE_MANUAL_LOCKED) {
                continue; // 锁定状态，彻底忽略一切触发
            }

            // 1. 判断是否 2 分钟频繁上电重启触发
            int64_t current_time = esp_timer_get_time();
            if (s_last_success_time > 0 && (current_time - s_last_success_time) < MIN_INTERVAL_US) {
                s_frequent_boot_count++;
                if (s_frequent_boot_count >= MAX_RETRY_COUNT) {
                    s_auto_state = AUTO_STATE_MANUAL_LOCKED;
                    report_auto_status(false, "!!! CRITICAL LOCKOUT: Fast Bootloop detected 3 times! Disabling automation. MANUAL INTERVENTION REQUIRED!");
                    continue;
                }
            } else {
                s_frequent_boot_count = 0; 
            }

            bool current_round_success = false;
            int current_uboot_fail_count = 0;

            // 2. 同一次 U-Boot 内的重试大循环
            while (current_uboot_fail_count ' timeout after command!");
                        seq_ok = false; break;
                    }
                }

                if (seq_ok) {
                    current_round_success = true;
                    s_last_success_time = esp_timer_get_time(); // 记录本次大流程的成功锚点时间
                    report_auto_status(true, "All commands executed and output verified successfully!");
                    s_auto_state = AUTO_STATE_IDLE;
                    break; // 全套通关，跳出重试循环
                } else {
                    current_uboot_fail_count++;
                    char err_log[128];
                    snprintf(err_log, sizeof(err_log), "Command execution failed in current U-Boot. Issuing Ctrl+C recovery (%d/3)...", current_uboot_fail_count);
                    report_auto_status(false, err_log);

                    // 强力灌入 Ctrl+C，唤醒卡住的 U-Boot 重新建立干净行
                    uart_write_bytes(UART_PORT_NUM, "\x03\n", 2);
                    vTaskDelay(pdMS_TO_TICKS(1500)); // 优雅等待 1.5 秒令其就绪，返回 while 头部重试
                }
            }

            // 如果用尽 3 次重试机会（每次都Ctrl+C补救了）依然无法完成：拉闸保护
            if (!current_round_success && current_uboot_fail_count >= MAX_RETRY_COUNT) {
                s_auto_state = AUTO_STATE_MANUAL_LOCKED;
                report_auto_status(false, "!!! LOCKOUT: 3 execution attempts failed. Suspending automation. MANUAL INTERVENTION REQUIRED!");
            }
        }
    }
}

// 供底层串口数据中断/接收任务实时喂入的旁路监听器
void auto_interact_feed_char(char c) {
    if (!s_auto_task_handle) return;

    // 常规空闲监听：死盯首个起始提示符 "uboot >"
    if (s_auto_state == AUTO_STATE_IDLE) {
        if (check_buffer_keyword(c, "uboot >")) {
            xTaskNotifyGive(s_auto_task_handle); // 丢出初次触发信号
        }
    } 
    // 运行中状态判定
    else if (s_auto_state == AUTO_STATE_WAIT_VERSION_REPLY || s_auto_state == AUTO_STATE_EXECUTING) {
        // 信号 0x01: 捕获到当前正在等待的关键回显字符串
        if (s_target_keyword && check_buffer_keyword(c, s_target_keyword)) {
            s_target_keyword = NULL; // 阶段匹配成功即刻清空防重触发
            xTaskNotify(s_auto_task_handle, 0x01, eSetValueWithOverwrite);
        }
        // 信号 0x02: 捕获到命令阶段物理收尾的提示符
        // 这里必须用一套完全独立的独立滑动窗口隔离判断，简化实现直接匹配
        static char p_buf[16]; static size_t p_len = 0;
        if (p_len >= 15) { memmove(p_buf, p_buf+1, p_len-1); p_len--; }
        p_buf[p_len++] = c; p_buf[p_len] = '\0';
        if (strstr(p_buf, "uboot >") != NULL) {
            memset(p_buf, 0, sizeof(p_buf)); p_len = 0;
            xTaskNotify(s_auto_task_handle, 0x02, eSetValueWithOverwrite);
        }
    }
}

// 人工打字/干预时的解锁钩子
void auto_interact_web_input_hook(void) {
    if (s_auto_state == AUTO_STATE_MANUAL_LOCKED) {
        s_auto_state = AUTO_STATE_IDLE;
        s_frequent_boot_count = 0;
        s_last_success_time = 0;
        s_target_keyword = NULL;
        reset_match_buf();
        report_auto_status(true, "[MANUAL-RELEASE] Administrator typed in console. Automation system re-armed.");
    }
}

// 初始化注册
void auto_interact_init(void) {
    xTaskCreate(auto_interact_task, "auto_interact_task", 4096, NULL, 5, &s_auto_task_handle);
    ESP_LOGI(TAG, "Interactive Automation engine started successfully.");
}
