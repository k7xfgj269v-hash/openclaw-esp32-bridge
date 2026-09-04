/*
 * wav_parse.c — RIFF/WAV 头解析（小端）
 */
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "wav_parse.h"

#define TAG "wav"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t wav_parse(const uint8_t *wav, size_t len, wav_info_t *info)
{
    if (info == NULL || wav == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));

    if (len < 12) {
        ESP_LOGW(TAG, "WAV 太短 (%u B)", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "不是 RIFF/WAVE 文件");
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t off = 12;
    bool got_fmt = false, got_data = false;

    /* 逐个 chunk 扫描；chunk 按 2 字节对齐 */
    while (off + 8 <= len) {
        const uint8_t *ck = wav + off;
        uint32_t csize = rd32(ck + 4);
        size_t body = off + 8;

        if (memcmp(ck, "fmt ", 4) == 0) {
            if (csize < 16 || body + csize > len) {
                ESP_LOGW(TAG, "fmt chunk 越界");
                return ESP_ERR_INVALID_SIZE;
            }
            info->audio_format    = rd16(ck + 8);    /* +0 */
            info->channels        = rd16(ck + 10);   /* +2 */
            info->sample_rate     = rd32(ck + 12);   /* +4 */
            /* byte_rate @+8, block_align @+12 不需要 */
            info->bits_per_sample = rd16(ck + 22);   /* +14 */
            got_fmt = true;
        } else if (memcmp(ck, "data", 4) == 0) {
            info->data_offset = (uint32_t)body;
            uint64_t dl = csize;
            if (body + dl > len) {
                dl = len - body;                 /* 缓冲被截断时按实际长度 */
            }
            info->data_len = (uint32_t)dl;
            got_data = true;
            break;                               /* data 是最后一个关心的块 */
        }

        /* 跳到下一 chunk（带 2 字节对齐） */
        uint32_t step = 8 + csize + (csize & 1u);
        off += step;
    }

    if (!got_fmt || !got_data) {
        ESP_LOGW(TAG, "WAV 缺少 fmt/data chunk");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}
