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

// 引入原固件中已实现的网页端数据广播函数（彩色日志将同步推送到 Web 屏幕和 ESP32 硬件 UART）
extern void ws_broadcast_binary(const char *data, size_t len);

#define TAG "AUTO_INTERACT"
#define UART_PORT_NUM         UART_NUM_1 // 根据项目绑定SBC的串口号调整
#define MIN_INTERVAL_US       (2 * 60 * 1000 * 1000LL) // 2分钟限制（微秒）
#define MAX_RETRY_COUNT       3

// ANSI 颜色控制字符
#define ANSI_RED   "\x1b[31m"
#define ANSI_GRN   "\x1b[32m"
#define ANSI_YEL   "\x1b[33m"
#define ANSI_RST   "\x1b[0m"

// FreeRTOS 事件组位定义（彻底杜绝旧版 TaskNotify 被高频覆盖丢失的漏洞）
#define BIT_EVENT_PROMPT      (1 << 0)  // 捕获到 "uboot >" 提示符
#define BIT_EVENT_KEYWORD     (1 << 1)  // 捕获到自定义关键字

// 状态机内部状态
typedef enum {
    AUTO_STATE_IDLE,          // 空闲：等待初次识别到 "uboot >"
    AUTO_STATE_VERIFYING,    // 确认中：正在发送并等待 version 命令的回显
    AUTO_STATE_EXECUTING,     // 执行中：正在顺序执行后续自定义命令集合
    AUTO_STATE_MANUAL_LOCKED  // 锁死：触发安全网，停止一切自动化，等待人工干预
} auto_state_t;

typedef struct {
    const char* command;
    const char* expected_keyword;
    uint32_t timeout_ms;
} auto_cmd_t;

// 【用户自定义命令集合】
// 满足需求1：首条命令固定为 version 进行二次验证。通过后才执行后续指令。
static const auto_cmd_t my_cmd_sequence[] = {
    { "version\n",  "U-Boot",      3000 }, // 确认当前真正处于 U-Boot 环境下
    { "printenv\n", "Environment", 5000 }, // 自定义命令1
    { "boot\n",     "Booting",     8000 }  // 自定义命令2
};
#define CMD_COUNT (sizeof(my_cmd_sequence) / sizeof(my_cmd_sequence[0]))

// 全局与同步变量
static TaskHandle_t s_auto_task_handle = NULL;
static EventGroupHandle_t s_auto_event_group = NULL;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile auto_state_t s_auto_state = AUTO_STATE_IDLE;
static int64_t s_last_success_time = 0;
static int s_frequent_boot_count = 0;

// 优化后的轻量级滑动窗口匹配缓冲区
#define MATCH_BUF_SIZE 64 
static char s_match_buf[MATCH_BUF_SIZE];
static size_t s_match_buf_len = 0;
static const char* s_target_keyword = NULL;

// NVS 持久化：防止 SBC 反复重启导致 ESP32 连带掉电，使内存中的计数器清零
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

// 状态安全切换
static void set_auto_state(auto_state_t new_state) {
    taskENTER_CRITICAL(&s_state_mux);
    s_auto_state = new_state;
    taskEXIT_CRITICAL(&s_state_mux);
}

// 满足需求1：向本地串口(UART)和网页端(Web)双向同步输出彩色状态信息
static void report_auto_status(bool is_ok, const char* msg) {
    char format_buf[256];
    if (is_ok) {
        snprintf(format_buf, sizeof(format_buf), "\r\n" ANSI_GRN "[ESP32-AUTO-OK] %s" ANSI_RST "\r\n", msg);
        printf("%s", format_buf); // 输出到本地 UART 控制台
    } else {
        snprintf(format_buf, sizeof(format_buf), "\r\n" ANSI_RED "[ESP32-AUTO-ERR] %s" ANSI_RST "\r\n", msg);
        printf("%s", format_buf); // 输出到本地 UART 控制台
    }
    ws_broadcast_binary(format_buf, strlen(format_buf)); // 广播到网页端
}

