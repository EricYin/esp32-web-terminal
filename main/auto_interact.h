#ifndef AUTO_INTERACT_H
#define AUTO_INTERACT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 初始化自动交互模块，并启动检测任务
void auto_interact_init(void);

// 外部同步调用：通知有新输出输入。必须极快！
void auto_interact_notify_output(const uint8_t *data, size_t len);

// 提供给 Web 页面/外部 API 调用的清除错误锁定状态的函数
void auto_interact_clear_lockout(void);

// 获取当前锁定状态（用于前端页面/API 展示）
bool auto_interact_is_locked_out(void);

#endif // AUTO_INTERACT_H
