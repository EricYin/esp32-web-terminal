#include "auto_interact.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "uart_bridge.h"
#include "config.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"


#define TAG "AUTO_INTERACT"

#define BUFFER_SIZE_LOG2   12   // 4096 字节环形缓冲区
#define BUFFER_SIZE         (1 << BUFFER_SIZE_LOG2)
#define BUFFER_MASK         (BUFFER_SIZE - 1)

static const int64_t ALLOWED_TWO_AUTO_CMD_INTERVAL = (120LL * 1000 * 1000); //2 min
static const int PORT_INDEX = 0;

// 配置您目前的 U-Boot 自动化需求
static const auto_interact_cmd_t cmd = {
    .target_prompt = "uboot >",
    .validate_cmd = "version\n",
    .validate_cmd_resp = "uboot >", 
    .exec_cmds =  {
                    "setenv serverip 192.168.1.100\n",
                    "tftpboot 0x81000000 op.bin\n",
                    "bootm 0x81000000\n"
                  },
    .exec_cmds_timeout_s = {5, 300, 20},
    .error_keywords = {"err", "error", "fail", "exception", "not found"},
    .error_keywords_count = 5,
    .cmd_count = 3
};

static auto_interact_config_t conf = {
    .locked_out = true, // 熔断锁定标志位
    .check_interval_s = 10,
    .cmd = cmd
};

static auto_interact_config_t finalConf;

// 环形缓冲区结构
typedef struct {
    uint8_t storage[BUFFER_SIZE];
    uint32_t head; // 仅由 NotifyOutput (单写者) 递增
    uint32_t count;
} ring_buf_t;

// 状态控制
static ring_buf_t g_rb;
static int64_t g_last_update_us = 0;   // 上次收到串口输出的时间戳
static int64_t g_last_exec_us = 0;     // 上次执行命令集的时间戳
static uint32_t g_lockout_counter = 0; // 连续高频触发计数器

static SemaphoreHandle_t conf_mutex = NULL;
static bool storage_ready = false;
static bool persisted_locked_out = true;
static bool initialized = false;

#define AUTO_CONFIG_PATH "/storage/auto_interact.bin"
#define AUTO_CONFIG_TEMP_PATH "/storage/auto_interact.tmp"
#define AUTO_CONFIG_BACKUP_PATH "/storage/auto_interact.bak"
#define AUTO_CONFIG_MAGIC 0x41494346U
#define AUTO_CONFIG_VERSION 1U

static esp_err_t auto_interact_config_read_file(const char *path, auto_interact_config_t *config);

static bool auto_interact_config_valid(const auto_interact_config_t *config)
{
    const auto_interact_cmd_t *commands = &config->cmd;
    if (config->check_interval_s == 0 || config->check_interval_s > UINT32_MAX / 1000U ||
        commands->cmd_count == 0 || commands->cmd_count > CONFIG_AUTO_INTERACT_EXEC_CMD_COUNT_MAX_LEN ||
        commands->error_keywords_count > CONFIG_AUTO_INTERACT_ERROR_KEYWORD_COUNT_MAX_LEN) return false;

    if (!commands->target_prompt[0] || !memchr(commands->target_prompt, 0, sizeof(commands->target_prompt)) ||
        !commands->validate_cmd[0] || !memchr(commands->validate_cmd, 0, sizeof(commands->validate_cmd)) ||
        !commands->validate_cmd_resp[0] || !memchr(commands->validate_cmd_resp, 0, sizeof(commands->validate_cmd_resp))) return false;

    for (size_t index = 0; index < commands->cmd_count; index++) {
        if (!commands->exec_cmds[index][0] ||
            !memchr(commands->exec_cmds[index], 0, sizeof(commands->exec_cmds[index])) ||
            commands->exec_cmds_timeout_s[index] == 0 ||
            commands->exec_cmds_timeout_s[index] > UINT32_MAX / 1000U) return false;
    }
    for (size_t index = 0; index < commands->error_keywords_count; index++) {
        if (!commands->error_keywords[index][0] ||
            !memchr(commands->error_keywords[index], 0, sizeof(commands->error_keywords[index]))) return false;
    }
    return true;
}

