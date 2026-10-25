#include "auto_interact.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "uart_bridge.h"


#define TAG "AUTO_INTERACT"

#define BUFFER_SIZE_LOG2   11   // 2048 字节环形缓冲区
#define BUFFER_SIZE         (1 << BUFFER_SIZE_LOG2)
#define BUFFER_MASK         (BUFFER_SIZE - 1)

// 策略配置结构体：便于未来扩展不同的交互模式（如 Uboot, Linux, Custom CLI）
typedef struct {
    const char* target_prompt;      // 目标提示符
    const char* validate_cmd;       // 验证命令
    const char* validate_response;  // 验证命令期待的响应包含内容（为空则仅校验再次出现提示符）
    const char** exec_cmds;         // 待执行的自定义命令数组
    size_t cmd_count;
} interact_profile_t;

// 配置您目前的 U-Boot 自动化需求
static const char* exec_cmds[] = {
    "setenv serverip 192.168.1.100\n",
    "tftpboot 0x81000000 op.bin\n",
    "bootm 0x81000000\n"
};

static const int PORT_INDEX = 0;
static const int64_t CHECK_INTERVAL = 10000;  //10 sec
static const int64_t CHECK_INTERVAL_TICK = (1000LL * CHECK_INTERVAL);
static const int64_t ALLOWED_TWO_AUTO_CMD_INTERVAL = (120LL * 1000 * 1000); //2 min
static const char *error_keywords[] = {"err", "error", "fail", "exception", "not found"};
static const size_t error_keywords_count = sizeof(error_keywords) / sizeof(error_keywords[0]);
    
static const interact_profile_t g_profile = {
    .target_prompt = "uboot >",
    .validate_cmd = "version\n",
    .validate_response = "uboot >", 
    .exec_cmds = exec_cmds,
    .cmd_count = sizeof(exec_cmds) / sizeof(exec_cmds[0])
};

// 环形缓冲区结构
typedef struct {
    uint8_t storage[BUFFER_SIZE];
    uint32_t head; // 仅由 NotifyOutput (单写者) 递增
} ring_buf_t;

// 状态控制
static ring_buf_t g_rb;
static int64_t g_last_update_us = 0;   // 上次收到串口输出的时间戳
static int64_t g_last_exec_us = 0;     // 上次执行命令集的时间戳
static uint32_t g_lockout_counter = 0; // 连续高频触发计数器
static bool g_is_locked_out = true;   // 熔断锁定标志位


// 新增：发送命令前强行复位缓冲区，防止读到上一次交互残留下来的旧提示符
void auto_interact_reset_buffer(void) {
    g_rb.head = 0;
    memset(g_rb.storage, 0, BUFFER_SIZE);
}

// 1. 同步快速通知函数：无锁、无移动、极快
void auto_interact_rx_notify(const uint8_t *data, size_t len) {
    if (!data || len == 0) return;
    
    // 快速将数据推入环形缓冲区，满则直接覆盖旧数据
    uint32_t local_head = g_rb.head;
    for (size_t i = 0; i < len; i++) {
        g_rb.storage[local_head & BUFFER_MASK] = data[i];
        local_head++;
    }
    g_rb.head = local_head; // 原子性更新
    
    // 更新最后收到输出的时间
    g_last_update_us = esp_timer_get_time();
}

// 设置或者清空锁定状态（人工干预）
void auto_interact_config_set(bool lock) {
    if (lock) {
        g_is_locked_out = true;
        ESP_LOGI(TAG, "Lockout status is set manually.");
    } else {
        g_lockout_counter = 0;
        g_is_locked_out = false;
        ESP_LOGI(TAG, "Lockout status cleared manually.");
    }
}

bool auto_interact_config_get(void) {
    return g_is_locked_out;
}

// 【新增】将环形缓冲区内容完整线性化复制到临时 buffer 中，方便进行全局关键字检索
static void rb_snapshot_to_linear(char *dest, size_t *out_len) {
    uint32_t current_head = g_rb.head;
    size_t len = current_head > BUFFER_SIZE ? BUFFER_SIZE : current_head;
    
    for (size_t i = 0; i < len; i++) {
        uint32_t idx = current_head - len + i;
        dest[i] = (char)g_rb.storage[idx & BUFFER_MASK];
    }
    dest[len] = '\0';
    if (out_len) *out_len = len;
}

