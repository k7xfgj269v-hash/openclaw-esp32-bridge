/*
 * wifi_mgr.c — WiFi STA 连接 + 断线退避重连
 *
 * 线程模型：
 *   - esp_event 默认事件循环里跑 wifi_handler，只置位事件组、计数重试；
 *   - 独立 wifi_mgr 任务等待“重连触发位”，先退避再调 esp_wifi_connect，
 *     避免把 esp_wifi_connect 阻塞在事件任务里。
 *
 * 重试策略：首次立即连；每次断线重试次数 +1，退避 = 2^retry 秒（封顶 10s），
 * 永不放弃（适合语音陪伴这类常驻设备）；拿到 IP 后计数清零。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "sdkconfig.h"
#include "wifi_mgr.h"

#define TAG "wifi_mgr"

#define EV_CONNECTED  BIT0  /* 已拿到 IP */
#define EV_TRY        BIT1  /* 触发一次连接/重连 */

static EventGroupHandle_t s_evg;
static int s_retry = 0;     /* 连续重试次数，仅事件任务内写，任务内读，无需加锁 */

/* ------------------------------------------------------------------ */
/* 事件处理：跑在 esp_event 默认事件任务里，只做轻量操作                */
/* ------------------------------------------------------------------ */
static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* WiFi 驱动就绪，发起首次连接 */
            xEventGroupSetBits(s_evg, EV_TRY);
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            /* 断开：清“已连接”，安排重连 */
            ESP_LOGW(TAG, "WiFi 断开，安排第 %d 次重连", s_retry + 1);
            s_retry++;
            xEventGroupClearBits(s_evg, EV_CONNECTED);
            xEventGroupSetBits(s_evg, EV_TRY);
            break;

        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "已连上 WiFi，IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_evg, EV_CONNECTED);
    }
}

/* ------------------------------------------------------------------ */
/* 重连任务：退避后调 esp_wifi_connect                                */
/* ------------------------------------------------------------------ */
static void wifi_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_evg, EV_TRY, pdTRUE /*清位*/, pdFALSE, portMAX_DELAY);

        /* 退避：第 N 次重连前等 2^N 秒，封顶 10s（首次 s_retry=0 不等待） */
        if (s_retry > 0) {
            uint32_t delay_s = (s_retry <= 3) ? (1u << s_retry) : 10u;
            vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
        }

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            /* 可能因为正在连接/未启动而报错，等下一个事件再触发即可 */
            ESP_LOGE(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(err));
        }
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */
esp_err_t wifi_mgr_start(void)
{
    /* 网络接口与事件循环（幂等：重复调用会返回已有，可忽略） */
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        return ret;
    }

    s_evg = xEventGroupCreate();
    if (s_evg == NULL) {
        ESP_LOGE(TAG, "创建事件组失败");
        return ESP_ERR_NO_MEM;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,  wifi_handler, NULL, NULL);

    wifi_config_t wc = { 0 };
    /* 注意：ssid/password 都是 char[]，长度由结构体保证；用 snprintf 写更稳 */
    snprintf((char *)wc.sta.ssid,     sizeof(wc.sta.ssid),     "%s", CONFIG_CC_WIFI_SSID);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", CONFIG_CC_WIFI_PASSWORD);
    /* 按 WPA2 阈值选网：覆盖 WPA/WPA2/WPA3-PSK 混合的大多数家用路由。
     * 若目标是纯开放网络，把下面一行注释掉或改成 WIFI_AUTH_OPEN。 */
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    /* WPA3 纯 SAE 网络需要额外配置 wc.sta.sae_pwe_h2e，见 ESP-IDF 示例 */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 事件任务可能已把 EV_TRY 置位；任务创建后再取到也不丢（事件组有记忆） */
    xTaskCreate(wifi_task, "wifi_mgr", 3072, NULL, 5, NULL);
    return ESP_OK;
}

bool wifi_mgr_wait_connected(uint32_t timeout_ms)
{
    if (s_evg == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_evg, EV_CONNECTED,
                                           pdFALSE, /*等待期间不清位*/
                                           pdTRUE,  /*全部位（单比特等价）*/
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & EV_CONNECTED) != 0;
}

bool wifi_mgr_is_connected(void)
{
    if (s_evg == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_evg) & EV_CONNECTED) != 0;
}