static esp_err_t auto_interact_storage_init(void)
{
    esp_vfs_spiffs_conf_t storage = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 2,
        .format_if_mount_failed = false,
    };

    if (esp_spiffs_mounted("storage")) {
        ESP_LOGI(TAG, "esp_spiffs_mounted done");
        return ESP_OK;
    }
    else {
        ESP_LOGE(TAG, "esp_spiffs_mounted not done, start to mount");
    }

    esp_err_t result = esp_vfs_spiffs_register(&storage);
    if (result != ESP_FAIL)  {
        ESP_LOGI(TAG, "esp_vfs_spiffs_register done");
        return result;
    }
    else {
         ESP_LOGE(TAG, "esp_vfs_spiffs_register not done, start to register");
    }

    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!partition) { 
        ESP_LOGE(TAG, "esp_partition_find_first failed");
        return ESP_ERR_NOT_FOUND;
    }
    else {
        ESP_LOGI(TAG, "esp_partition_find_first done, start to read partition");
    }

    uint8_t bytes[256];
    for (size_t offset = 0; offset < partition->size; offset += sizeof(bytes)) {
        size_t length = partition->size - offset;
        if (length > sizeof(bytes)) length = sizeof(bytes);
        result = esp_partition_read(partition, offset, bytes, length);
        if (result != ESP_OK) {  
            ESP_LOGE(TAG, "esp_partition_read failed");
            return result;
        }
        for (size_t index = 0; index < length; index++) {
            if (bytes[index] != 0xff) {
                ESP_LOGE(TAG, "failed: storage data is not 0xff");
                return ESP_FAIL;
            }
        }
    }
    storage.format_if_mount_failed = true;
    return esp_vfs_spiffs_register(&storage);
}

static esp_err_t auto_interact_config_write(const auto_interact_config_t *config)
{
    uint32_t header[] = {
        AUTO_CONFIG_MAGIC, AUTO_CONFIG_VERSION, sizeof(*config),
        esp_rom_crc32_le(0, (const uint8_t *)config, sizeof(*config)),
    };
    FILE *file = fopen(AUTO_CONFIG_TEMP_PATH, "wb");
    if (!file) {
        ESP_LOGE(TAG, "open to write AUTO_CONFIG_TEMP_PATH failed");
        return ESP_FAIL;
    } 
    ESP_LOGI(TAG, "Writing header (magic=%08lx, version=%lu, size=%lu) to %s",
             (unsigned long)AUTO_CONFIG_MAGIC, (unsigned long)AUTO_CONFIG_VERSION,
             (unsigned long)sizeof(*config), AUTO_CONFIG_TEMP_PATH);
    if (fwrite(header, sizeof(header), 1, file) != 1) {
        ESP_LOGE(TAG, "fwrite(header) failed");
        goto failed;
    }
    ESP_LOGI(TAG, "Header written OK (%zu bytes)", sizeof(header));

    ESP_LOGI(TAG, "Writing config struct (%zu bytes)...", sizeof(*config));
    if (fwrite(config, sizeof(*config), 1, file) != 1) {
        ESP_LOGE(TAG, "fwrite(config) failed");
        goto failed;
    }
    ESP_LOGI(TAG, "Config written OK");

    ESP_LOGI(TAG, "Flushing file...");
    if (fflush(file) != 0) {
        ESP_LOGE(TAG, "fflush() failed: %s", strerror(errno));
        goto failed;
    }
    ESP_LOGI(TAG, "fflush OK");

    // Note: fsync() is not supported by SPIFFS on ESP-IDF; fflush() is sufficient.

    ESP_LOGI(TAG, "Closing file...");
    int close_result = fclose(file);
    file = NULL;
    if (close_result != 0) {      
        ESP_LOGE(TAG, "fclose() failed: %s", strerror(errno));
        goto failed;
    }
    ESP_LOGI(TAG, "File closed OK");

    auto_interact_config_t *previous = malloc(sizeof(*previous));
    if (!previous) {
        ESP_LOGE(TAG, "malloc auto_interact_config_t failed: ESP_ERR_NO_MEM");
        remove(AUTO_CONFIG_TEMP_PATH);
        return ESP_ERR_NO_MEM;
    }
    bool primary_valid = auto_interact_config_read_file(AUTO_CONFIG_PATH, previous) == ESP_OK;
    free(previous);
    if (primary_valid) {
        if (remove(AUTO_CONFIG_BACKUP_PATH) != 0 && errno != ENOENT) {
            ESP_LOGE(TAG, "remove(AUTO_CONFIG_BACKUP_PATH) failed");
            goto failed;
        }
        if (rename(AUTO_CONFIG_PATH, AUTO_CONFIG_BACKUP_PATH) != 0) {
            ESP_LOGE(TAG, "rename(AUTO_CONFIG_PATH, AUTO_CONFIG_BACKUP_PATH) failed");
            goto failed;
        }
    } else {
        if (remove(AUTO_CONFIG_PATH) != 0 && errno != ENOENT) {
            ESP_LOGE(TAG, "remove(AUTO_CONFIG_PATH) failed");
            goto failed;
        }
    }
    if (rename(AUTO_CONFIG_TEMP_PATH, AUTO_CONFIG_PATH) != 0) {
        if (primary_valid && rename(AUTO_CONFIG_BACKUP_PATH, AUTO_CONFIG_PATH) != 0) {
            ESP_LOGE(TAG, "Configuration replacement failed; previous configuration remains in backup");
        }
        else {
            ESP_LOGE(TAG, "rename(AUTO_CONFIG_TEMP_PATH, AUTO_CONFIG_PATH) failed");
        }

        goto failed;
    }
    return ESP_OK;

failed:   
    if (file) {
        fclose(file);
    }
    remove(AUTO_CONFIG_TEMP_PATH);
    return ESP_FAIL;
}