// 字符塞入与动态目标匹配（核心过滤器）
static void process_char_for_match(char c) {
    if (s_match_buf_len >= MATCH_BUF_SIZE - 1) {
        memmove(s_match_buf, s_match_buf + 1, s_match_buf_len - 1);
        s_match_buf_len--;
    }
    s_match_buf[s_match_buf_len++] = c;
    s_match_buf[s_match_buf_len] = '\0';

    // 满足需求3：只有在空闲等待唤醒，或者前一条命令已对齐关键字、正在等待收尾提示符时，才识别 "uboot >"
    // 这彻底避免了 SBC 进入 Linux 系统后由于应用日志输出类似字符而导致的误触发
    if (s_auto_state == AUTO_STATE_IDLE || s_target_keyword == NULL) { 
        if (strstr(s_match_buf, "uboot >") != NULL) {
            s_match_buf[0] = '\0'; 
            s_match_buf_len = 0;
            xEventGroupSetBitsFromISR(s_auto_event_group, BIT_EVENT_PROMPT, NULL);
            return;
        }
    }

    // 动态盯防当前步骤的自定义预期关键字
    if (s_target_keyword && strstr(s_match_buf, s_target_keyword) != NULL) {
        s_target_keyword = NULL; // 匹配成功后即刻清空，防止单视窗内二次重复触发
        xEventGroupSetBitsFromISR(s_auto_event_group, BIT_EVENT_KEYWORD, NULL);
    }
}

// 核心自动化处理 FreeRTOS 任务
static void auto_interact_task(void *pvParameters) {
    while(1) {
        // 阻塞等待底层的第一个 "uboot >" 提示符信号唤醒
        EventBits_t bits = xEventGroupWaitBits(s_auto_event_group, BIT_EVENT_PROMPT, pdTRUE, pdFALSE, portMAX_DELAY);
        
        if (s_auto_state == AUTO_STATE_MANUAL_LOCKED) {
            continue; // 如果系统已拉闸锁定，彻底忽略外界任何串口信号
        }

        // 满足需求2 & 5：跨周期安全防刷网（结合 NVS 闪存数据）
        int64_t current_time = esp_timer_get_time();
        if (s_last_success_time > 0 && (current_time - s_last_success_time) < MIN_INTERVAL_US) {
            s_frequent_boot_count++;
            nvs_save_boot_stats(s_frequent_boot_count, current_time);
            if (s_frequent_boot_count >= MAX_RETRY_COUNT) {
                set_auto_state(AUTO_STATE_MANUAL_LOCKED);
                report_auto_status(false, "!!! CRITICAL LOCKOUT: Fast Bootloop detected >= 3 times! Disabling automation. MANUAL INTERVENTION REQUIRED!");
                continue;
            }
        } else {
            s_frequent_boot_count = 0;
            nvs_save_boot_stats(s_frequent_boot_count, current_time);
        }

        bool current_round_success = false;
        int current_uboot_fail_count = 0;

        // 同一次 U-Boot 内的重试大循环（最高3次）
        while (current_uboot_fail_count < MAX_RETRY_COUNT) {
            bool seq_ok = true;
            set_auto_state(AUTO_STATE_VERIFYING);

            // 清空硬件串口缓冲区及事件位，确保时序环境绝对干净
            uart_flush(UART_PORT_NUM);
            xEventGroupClearBits(s_auto_event_group, BIT_EVENT_KEYWORD | BIT_EVENT_PROMPT);

            // 逐条串行执行并校验命令链
            for (int i = 0; i < CMD_COUNT; i++) {
                if (i == 1) {
                    set_auto_state(AUTO_STATE_EXECUTING); // 过了第一条version双检命令后，正式转入正在执行状态
                }

                s_target_keyword = my_cmd_sequence[i].expected_keyword;

                char cmd_log[128];
                snprintf(cmd_log, sizeof(cmd_log), "Sending Command [%d/%d]: %s", i + 1, CMD_COUNT, my_cmd_sequence[i].command);
                report_auto_status(true, cmd_log);

                // 写入 SBC 串口
                uart_write_bytes(UART_PORT_NUM, my_cmd_sequence[i].command, strlen(my_cmd_sequence[i].command));

                TickType_t timeout_ticks = pdMS_TO_TICKS(my_cmd_sequence[i].timeout_ms);
                
                // 满足需求1：串行时序步进 —— 必须先等到关键字回显
                bits = xEventGroupWaitBits(s_auto_event_group, BIT_EVENT_KEYWORD, pdTRUE, pdFALSE, timeout_ticks);
                if (!(bits & BIT_EVENT_KEYWORD)) {
                    report_auto_status(false, "-> ERROR: Expected keyword missing or command execution failed!");
                    seq_ok = false; // 满足需求1：出错立即标记
                    break;          // 满足需求1：不继续下一条，直接跳出 for 循环
                }

                // 串行时序步进 —— 关键字通过后，再等待该命令收尾产生的新提示符
                bits = xEventGroupWaitBits(s_auto_event_group, BIT_EVENT_PROMPT, pdTRUE, pdFALSE, timeout_ticks);
                if (!(bits & BIT_EVENT_PROMPT)) {
                    // 满足需求4：如果在 U-Boot 下触发的命令卡住了（不吐出新提示符），seq_ok = false 且外部没有 Ctrl+C 动作
                    // 任务将直接跳出，停留在当前挂起状态，完美保留 U-Boot 现场等待人工干预
                    report_auto_status(false, "-> ERROR: Command hanged! No new U-Boot prompt returned. Suspending for manual check.");
                    seq_ok = false;
                    break;
                }
            }

            if (seq_ok) {
                current_round_success = true;
                s_last_success_time = esp_timer_get_time();
                nvs_save_boot_stats(0, s_last_success_time); // 全套命令完美通关，NVS 计数清零
                report_auto_status(true, "All commands executed and output verified successfully!");
                set_auto_state(AUTO_STATE_IDLE);
                break; // 跳出重试大循环
            } else {
                current_uboot_fail_count++;
                s_target_keyword = NULL; 

                // 满足需求3：如果是第一条双检 version 命令就失败了（表明当前可能处在非预期提示符或 Linux 系统下）
                if (s_auto_state == AUTO_STATE_VERIFYING) {
                    report_auto_status(false, "Verification failed (Not in trusted U-Boot environment). Abandoning sequence.");
                    set_auto_state(AUTO_STATE_IDLE); // 优雅退回空闲监听状态，绝不往下盲目触发后续自定义命令
                    break; 
                }

                // 满足需求4：如果没卡住（命令报错了，但重新看到了 U-Boot 提示符），则进入下一次重试大循环
                if (current_uboot_fail_count < MAX_RETRY_COUNT) {
                    char err_log[128];
                    snprintf(err_log, sizeof(err_log), "Execution failed but prompt alive. Issuing Ctrl+C recovery (%d/3)...", current_uboot_fail_count);
                    report_auto_status(false, err_log);

                    uart_write_bytes(UART_PORT_NUM, "\x03\n", 2); // 灌入 Ctrl+C 清理脏行
                    vTaskDelay(pdMS_TO_TICKS(1500));             // 等待 1.5 秒后回到 while 头部重新开始
                }
            }
        }

        // 满足需求4：如果没卡住、疯狂刷错并反复看到提示符，用尽了 3 次重试机会，或者引发了 2 分钟防刷限制
        if (!current_round_success && current_uboot_fail_count >= MAX_RETRY_COUNT) {
            set_auto_state(AUTO_STATE_MANUAL_LOCKED);
            report_auto_status(false, "!!! LOCKOUT: 3 execution attempts failed. Suspending automation. MANUAL INTERVENTION REQUIRED!");
        }
    }
}

