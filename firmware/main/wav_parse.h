/*
 * wav_parse.h — 极简 RIFF/WAV 头解析
 *
 * 只提取播放需要的字段和 data 偏移，支持任意 chunk 顺序/附加 chunk
 * （LIST 等），不做解压，仅支持 PCM(fmt=1) 的定位。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t audio_format;      /* 1 = PCM */
    uint16_t channels;          /* 声道数 */
    uint32_t sample_rate;       /* 采样率 Hz */
    uint16_t bits_per_sample;   /* 位深 */
    uint32_t data_offset;       /* data 数据在 wav 缓冲内的偏移 */
    uint32_t data_len;          /* data 数据长度（已按实际缓冲长度截断） */
} wav_info_t;

/* 解析 wav[len]，成功返回 ESP_OK 并填 *info */
esp_err_t wav_parse(const uint8_t *wav, size_t len, wav_info_t *info);

#ifdef __cplusplus
}
#endif
