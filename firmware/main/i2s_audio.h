/*
 * i2s_audio.h — I2S 录音 / 播放（新 API：driver/i2s_std.h）
 *
 * 硬件：
 *   - 麦克风 INMP441：I2S_NUM_0 仅 RX。芯片输出 24bit、MSB 优先、LJ；
 *     这里按“伪立体声”取左声道 32bit 槽的高 16bit，得到 16bit/16kHz/单声道。
 *   - 功放 MAX98357A：I2S_NUM_1 仅 TX，I2S 标准 16bit 立体声；播放时单声道
 *     复制到 L/R，立体声直通。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化两路 I2S（只初始化，默认不使能，随用随开以省电/防底噪） */
esp_err_t audio_init(void);

/* 录音控制 */
esp_err_t audio_mic_start(void);                          /* 使能 RX（开始采） */
esp_err_t audio_mic_stop(void);                           /* 停 RX */
/* 阻塞读最多 max_samples 个 16bit/单声道样本到 dst，返回实际读到个数。
 * 内部自动完成 32bit→16bit 降位与去右声道。 */
esp_err_t audio_mic_read(int16_t *dst, size_t max_samples, size_t *got_samples);

/* 播放一段 WAV（内部解析头，按实际采样率/声道/位深播放；8/16bit 支持，
 * 24/32bit 或非 PCM 返回 ESP_ERR_NOT_SUPPORTED） */
esp_err_t audio_play_wav(const uint8_t *wav, size_t len);

#ifdef __cplusplus
}
#endif
