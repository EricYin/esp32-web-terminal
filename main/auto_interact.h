#ifndef AUTO_INTERACT_H
#define AUTO_INTERACT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

// 初始化自动交互模块，并启动检测任务
esp_err_t auto_interact_init(void);

// 外部同步调用：通知有新输出输入。必须极快！
void auto_interact_rx_notify(const uint8_t *data, size_t len);

// 提供给 Web 页面/外部 API 调用的清除错误锁定状态的函数
esp_err_t auto_interact_config_set(auto_interact_config_t config, bool update_lock);

// 获取当前锁定状态（用于前端页面/API 展示）
auto_interact_config_t auto_interact_config_get(void);

#endif // AUTO_INTERACT_H
