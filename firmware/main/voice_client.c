/*
 * voice_client.c — 与仓库 openclaw_subagent_server.py 的 /voice 对接：
 *
 *   请求：POST /voice
 *         Content-Type: application/octet-stream
 *         可选 Authorization: Bearer <token>
 *         body = 裸 PCM（16kHz / 16bit / 单声道）
 *   响应：200 audio/wav（16k 单声道），头部 X-AI-Reply 为纯文本回复
 *
 * TLS 策略由 menuconfig 控制：CC_TLS_SKIP_VERIFY=y 跳过校验（自签调试）；
 * =n 时默认挂 esp_crt_bundle（系统根证书）。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"

#include "sdkconfig.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "voice_client.h"

#define TAG "voice"

/* 优先 PSRAM、回退内部 RAM 的大块分配 */
static void *alloc_big(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    }
    return p;
}

static void http_cleanup(voice_req_t *req)
{
    if (req->client) {
        esp_http_client_cleanup(req->client);
        req->client = NULL;
    }
    req->active = false;
}

esp_err_t voice_client_begin(voice_req_t *req, const int16_t *pcm, size_t pcm_bytes)
{
    memset(req, 0, sizeof(*req));
    if (pcm == NULL || pcm_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 组 URL：scheme://host:port/path，全部来自 menuconfig */
    char url[320];
    int n = snprintf(url, sizeof(url), "%s://%s:%d%s",
                     CONFIG_CC_SERVER_SCHEME,
                     CONFIG_CC_SERVER_HOST,
                     CONFIG_CC_SERVER_PORT,
                     CONFIG_CC_SERVER_PATH);
    if (n <= 0 || n >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "服务器地址非法/过长，请在 menuconfig 修改");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = { 0 };
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = CONFIG_CC_HTTP_TIMEOUT_MS;
    cfg.buffer_size = 2048;          /* 响应头缓冲够用即可，body 由我们手动读 */

#if CONFIG_CC_TLS_SKIP_VERIFY
    cfg.skip_cert_common_name_check = true;   /* 不给 CA 时即整体跳过校验 */
    ESP_LOGW(TAG, "TLS 证书校验已跳过（自签调试模式）");
#else
# if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ESP_LOGI(TAG, "TLS 将校验证书（系统根证书）");
# else
    ESP_LOGW(TAG, "未启用 mbedTLS Certificate Bundle，TLS 校验不可用");
# endif
#endif

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init 失败");
        return ESP_ERR_NO_MEM;
    }
    req->client = cli;
    req->active = true;

    esp_http_client_set_header(cli, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(cli, "Accept", "audio/wav");

    if (strlen(CONFIG_CC_AUTH_TOKEN) > 0) {
        char auth[200];
        int m = snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_CC_AUTH_TOKEN);
        if (m > 0 && m < (int)sizeof(auth)) {
            esp_http_client_set_header(cli, "Authorization", auth);
        } else {
            ESP_LOGW(TAG, "Token 过长，忽略 Authorization");
        }
    }

    /* 打开连接并带 Content-Length 发请求行/头 */
    if (esp_http_client_open(cli, (int)pcm_bytes) != ESP_OK) {
        ESP_LOGE(TAG, "esp_http_client_open 失败");
        http_cleanup(req);
        return ESP_FAIL;
    }

    /* 写完整 body（裸 PCM） */
    const char *body = (const char *)pcm;
    size_t sent = 0;
    while (sent < pcm_bytes) {
        size_t chunk = pcm_bytes - sent;
        if (chunk > 4096) {
            chunk = 4096;
        }
        int w = esp_http_client_write(cli, body + sent, (int)chunk);
        if (w <= 0) {
            ESP_LOGE(TAG, "上传中断: sent=%u/%u", (unsigned)sent, (unsigned)pcm_bytes);
            http_cleanup(req);
            return ESP_FAIL;
        }
        sent += (size_t)w;
    }
    ESP_LOGI(TAG, "已上传 %u 字节 PCM", (unsigned)pcm_bytes);

    /* 读响应头 */
    req->content_length = esp_http_client_fetch_headers(cli);
    req->status = esp_http_client_get_status_code(cli);

    char *hv = NULL;
    if (esp_http_client_get_header(cli, "X-AI-Reply", &hv) == ESP_OK && hv != NULL) {
        strncpy(req->reply, hv, sizeof(req->reply) - 1);
        req->reply[sizeof(req->reply) - 1] = '\0';
    }

    if (req->status < 200 || req->status >= 300) {
        ESP_LOGE(TAG, "服务器返回 HTTP %d", req->status);
        /* 读一小段错误正文便于排查（200 以外通常是小 JSON/文本） */
        char tmp[256];
        int r = esp_http_client_read(cli, tmp, sizeof(tmp) - 1);
        if (r > 0) {
            tmp[r] = '\0';
            ESP_LOGE(TAG, "错误正文: %s", tmp);
        }
        http_cleanup(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "响应 %d, Content-Length=%lld, AI: %s",
             req->status, (long long)req->content_length,
             req->reply[0] ? req->reply : "(空)");
    return ESP_OK;
}

esp_err_t voice_client_read_body(voice_req_t *req, uint8_t **out_wav, size_t *out_len)
{
    *out_wav = NULL;
    *out_len = 0;
    if (!req->active || req->client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_handle_t cli = req->client;
    esp_err_t ret = ESP_OK;
    uint8_t *buf = NULL;
    size_t total = 0;
    size_t cap = (size_t)CONFIG_CC_RESP_MAX_KB * 1024;

    if (req->content_length >= 0) {
        uint64_t cl = (uint64_t)req->content_length;
        if (cl > cap) {
            ESP_LOGE(TAG, "响应过大(%lluB>上限%uKB)，放弃",
                     (unsigned long long)cl, CONFIG_CC_RESP_MAX_KB);
            ret = ESP_ERR_INVALID_SIZE;
            goto out;
        }
        if (cl == 0) {
            goto out;   /* 空 body，成功但 wav 为空 */
        }
        buf = alloc_big((size_t)cl);
        if (buf == NULL) {
            ESP_LOGE(TAG, "分配响应缓冲失败(%uKB)", (unsigned)(cl / 1024));
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
        while (total < (size_t)cl) {
            size_t want = (size_t)cl - total;
            if (want > 65536) want = 65536;
            int r = esp_http_client_read(cli, (char *)buf + total, (int)want);
            if (r <= 0) {
                ESP_LOGE(TAG, "读取响应中断 r=%d(%u/%llu)", r,
                         (unsigned)total, (unsigned long long)cl);
                ret = ESP_FAIL;
                goto out;
            }
            total += (size_t)r;
        }
    } else {
        /* 理论上 BaseHTTPRequestHandler 总会给 Content-Length，这里兜底读满 cap */
        ESP_LOGW(TAG, "响应无 Content-Length，按流式读到 %uKB 上限", CONFIG_CC_RESP_MAX_KB);
        buf = alloc_big(cap);
        if (buf == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
        int r;
        while (total < cap &&
               (r = esp_http_client_read(cli, (char *)buf + total, (int)(cap - total))) > 0) {
            total += (size_t)r;
        }
    }

    *out_wav = buf;
    *out_len = total;
    buf = NULL;

out:
    if (buf) {
        free(buf);
    }
    http_cleanup(req);
    return ret;
}

void voice_client_abort(voice_req_t *req)
{
    if (req == NULL) {
        return;
    }
    http_cleanup(req);
}
