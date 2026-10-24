#include "auto_interact.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/uart.h"

// 引入原固件中已实现的网页端数据广播函数
extern void ws_broadcast_binary(const char *data, size_t len);

#define TAG "AUTO_INTERACT"
#define UART_PORT_NUM UART_NUM_1 
#define MIN_INTERVAL_US (2 * 60 * 1000 * 1000LL) // 2分钟限制
#define MAX_FREQ_ERR_COUNT 3
#define UBOOT_TIMEOUT_MS 10000                  // 10秒无更新超时

// ANSI 颜色控制字符
#define ANSI_RED "\x1b[31m"
#define ANSI_GRN "\x1b[32m"
#define ANSI_RST "\x1b[0m"

// U-Boot 相关目标字符串（可根据实际硬件微调，如 "=> " 或 "uboot> "）
#define UBOOT_PROMPT "=> "
#define VERSION_CMD "version\r\n"
#define VERSION_EXPECTED "U-Boot"

// 状态机内部状态
typedef enum {
    AUTO_STATE_CHECKING,      // 正在监控并读取 UART 数据流
    AUTO_STATE_EXECUTING,     // 正在执行自定义命令序列（此时挂起检测）
    AUTO_STATE_MANUAL_LOCKED  // 频率触发超限，锁死需要人工干预
} auto_state_t;

// 绑定命令与其成功预期的输出关键字
typedef struct {
    const char* command;
    const char* expected_keyword;
    uint32_t timeout_ms;
} auto_cmd_t;

// 【用户自定义命令集合】统一改为兼容性最好的 \r\n 结尾
static const auto_cmd_t my_cmd_sequence[] = {
    { "printenv\r\n", "Environment", 5000 },
    { "boot\r\n",     "Booting",     8000 }
};
#define CMD_COUNT (sizeof(my_cmd_sequence) / sizeof(my_cmd_sequence[0]))

// 环形缓冲区结构体 (满足需求 1)
#define RING_BUF_SIZE 512
typedef struct {
    char buffer[RING_BUF_SIZE];
    size_t head;
    size_t tail;
    size_t count;
} ring_buf_t;

// 全局变量
static ring_buf_t s_uart_ring_buf;
static volatile auto_state_t s_auto_state = AUTO_STATE_CHECKING;
static int64_t s_last_update_time = 0;   // 缓冲区最后更新的时间戳
static int64_t s_last_execute_time = 0;  // 上一次成功/尝试执行的时间戳
static int s_frequent_err_count = 0;     // 触发过于频繁的计数器

// 双向输出状态函数
static void report_auto_status(bool is_ok, const char* msg) {
    // 扩大到 512 字节，防范长日志溢出截断（针对改进 3）
    char format_buf[512]; 
    if (is_ok) {
        snprintf(format_buf, sizeof(format_buf), "\r\n" ANSI_GRN "[ESP32-AUTO-OK] %s" ANSI_RST "\r\n", msg);
        ESP_LOGI(TAG, "%s", msg);
    } else {
        snprintf(format_buf, sizeof(format_buf), "\r\n" ANSI_RED "[ESP32-AUTO-ERR] %s" ANSI_RST "\r\n", msg);
        ESP_LOGE(TAG, "%s", msg);
    }
    // 同时输出到 UART 本地和网页端
    uart_write_bytes(UART_PORT_NUM, format_buf, strlen(format_buf));
    ws_broadcast_binary(format_buf, strlen(format_buf));
}

// ==================== 环形缓冲区基础操作 ====================
static void ring_buf_init(ring_buf_t *rb) {
    memset(rb, 0, sizeof(ring_buf_t));
}

static void ring_buf_push(ring_buf_t *rb, char c) {
    rb->buffer[rb->head] = c;
    rb->head = (rb->head + 1) % RING_BUF_SIZE;
    if (rb->count < RING_BUF_SIZE) {
        rb->count++;
    } else {
        // 缓冲区满，丢弃最旧的部分 (满足需求 1)
        rb->tail = (rb->tail + 1) % RING_BUF_SIZE;
    }
}

