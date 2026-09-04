/*
 * wifi_mgr.h — WiFi STA 连接管理
 *
 * 负责：初始化 netif/event/wifi，自动连接，断线退避重连。
 * 暴露两个查询接口，供主流程在“上传前等网络就绪”使用。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化并异步开始连接（不阻塞）；内部起一个重连任务 */
esp_err_t wifi_mgr_start(void);

/* 等待连接就绪（拿到 IP），最多等 timeout_ms 毫秒；返回是否就绪 */
bool wifi_mgr_wait_connected(uint32_t timeout_ms);

/* 当前是否已拿到 IP */
bool wifi_mgr_is_connected(void);

#ifdef __cplusplus
}
#endif