static esp_err_t auto_interact_config_read_file(const char *path, auto_interact_config_t *config)
{
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    uint32_t header[4];
    bool valid = fread(header, sizeof(header), 1, file) == 1 &&
        header[0] == AUTO_CONFIG_MAGIC && header[1] == AUTO_CONFIG_VERSION &&
        header[2] == sizeof(*config) && fread(config, sizeof(*config), 1, file) == 1;
    if (valid) valid = fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0) valid = false;
    if (!valid || header[3] != esp_rom_crc32_le(0, (const uint8_t *)config, sizeof(*config))) return ESP_FAIL;
    uint8_t lock_value;
    memcpy(&lock_value, &config->locked_out, sizeof(lock_value));
    if (lock_value > 1 || !auto_interact_config_valid(config)) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t auto_interact_config_read(auto_interact_config_t *config)
{
    esp_err_t primary_result = auto_interact_config_read_file(AUTO_CONFIG_PATH, config);
    if (primary_result == ESP_OK) {
        ESP_LOGI(TAG, "primary_result from auto_interact_config_read_file done");
        return ESP_OK;
    }
    else {
        ESP_LOGE(TAG, "primary_result failed");
    }
    esp_err_t backup_result = auto_interact_config_read_file(AUTO_CONFIG_BACKUP_PATH, config);
    if (backup_result == ESP_OK) {
        ESP_LOGW(TAG, "Recovered configuration from backup");
        return ESP_OK;
    }
    if (primary_result == ESP_ERR_NOT_FOUND && backup_result == ESP_ERR_NOT_FOUND) { 
        ESP_LOGE(TAG, "auto_interact_config_read_file: not found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGE(TAG, "auto_interact_config_read_file failed");
    return ESP_FAIL;
}

static void init_conf_mutex(void) {
    if (conf_mutex == NULL) {
        conf_mutex = xSemaphoreCreateMutex();
    }
}

// 新增：发送命令前强行复位缓冲区，防止读到上一次交互残留下来的旧提示符
void auto_interact_reset_buffer(void) {
    g_rb.head = 0;
    g_rb.count = 0;
    memset(g_rb.storage, 0, BUFFER_SIZE);
}

// 1. 同步快速通知函数：无锁、无移动、极快
void auto_interact_rx_notify(const uint8_t *data, size_t len) {
    if (!initialized) return;
    if (!data || len == 0) return;
    
    // 快速将数据推入环形缓冲区，满则直接覆盖旧数据
    uint32_t local_head = g_rb.head;
    for (size_t i = 0; i < len; i++) {
        g_rb.storage[local_head & BUFFER_MASK] = data[i];
        local_head++;
    }
    g_rb.head = local_head; 
    if (len >= BUFFER_SIZE - g_rb.count) {
        g_rb.count = BUFFER_SIZE;
    } else {
        g_rb.count += (uint32_t)len;
    }
    
    // 更新最后收到输出的时间
    g_last_update_us = esp_timer_get_time();
}


esp_err_t auto_interact_config_set(auto_interact_config_t config, bool update_lock) {
    if (!initialized || conf_mutex == NULL) return ESP_ERR_INVALID_STATE;

    BaseType_t taken = xSemaphoreTake(conf_mutex, portMAX_DELAY);
    configASSERT(taken == pdTRUE);

    if (!storage_ready) {
        xSemaphoreGive(conf_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    auto_interact_config_t updated = finalConf;
    if (update_lock) {
        updated.locked_out = config.locked_out;
    } else {
        updated.check_interval_s = config.check_interval_s;
        updated.cmd = config.cmd;
    }
    if (!auto_interact_config_valid(&updated)) {
        xSemaphoreGive(conf_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    config = updated;
    if (!update_lock) config.locked_out = persisted_locked_out;
    esp_err_t result = auto_interact_config_write(&config);
    if (result != ESP_OK) {
        xSemaphoreGive(conf_mutex);
        ESP_LOGE(TAG, "Failed to save auto interaction configuration");
        return result;
    }
    persisted_locked_out = config.locked_out;
    finalConf = updated;
    if (update_lock) {
        if (finalConf.locked_out) {
            ESP_LOGI(TAG, "Lockout status is set manually.");
        } else {
            g_lockout_counter = 0;
            ESP_LOGI(TAG, "Lockout status cleared manually.");
        }
    }
    
    //trigger prompt by sending validation cmd if not locked
    if (!finalConf.locked_out) {
        esp_err_t send_result = uart_bridge_send(PORT_INDEX, (const uint8_t*)finalConf.cmd.validate_cmd, strlen(finalConf.cmd.validate_cmd));       
        if (send_result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send validation command after config set. Error: %d", send_result);
        }   
    }
    
    xSemaphoreGive(conf_mutex);

    return ESP_OK;
}

auto_interact_config_t auto_interact_config_get(void)
{
    if (!initialized || conf_mutex == NULL) {
        auto_interact_config_t fallback = conf;
        fallback.locked_out = true;
        return fallback;
    }

    BaseType_t taken = xSemaphoreTake(conf_mutex, portMAX_DELAY);
    configASSERT(taken == pdTRUE);

    auto_interact_config_t ret = finalConf;
    xSemaphoreGive(conf_mutex);
    return ret;
}

// 【新增】将环形缓冲区内容完整线性化复制到临时 buffer 中，方便进行全局关键字检索
static void rb_snapshot_to_linear(char *dest, size_t *out_len) {
    uint32_t current_head = g_rb.head;
    size_t len = g_rb.count;
    
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
static bool wait_for_response(const char* expected_prompt, const char* expected_resp, char error_keywords[][CONFIG_AUTO_INTERACT_ERROR_KEYWORD_MAX_LEN], size_t error_keywords_count, uint32_t wait_timeout_ms, uint32_t timeout_ms, bool is_last_cmd) {

    // 稍作延时，错开刚输入的命令自身
    vTaskDelay(pdMS_TO_TICKS(wait_timeout_ms));
    //return true;

    size_t prompt_len = strlen(expected_prompt);
    char *tail_buf = malloc(prompt_len + 1);
    if (!tail_buf) return false;
    
    bool seen_prompt_at_tail = false;
    int64_t start = esp_timer_get_time();   
    int64_t now = esp_timer_get_time();
    int64_t delta = (now - start);
    int64_t max = (1000LL * timeout_ms);
    // 非阻塞轮询：检查当前环形缓冲区的“最末尾”是否为指定的提示符
    while (delta < max) {
        size_t available = g_rb.count;
        if (available >= prompt_len) {
            rb_read_tail(tail_buf, prompt_len);
            ESP_LOGI(TAG, "rb_read_tail for response: '%s'", tail_buf);
            if (strcmp(tail_buf, expected_prompt) == 0) {
                seen_prompt_at_tail = true;
                break; // 看到提示符为结尾，立刻退出
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); 
        now = esp_timer_get_time();
        delta = (now - start);
        ESP_LOGI(TAG, "now: %" PRId64 ", delta: %" PRId64 ", max: %" PRId64, now, delta, max);
    }
    
    //check it last time
    if (!seen_prompt_at_tail && g_rb.count >= prompt_len) {
        rb_read_tail(tail_buf, prompt_len);
        seen_prompt_at_tail = strcmp(tail_buf, expected_prompt) == 0;
    }
    free(tail_buf);

    // 全局错误字串提取与检索
    char *linear_buf = malloc(BUFFER_SIZE + 1);
    if (!linear_buf) return false;
    
    size_t current_len = 0;
    rb_snapshot_to_linear(linear_buf, &current_len);
    
    bool has_error = false;

    if (expected_resp != NULL && strcasestr_custom(linear_buf, expected_resp) == NULL) {
        has_error = true;
        ESP_LOGE(TAG, "Expected response key text not found: %s", expected_resp);
        free(linear_buf);
        return false;
    }
    
    // 使用循环检查缓冲区是否包含任意一个关键字
    for (size_t i = 0; i < error_keywords_count; i++) {
        if (strcasestr_custom(linear_buf, error_keywords[i]) != NULL) {
            ESP_LOGE(TAG, "Error key text %s found in %s", error_keywords[i], linear_buf);
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

esp_err_t auto_interact_run_cmd(auto_interact_cmd_t cmd) {
    //发送验证命令（例如 version）
    auto_interact_reset_buffer();
    ESP_LOGI(TAG, "Sending validation command...");
    esp_err_t send_result = uart_bridge_send(PORT_INDEX, (const uint8_t*)cmd.validate_cmd, strlen(cmd.validate_cmd));       
    if (send_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send validation command. Error: %d", send_result);
        return ESP_FAIL;
    }   
    // 检查验证是否通过
    if (!wait_for_response(cmd.target_prompt, cmd.validate_cmd_resp, cmd.error_keywords, cmd.error_keywords_count, 150, 5000, false)) {
        ESP_LOGE(TAG, "validation command failed. Error: %d", send_result);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Validation passed. Executing auto command sequence...");    
    // 顺序输入交互命令集合
    for (size_t i = 0; i < cmd.cmd_count; i++) {
        auto_interact_reset_buffer();
        ESP_LOGI(TAG, "Sending sequence [%zu/%zu]: %s", i + 1, cmd.cmd_count, cmd.exec_cmds[i]);
        send_result = uart_bridge_send(PORT_INDEX, (const uint8_t*)cmd.exec_cmds[i], strlen(cmd.exec_cmds[i]));                
        if (send_result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send command index %d. Error: %d", (int)i, send_result);
            return ESP_FAIL;
        }            
        
        // 【改动点】动态判定当前循环处理的是否为最后一条命令
        bool is_last = (i == (cmd.cmd_count - 1));
        
        // 【改动点】将 is_last 标志传入重构后的函数中
        uint32_t timeout = cmd.exec_cmds_timeout_s[i] * 1000U;
        if (!wait_for_response(cmd.target_prompt, NULL, cmd.error_keywords, cmd.error_keywords_count, 150, timeout, is_last)) {
            ESP_LOGE(TAG, "Command index %d failed or timed out unconditionally. Aborting sequence.", (int)i);
            return ESP_FAIL;
        }    
    }    

    ESP_LOGI(TAG, "All automated actions completed successfully.");
    return ESP_OK;
}

// 2. 核心检测与执行状态机 Task
static void auto_interact_task(void *pvParameters) {
    ESP_LOGI(TAG, "auto_interact_task started.");
    
    if (conf.locked_out) {
        ESP_LOGI(TAG, "auto_interact_task locked out.");
    }

    g_last_update_us = esp_timer_get_time();   

    while (1) {
        int64_t wait_start_us = esp_timer_get_time();
        do {
            vTaskDelay(pdMS_TO_TICKS(2000));
            conf = auto_interact_config_get();
        } while (esp_timer_get_time() - wait_start_us < (int64_t)conf.check_interval_s * 1000000LL);

        if (conf.locked_out) {
            continue; // 处于熔断锁定状态，跳过检测
        }
        
        int64_t check_interval_tick = (int64_t)conf.check_interval_s * 1000000LL;
        int64_t now = esp_timer_get_time();
        bool withinSlient = (now - g_last_update_us) <= check_interval_tick;        
        
        // 条件一：静默期检查（大于 10 秒钟没有收到任何新输入，说明疯狂输出已停止，处于稳定提示符状态）
        if (withinSlient) {
            continue;
        }    

        // 条件二：检查尾部字符是否精确匹配当前设定的提示符
        size_t prompt_len = strlen(conf.cmd.target_prompt);        
        char prompt_check_buf[sizeof(conf.cmd.target_prompt)];
        rb_read_tail(prompt_check_buf, prompt_len);        

        ESP_LOGI(TAG, "rb_read_tail output: '%s'", prompt_check_buf);
        bool not_expected_prompt = strcmp(prompt_check_buf, conf.cmd.target_prompt) != 0;
        if (not_expected_prompt) {
            continue;
        }

        ESP_LOGI(TAG, "Matched target prompt '%s'. Checking anti-damage counters...", conf.cmd.target_prompt);

        // 3. 硬件防损坏/无限重启环路熔断判定
        if (g_last_exec_us > 0 && (now - g_last_exec_us) < ALLOWED_TWO_AUTO_CMD_INTERVAL) {
            g_lockout_counter++;
            ESP_LOGW(TAG, "Consecutive quick execution loop detected. Counter: %ld/3", g_lockout_counter);
            if (g_lockout_counter >= 3) {
                conf.locked_out = true;
                xSemaphoreTake(conf_mutex, portMAX_DELAY);
                finalConf.locked_out = true;
                xSemaphoreGive(conf_mutex);
                ESP_LOGE(TAG, "CRITICAL: Hard protection active! Auto interact disabled to prevent device damage.");
                continue;
            }
        } 
        else {
            g_lockout_counter = 0; // 正常间隔，清零连续计数
        }       
        
        auto_interact_run_cmd(conf.cmd);     
        
        // 记录本次交互尝试的时间戳
        g_last_exec_us = now; 
    }
}

esp_err_t auto_interact_init(void) {
    init_conf_mutex();
    if (conf_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create config mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t storage_result = auto_interact_storage_init();
    storage_ready = storage_result == ESP_OK;
    if (storage_ready) {
        auto_interact_config_t loaded = {0};
        esp_err_t load_result = auto_interact_config_read(&loaded);
        if (load_result == ESP_OK) {
            conf = loaded;
            ESP_LOGI(TAG, "Loaded auto interaction configuration");
        } else if (load_result == ESP_ERR_NOT_FOUND) {
            storage_result = auto_interact_config_write(&conf);
            if (storage_result != ESP_OK) {
                conf.locked_out = true;
                ESP_LOGE(TAG, "Failed to save defaults; auto interaction remains locked");
            }
        } else {
            conf.locked_out = true;
            ESP_LOGW(TAG, "Invalid configuration files; using locked defaults");
        }
    } else {
        conf.locked_out = true;
        ESP_LOGE(TAG, "Configuration storage unavailable (%s); continuing with auto interaction locked",
                 esp_err_to_name(storage_result));
    }
    persisted_locked_out = conf.locked_out;
    finalConf = conf;
    
    memset(&g_rb, 0, sizeof(ring_buf_t));
    // 优先级 5（中等），绑定到 Core 1（避开 WiFi 核心 Core 0），分配 4KB 栈空间
   
    BaseType_t result;
    #if CONFIG_IDF_TARGET_ESP32S3
        //for esp32-s3
        result = xTaskCreatePinnedToCore(auto_interact_task, "auto_interact_task", 4096, NULL, 5, NULL, 1);
    #else
        //for esp32-c3/c6
        result = xTaskCreate( auto_interact_task, "auto_interact_task", 4096, NULL, 5, NULL);
    #endif

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto interact task");
        vSemaphoreDelete(conf_mutex);
        conf_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    initialized = true;
    return ESP_OK;
}
