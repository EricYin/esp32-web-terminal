#ifndef AUTO_INTERACT_H
#define AUTO_INTERACT_H

#include <stdbool.h>
#include <stdint.h>

// 初始化自动化控制任务
void auto_interact_init(void);

// 流式喂入串口接收到的每个字符
void auto_interact_feed_char(char c);

// 用户通过网页终端输入（打字）时调用的解锁钩子
void auto_interact_web_input_hook(void);

#endif // AUTO_INTERACT_H
