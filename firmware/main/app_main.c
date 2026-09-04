/*
 * app_main.c — OpenClaw 语音陪伴设备主流程
 *
 * 状态机（单任务顺序执行，无需复杂并发）：
 *
 *   IDLE ──按下 PTT──> REC(采 I2S 到 PCM 缓冲) ──松开──>
 *   SEND ──voice_client_begin(上传 PCM + 读响应头)──> free(PCM)
 *       ──voice_client_read_body(收 WAV)──> PLAY ──> IDLE
 *
 * 说明：
 *   - 一次只做一件事，录音时不会播放，天然避免自激/回授路径；
 *   - 内存：录音缓冲与响应 WAV 不同时持有（两段式 HTTP），见 voice_client.h。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "sdkconfig.h"
#include "board.h"
#include "wifi_mgr.h"
#include "i2s_audio.h"
#include "voice_client.h"

#define TAG "main"

#define PTT_DEBOUNCE_MS  40      /* 按键消抖 */
#define PTT_IDLE_POLL_MS 20

/* 录音缓冲：先按上限申请，内存不够自动减半，直到 ≥2s */
static bool alloc_pcm_buf(int16_t **buf, size_t *cap_bytes)
{
    int secs = CONFIG_CC_REC_MAX_SEC;
    while (secs >= 2) {
        size_t bytes = (size_t)secs * CC_AUDIO_BYTES_PER_SEC;
        int16_t *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p == NULL) {
            p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }
        if (p != NULL) {
            *buf = p;
            *cap_bytes = bytes;
            ESP_LOGI(TAG, "录音缓冲 %uKB ≈ %ds", (unsigned)(bytes / 1024), secs);
            return true;
        }
        secs /= 2;
    }
    ESP_LOGE(TAG, "录音缓冲分配失败（请减小 CC_REC_MAX_SEC 或加 PSRAM）");
    return false;
}

/* 等按键松开，保证回到 IDLE 时状态干净 */
static void wait_ptt_release(void)
{
    while (board_ptt_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(PTT_IDLE_POLL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* 一次完整的“按住说话 → 上传 → 播放回复” */
static void record_and_ask(void)
{
    int16_t *pcm = NULL;
    size_t cap = 0;

    if (!alloc_pcm_buf(&pcm, &cap)) {
        wait_ptt_release();
        return;
    }

    /* ---- 1. 录音（按住期间循环采，满了截断） ---- */
    audio_mic_start();
    size_t filled = 0;                 /* PCM 字节数 */
    while (board_ptt_pressed() && filled < cap) {
        size_t want = (cap - filled) / sizeof(int16_t);
        if (want > 256) {
            want = 256;                /* 每次约 16ms，松开响应及时 */
        }
        size_t got = 0;
        if (audio_mic_read(pcm + filled / sizeof(int16_t), want, &got) != ESP_OK) {
            break;
        }
        filled += got * sizeof(int16_t);
        if (got < want) {
            break;
        }
    }
    audio_mic_stop();
    wait_ptt_release();

    if (filled >= cap) {
        ESP_LOGW(TAG, "录音达到缓冲上限(%uKB)，已截断", (unsigned)(cap / 1024));
    }
    if (filled < CC_AUDIO_BYTES_PER_SEC / 4) {
        ESP_LOGW(TAG, "录音太短(%.0fms)，忽略", filled * 1000.0f / CC_AUDIO_BYTES_PER_SEC);
        free(pcm);
        return;
    }
    ESP_LOGI(TAG, "录音完成 %u 字节 (%.2fs)",
             (unsigned)filled, filled / (float)CC_AUDIO_BYTES_PER_SEC);

    /* ---- 2. 等 WiFi，就绪后上传 ---- */
    if (!wifi_mgr_wait_connected(15000)) {
        ESP_LOGE(TAG, "WiFi 未在 15s 内就绪，丢弃本次录音");
        free(pcm);
        return;
    }

    voice_req_t req = { 0 };
    esp_err_t err = voice_client_begin(&req, pcm, filled);
    free(pcm);                          /* 上传完，尽早释放给响应腾内存 */
    pcm = NULL;

    if (err != ESP_OK) {
        voice_client_abort(&req);
        return;
    }

    /* ---- 3. 读 WAV 响应并播放 ---- */
    uint8_t *wav = NULL;
    size_t wav_len = 0;
    err = voice_client_read_body(&req, &wav, &wav_len);
    if (err != ESP_OK) {
        voice_client_abort(&req);
        return;
    }

    if (req.reply[0] != '\0') {
        ESP_LOGI(TAG, "AI 回复: %s", req.reply);
    }
    if (wav != NULL && wav_len > 0) {
        audio_play_wav(wav, wav_len);
    } else {
        ESP_LOGW(TAG, "服务器返回空音频");
    }
    free(wav);
}

/* 初始化 PTT 按键（内部上拉，按下为低） */
static void ptt_gpio_init(void)
{
    gpio_config_t io = { 0 };
    io.pin_bit_mask = (1ULL << CONFIG_CC_PTT_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
}

/* 开机打印配置概览，占位默认值给出醒目警告 */
static void log_config_summary(void)
{
    ESP_LOGI(TAG, "=== 配置概览 ===");
    ESP_LOGI(TAG, "WiFi SSID: %s", CONFIG_CC_WIFI_SSID);
    ESP_LOGI(TAG, "服务器:   %s://%s:%d%s", CONFIG_CC_SERVER_SCHEME,
             CONFIG_CC_SERVER_HOST, CONFIG_CC_SERVER_PORT, CONFIG_CC_SERVER_PATH);
    ESP_LOGI(TAG, "鉴权:      %s", strlen(CONFIG_CC_AUTH_TOKEN) ? "带 Bearer token" : "无");
#if CONFIG_CC_TLS_SKIP_VERIFY
    ESP_LOGI(TAG, "TLS 校验:  跳过(自签调试)");
#else
    ESP_LOGI(TAG, "TLS 校验:  校验");
#endif
    ESP_LOGI(TAG, "PTT 键=GPIO%d, 麦克风 BCLK/WS/DIN=%d/%d/%d, 功放 BCLK/WS/DIN=%d/%d/%d",
             CONFIG_CC_PTT_GPIO,
             CONFIG_CC_MIC_BCLK, CONFIG_CC_MIC_WS, CONFIG_CC_MIC_DIN,
             CONFIG_CC_SPK_BCLK, CONFIG_CC_SPK_WS, CONFIG_CC_SPK_DIN);

    if (strcmp(CONFIG_CC_WIFI_SSID, "myssid") == 0 ||
        strcmp(CONFIG_CC_WIFI_PASSWORD, "mypassword") == 0) {
        ESP_LOGW(TAG, "WiFi 仍是占位默认值，请 menuconfig 修改");
    }
    if (strchr(CONFIG_CC_SERVER_HOST, '<') != NULL) {
        ESP_LOGW(TAG, "服务器地址仍是占位 <DE_IP>，请 menuconfig 填真实主机/IP");
    }
}

void app_main(void)
{
    /* NVS（WiFi 校准等需要） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ptt_gpio_init();

    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(wifi_mgr_start());

    log_config_summary();
    ESP_LOGI(TAG, "按住 PTT 开始说话…");

    /* 主循环：等按键 → 说话/上传/播放 */
    for (;;) {
        if (board_ptt_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(PTT_DEBOUNCE_MS));
            if (board_ptt_pressed()) {
                record_and_ask();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(PTT_IDLE_POLL_MS));
        }
    }
}
