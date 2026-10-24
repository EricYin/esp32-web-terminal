/**
 * @file auto_interact.h
 * @brief 工业级通用提示符自动交互模块（支持U-Boot/Shell/CLI自动应答）
 */

#ifndef AUTO_INTERACT_H
#define AUTO_INTERACT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化自动交互模块，分配资源并创建后台异步监听任务
 */
void auto_interact_init(void);

/**
 * @brief 外部串口接收拦截点。由 UART 驱动接收线程同步调用。
 * @note 本函数内部采用非阻塞无锁环形设计，执行效率极高（常数级别时间复杂度），绝不阻塞调用者。
 * @param data 接收到的原始字节流指针
 * @param len  字节流长度
 */
void auto_interact_notify_output(const uint8_t *data, size_t len);

/**
 * @brief 手工恢复接口。清空熔断触发计数器，解除锁定状态。
 */
void auto_interact_clear_lockout(void);

/**
 * @brief 获取当前自动化交互模块是否处于熔断锁定状态
 * @return true 处于锁定状态（自动化功能关闭）/ false 正常运行
 */
bool auto_interact_is_locked_out(void);

#ifdef __cplusplus
}
#endif

#endif // AUTO_INTERACT_H
