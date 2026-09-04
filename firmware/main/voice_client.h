/*
 * voice_client.h — HTTPS POST /voice 客户端（裸 PCM 上传，WAV 接收）
 *
 * 两段式接口，目的是在读完响应头后立即释放上传用的 PCM 缓冲，
 * 避免“录音 PCM + 响应 WAV”同时占内存（无 PSRAM 的板子内存吃紧）。
 *
 * 流程：
 *   voice_client_begin(req, pcm, len)   // 发送完整 body，读回响应头
 *   ... 这里可以 free(pcm) ...
 *   voice_client_read_body(req, &wav, &wav_len)   // 分配并按长度读 body
 *   ... 播放 wav，free(wav) ...
 * 任一步出错都调 voice_client_abort(req) 收尾（重复调用安全）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_http_client_handle_t client;
    int         status;           /* HTTP 状态码 */
    int64_t     content_length;   /* 响应 Content-Length，-1=未知 */
    bool        active;           /* client 是否还持有连接 */
    char        reply[128];       /* X-AI-Reply 文本（最多 127 字符） */
} voice_req_t;

/* 上传 PCM(16k/16bit/mono)并读完响应头；成功返回 ESP_OK（此时 body 未读） */
esp_err_t voice_client_begin(voice_req_t *req, const int16_t *pcm, size_t pcm_bytes);

/* 读取 body 到新分配的堆缓冲（*wav，需 free），成功返回 ESP_OK */
esp_err_t voice_client_read_body(voice_req_t *req, uint8_t **wav, size_t *wav_len);

/* 中止/清理；begin 失败后调用也安全 */
void voice_client_abort(voice_req_t *req);

#ifdef __cplusplus
}
#endif