// 【新增】不区分大小写的子串查找，用于匹配 err, fail, exception
static const char* strcasestr_custom(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            const char *h, *n;
            for (h = haystack, n = needle; *h && *n; h++, n++) {
                if (tolower((unsigned char)*h) != tolower((unsigned char)*n)) break;
            }
            if (!*n) return haystack;
        }
    }
    return NULL;
}

// 从环形缓冲区反向读取最后 N 个字符拼成线性字符串
static void rb_read_tail(char *dest, size_t n) {
    uint32_t current_head = g_rb.head;
    for (size_t i = 0; i < n; i++) {
        uint32_t idx = current_head - n + i;
        dest[i] = (char)g_rb.storage[idx & BUFFER_MASK];
    }
    dest[n] = '\0';
}


// 【核心重构】等待 U-Boot 响应函数
// 结束条件：看到控制台提示符比如 uboot 提示符为结尾；结束后检查缓冲区是否含有错误字串。
// 例外逻辑：最后一条命令超时视为成功；若超时且没有停留在当前控制台提示符比如 uboot 提示符则设为失败。
static bool wait_for_response(const char* expected_prompt, uint32_t wait_timeout_ms, uint32_t timeout_ms, bool is_last_cmd) {

    // 稍作延时，错开刚输入的命令自身
    vTaskDelay(pdMS_TO_TICKS(wait_timeout_ms));

    size_t prompt_len = strlen(expected_prompt);
    char *tail_buf = malloc(prompt_len + 1);
    if (!tail_buf) return false;
    
    int64_t start = esp_timer_get_time();
    bool seen_prompt_at_tail = false;
    
    // 非阻塞轮询：检查当前环形缓冲区的“最末尾”是否为指定的提示符
    while ((esp_timer_get_time() - start) < (1000LL * timeout_ms)) {
        uint32_t current_head = g_rb.head;
        size_t available = current_head > BUFFER_SIZE ? BUFFER_SIZE : current_head;
        
        if (available >= prompt_len) {
            rb_read_tail(tail_buf, prompt_len);
            if (strcmp(tail_buf, expected_prompt) == 0) {
                seen_prompt_at_tail = true;
                break; // 看到提示符为结尾，立刻退出
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // 降频检查
    }
    free(tail_buf);

    // 全局错误字串提取与检索
    char *linear_buf = malloc(BUFFER_SIZE + 1);
    if (!linear_buf) return false;
    
    size_t current_len = 0;
    rb_snapshot_to_linear(linear_buf, &current_len);
    
    bool has_error = false;
    
    // 使用循环检查缓冲区是否包含任意一个关键字
    for (size_t i = 0; i < error_keywords_count; i++) {
        if (strcasestr_custom(linear_buf, error_keywords[i]) != NULL) {
            has_error = true;
            break; // 只要匹配到一个错误关键字，立即判定为有错并退出循环
        }
    }
    
    free(linear_buf);

    // ---- 核心业务逻辑决策矩阵 ----
    if (seen_prompt_at_tail) {
        if (is_last_cmd) {
            ESP_LOGW(TAG, "Last command finished but still in current console env. Treated as FAIL.");
            return false; 
        } 
        else if (has_error) {
            ESP_LOGE(TAG, "Prompt matched at tail, but error keyword detected in buffer!");
            return false; // 包含错误关键字符串，失败
        } 
        else {
            return true; // 看到提示符为结尾且无错，成功
        }
    } else {
        // 未看到提示符（超时）的情况
        if (is_last_cmd) {
            ESP_LOGW(TAG, "Last command timed out. Treated as SUCCESS.");
            return true; // 例外：最后一条命令无错超时，视为成功
        } else {
            ESP_LOGE(TAG, "Command timed out and missed the prompt at tail.");
            return false; // 非最后一条命令超时，失败
        }
    }
}

// 2. 核心检测与执行状态机 Task
static void auto_interact_task(void *pvParameters) {
    ESP_LOGI(TAG, "auto_interact_task started.");
    
    if (g_is_locked_out) {
        ESP_LOGI(TAG, "auto_interact_task locked out.");
    }

    g_last_update_us = esp_timer_get_time();
    size_t prompt_len = strlen(g_profile.target_prompt);
    
    char *prompt_check_buf = malloc(prompt_len + 1);
    if (!prompt_check_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for task.");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL)); // 每秒轮询一次条件
        
        if (g_is_locked_out) {
            continue; // 处于熔断锁定状态，跳过检测
        }
        
        int64_t now = esp_timer_get_time();
        bool withinSlient = (now - g_last_update_us) <= CHECK_INTERVAL_TICK;
        
        // 条件一：静默期检查（大于 10 秒钟没有收到任何新输入，说明疯狂输出已停止，处于稳定提示符状态）
        if (withinSlient) {
            continue;
        }    

        // 条件二：检查尾部字符是否精确匹配当前设定的提示符
        rb_read_tail(prompt_check_buf, prompt_len);        

        ESP_LOGI(TAG, "DEBUG for rb_read_tail output: '%s'", prompt_check_buf);
        bool not_expected_prompt = strcmp(prompt_check_buf, g_profile.target_prompt) != 0;
        if (not_expected_prompt) {
            continue;
        }

        ESP_LOGI(TAG, "Matched target prompt '%s'. Checking anti-damage counters...", g_profile.target_prompt);

        // 3. 硬件防损坏/无限重启环路熔断判定
        if (g_last_exec_us > 0 && (now - g_last_exec_us) < ALLOWED_TWO_AUTO_CMD_INTERVAL) {
            g_lockout_counter++;
            ESP_LOGW(TAG, "Consecutive quick execution loop detected. Counter: %ld/3", g_lockout_counter);
            if (g_lockout_counter >= 3) {
                g_is_locked_out = true;
                ESP_LOGE(TAG, "CRITICAL: Hard protection active! Auto interact disabled to prevent device damage.");
                continue;
            }
        } else {
            g_lockout_counter = 0; // 正常间隔，清零连续计数
        }
        
        g_last_exec_us = now; // 记录本次交互尝试的时间戳
        
        // 条件三：发送二次验证命令（例如 version）
        auto_interact_reset_buffer();
        ESP_LOGI(TAG, "Sending validation command...");
        uart_bridge_send(PORT_INDEX, (const uint8_t*)g_profile.validate_cmd, strlen(g_profile.validate_cmd));       
       
        // 检查二次验证是否通过
        if (wait_for_response(g_profile.validate_response, 150, 3000, false)) {
            ESP_LOGI(TAG, "Validation passed. Executing auto command sequence...");
            
            bool seq_success = true;
            // 顺序输入交互命令集合
            for (size_t i = 0; i < g_profile.cmd_count; i++) {
                auto_interact_reset_buffer();
                ESP_LOGI(TAG, "Sending sequence [%d/%d]: %s", i + 1, g_profile.cmd_count, g_profile.exec_cmds[i]);
                uart_bridge_send(PORT_INDEX, (const uint8_t*)g_profile.exec_cmds[i], strlen(g_profile.exec_cmds[i]));                
                
                // 【改动点】动态判定当前循环处理的是否为最后一条命令
                bool is_last = (i == (g_profile.cmd_count - 1));
                
                // 【改动点】将 is_last 标志传入重构后的函数中
                if (!wait_for_response(g_profile.target_prompt, 100, 5000, is_last)) {
                    ESP_LOGE(TAG, "Command index %d failed or timed out unconditionally. Aborting sequence.", (int)i);
                    seq_success = false;
                    break; 
                }
            }
            
            if (seq_success) {
                ESP_LOGI(TAG, "All automated actions completed successfully.");
            }
        } else {
            ESP_LOGW(TAG, "Validation failed. Target environment did not reply as expected.");
        }       
        
        // 重置更新时间，强制下一次状态检测至少在 10s 之后
        // comment out because task delay is 10s
        //g_last_update_us = esp_timer_get_time();
      
    }
    free(prompt_check_buf);
}

void auto_interact_init(void) {
    memset(&g_rb, 0, sizeof(ring_buf_t));
    // 优先级 5（中等），绑定到 Core 1（避开 WiFi 核心 Core 0），分配 4KB 栈空间
   
    #if CONFIG_IDF_TARGET_ESP32S3
        //for esp32-s3
        xTaskCreatePinnedToCore(auto_interact_task, "auto_interact_task", 4096, NULL, 5, NULL, 1);
    #else
        //for esp32-c3/c6
        xTaskCreate( auto_interact_task, "auto_interact_task", 4096, NULL, 5, NULL);
    #endif
}