// 将当前环形缓冲区中的内容导出为线性的字符串
static void ring_buf_to_str(ring_buf_t *rb, char *dest, size_t max_len) {
    size_t t = rb->tail;
    size_t i = 0;
    while (i < rb->count && i < (max_len - 1)) {
        dest[i++] = rb->buffer[t];
        t = (t + 1) % RING_BUF_SIZE;
    }
    dest[i] = '\0'; // 强制尾部截断隔离
}

// ==================== 核心逻辑支撑函数 ====================

/**
 * @brief 判断环形缓冲区的末尾是否以指定的提示符结尾 (满足需求 2)
 */
static bool ring_buf_ends_with(ring_buf_t *rb, const char *suffix) {
    size_t suffix_len = strlen(suffix);
    if (rb->count < suffix_len) return false;

    size_t t = (rb->head + RING_BUF_SIZE - suffix_len) % RING_BUF_SIZE;
    for (size_t i = 0; i < suffix_len; i++) {
        if (rb->buffer[t] != suffix[i]) return false;
        t = (t + 1) % RING_BUF_SIZE;
    }
    return true;
}

/**
 * @brief 等待特定关键字（通过持续读取 UART 更新缓冲区来匹配命令结果）
 */
static bool wait_for_keyword(const char* expected, uint32_t timeout_ms) {
    int64_t start_time = esp_timer_get_time();
    uint32_t timeout_us = timeout_ms * 1000;
    uint8_t ch;
    // 设为大小 +1，完美防止 ring_buf_to_str 转换时的溢出（针对改进 1）
    char temp_linear_buf[RING_BUF_SIZE + 1]; 

    while ((esp_timer_get_time() - start_time) < timeout_us) {
        int len = uart_read_bytes(UART_PORT_NUM, &ch, 1, pdMS_TO_TICKS(10));
        if (len > 0) {
            ring_buf_push(&s_uart_ring_buf, (char)ch);
            s_last_update_time = esp_timer_get_time(); // 更新最新数据流时间戳
            
            // 转换为线性字符串进行匹配
            ring_buf_to_str(&s_uart_ring_buf, temp_linear_buf, sizeof(temp_linear_buf));
            if (strstr(temp_linear_buf, expected) != NULL) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

/**
 * @brief 执行自定义命令序列
 */
static void execute_uboot_sequence(void) {
    s_auto_state = AUTO_STATE_EXECUTING; // 标记进入执行期间，阻止循环重复触发 (满足需求 2)
    report_auto_status(true, "U-Boot environment confirmed. Executing sequence...");

    bool all_success = true;
    for (size_t i = 0; i < CMD_COUNT; i++) {
        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf), "Sending command [%d/%d]: %s", (int)i+1, (int)CMD_COUNT, my_cmd_sequence[i].command);
        report_auto_status(true, log_buf);

        // 发送命令
        uart_write_bytes(UART_PORT_NUM, my_cmd_sequence[i].command, strlen(my_cmd_sequence[i].command));

        // 等待上一条结束
        if (!wait_for_keyword(my_cmd_sequence[i].expected_keyword, my_cmd_sequence[i].timeout_ms)) {
            snprintf(log_buf, sizeof(log_buf), "Command %s failed (Timeout). Interrupted.", my_cmd_sequence[i].command);
            report_auto_status(false, log_buf);
            all_success = false;
            break; // 出错不继续下一条 (满足需求 2)
        }
    }

    if (all_success) {
        report_auto_status(true, "All automated custom commands completed successfully.");
    } else {
        // 如果中间卡住或失败，停留在 U-Boot 等待人工干预 (满足需求 5)
        report_auto_status(false, "Sequence halted. Waiting for retry or manual intervention."); 
    }

    // 无论成功还是出错，结束执行后重新回到检测环节 (满足需求 2)
    s_auto_state = AUTO_STATE_CHECKING;
}

// ==================== 主任务与控制接口 ====================

/**
 * @brief 自动交互主监控任务
 */
static void auto_interact_task(void *pvParameters) {
    uint8_t ch;
    ring_buf_init(&s_uart_ring_buf);
    s_last_update_time = esp_timer_get_time();
    s_auto_state = AUTO_STATE_CHECKING;

    while (1) {
        // 1. 如果处于人工锁死状态，挂起任务，不再进行任何检测
        if (s_auto_state == AUTO_STATE_MANUAL_LOCKED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 2. 如果正在执行命令期间，主循环让出 CPU
        if (s_auto_state == AUTO_STATE_EXECUTING) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 3. 流式读取 UART 数据 (满足需求 1)
        int len = uart_read_bytes(UART_PORT_NUM, &ch, 1, pdMS_TO_TICKS(20));
        if (len > 0) {
            ring_buf_push(&s_uart_ring_buf, (char)ch);
            s_last_update_time = esp_timer_get_time(); // 有数据更新，刷新时间戳
        } else {
            // 4. UART 没收到数据更新，检查是否已经持续静止了 10 秒 (满足需求 2)
            int64_t now = esp_timer_get_time();
            if ((now - s_last_update_time) >= ((int64_t)UBOOT_TIMEOUT_MS * 1000)) {
                
                // 5. 判断缓冲区最后的字符是否等于 U-Boot 提示符 (满足需求 2)
                if (ring_buf_ends_with(&s_uart_ring_buf, UBOOT_PROMPT)) {
                    
                    // 6. 满足静止和提示符，进行防御性频率锁死检查 (满足需求 3)
                    if (s_last_execute_time > 0 && (now - s_last_execute_time) < MIN_INTERVAL_US) {
                        s_frequent_err_count++;
                        if (s_frequent_err_count >= MAX_FREQ_ERR_COUNT) {
                            s_auto_state = AUTO_STATE_MANUAL_LOCKED;
                            report_auto_status(false, "Triggered too frequently! FREQUENCY LOCK active. Need manual reset.");
                            continue;
                        }
                    } else {
                        // 超过2分钟没有发生，或者是首次触发，清空频率计数
                        if ((now - s_last_execute_time) >= MIN_INTERVAL_US) {
                            s_frequent_err_count = 0;
                        }
                    }
                    s_last_execute_time = now; // 更新本次触发尝试的时间戳

                    // 7. 使用 version 命令再确认 (满足需求 2)
                    report_auto_status(true, "U-Boot prompt detected. Sending 'version' to double check...");
                    uart_write_bytes(UART_PORT_NUM, VERSION_CMD, strlen(VERSION_CMD));
                    
                    // 清除缓冲避免旧数据干扰 version 关键字匹配
                    ring_buf_init(&s_uart_ring_buf); 
                    
                    if (wait_for_keyword(VERSION_EXPECTED, 3000)) {
                        // 确认成功，开始输入自定义命令集合
                        execute_uboot_sequence();
                    } else {
                        report_auto_status(false, "Double check 'version' failed. Fake prompt or mismatched environment.");
                        // 非预期提示符（如进入了内核或Linux系统），不满足确认，不会触发自定义命令 (满足需求 4)
                    }
                }
                
                // 重置时间戳，防止在无新数据时每个监控循环都高频触发检测
                s_last_update_time = esp_timer_get_time();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1)); 
    }
}

/**
 * @brief 初始化自动交互模块（在应用启动时调用一次）
 */
void auto_interact_init(void) {
    // 针对改进 2：将栈深度从 4096 调整至 6144，绝对防范网络同步发送产生的栈溢出
    xTaskCreate(auto_interact_task, "auto_interact_task", 6144, NULL, 5, NULL);
}

/**
 * @brief 清空状态/解锁接口 (满足需求 6：可由前端网页按钮直接调用)
 */
void clear_auto_interact_status(void) {
    s_frequent_err_count = 0;
    s_last_execute_time = 0;
    ring_buf_init(&s_uart_ring_buf);
    s_last_update_time = esp_timer_get_time();
    
    if (s_auto_state == AUTO_STATE_MANUAL_LOCKED) {
        s_auto_state = AUTO_STATE_CHECKING;
        report_auto_status(true, "MANUAL OVERRIDE: System unlocked, restarted auto-detection.");
    } else {
        report_auto_status(true, "Status cleared and ring buffer reset.");
    }
}