// 供底层串口接收中断或数据接收任务（如 uart_read_bytes 边缘）实时喂入字符的旁路监听钩子
void auto_interact_feed_char(char c) {
    if (!s_auto_task_handle) return;
    process_char_for_match(c);
}

// 满足需求2：当管理员在控制台（如 Web 端）亲自打字输入时触发的解锁钩子
void auto_interact_web_input_hook(void) {
    if (s_auto_state == AUTO_STATE_MANUAL_LOCKED) {
        set_auto_state(AUTO_STATE_IDLE);
        s_frequent_boot_count = 0;
        s_last_success_time = 0;
        s_target_keyword = NULL;
        nvs_save_boot_stats(0, 0); // 清空并重置 NVS 闪存数据状态
        
        char clear_msg[128];
        snprintf(clear_msg, sizeof(clear_msg), "\r\n" ANSI_YEL "[MANUAL-RELEASE] Administrator typed in console. Automation system re-armed." ANSI_RST "\r\n");
        printf("%s", clear_msg);
        ws_broadcast_binary(clear_msg, strlen(clear_msg));
    }
}

// 初始化函数（可在主程序 main.c 启动时调用）
void auto_interact_init(void) {
    // 满足需求5：从持久化 NVS 闪存加载历史断电状态，防御硬件级 Bootloop 
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
