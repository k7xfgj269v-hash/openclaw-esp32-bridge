/*
 * i2s_audio.c — I2S 录音/播放实现（新 I2S API，driver/i2s_std.h，IDF v5+）
 *
 * 录音通路说明：
 *   INMP441 是 24bit 单声道 MEMS，接口按 I2S 标准对齐输出。
 *   我们把它当作“双声道 32bit 槽”的伪立体声来采，只取左(或右)一个槽，
 *   每个槽 32bit 里数据占高位 24bit，右移 16 位即得到 16bit 有效音频。
 *   这样不依赖驱动对 24bit 的移位细节，跨 IDF 版本行为稳定。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/i2s_std.h"

#include "sdkconfig.h"
#include "board.h"
#include "wav_parse.h"
#include "i2s_audio.h"

#define TAG "audio"

/* 录音原始数据暂存（32bit/槽）与播放暂存（16bit） */
static int32_t s_mic_scratch[512];      /* 2KB */
static int16_t s_spk_scratch[2048];     /* 4KB */

static i2s_chan_handle_t s_mic = NULL;
static i2s_chan_handle_t s_spk = NULL;
static bool    s_spk_enabled = false;
static uint32_t s_spk_rate = CC_AUDIO_SAMPLE_RATE;

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */
static esp_err_t audio_init_mic(void)
{
    /* 1) 分配 RX 通道（I2S_NUM_0，simplex：只传 rx_handle） */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CC_MIC_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_mic);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mic i2s_new_channel: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2) slot：伪立体声取左声道；32bit 槽内高位 24bit 是 INMP441 数据。
     * 注：Philips 模式带 1 bit shift(bit_shift=true)。INMP441 数据手册标称 I2S 兼容，
     * 若实测首样本/波形异常，可改 I2S_STD_MSB_SLOT_DEFAULT_CONFIG(左对齐,无 shift)
     * 再对比。见 README「未验证假设」。 */
    i2s_std_slot_config_t mic_slot =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
#if defined(CONFIG_CC_MIC_LR_RIGHT)
    mic_slot.slot_mask = I2S_STD_SLOT_RIGHT;
#else
    mic_slot.slot_mask = I2S_STD_SLOT_LEFT;
#endif

    /* 3) 时钟 */
    i2s_std_clk_config_t mic_clk = {
        .sample_rate_hz = CC_AUDIO_SAMPLE_RATE,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    };

    /* 4) GPIO：din 接 INMP441 SD */
    i2s_std_gpio_config_t mic_gpio = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = CC_MIC_BCLK_GPIO,
        .ws   = CC_MIC_WS_GPIO,
        .dout = I2S_GPIO_UNUSED,
        .din  = CC_MIC_DIN_GPIO,
        .invert_flags = { 0 },
    };

    i2s_std_config_t mic_std = {
        .clk_cfg = mic_clk,
        .slot_cfg = mic_slot,
        .gpio_cfg = mic_gpio,
    };
    ret = i2s_channel_init_std_mode(s_mic, &mic_std);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mic i2s_channel_init_std_mode: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t audio_init_spk(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CC_SPK_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_spk, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spk i2s_new_channel: %s", esp_err_to_name(ret));
        return ret;
    }

    /* MAX98357A：I2S 标准，16bit 立体声（播放时把单声道复制到 L/R） */
    i2s_std_slot_config_t spk_slot =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

    i2s_std_clk_config_t spk_clk = {
        .sample_rate_hz = CC_AUDIO_SAMPLE_RATE,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    };

    i2s_std_gpio_config_t spk_gpio = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = CC_SPK_BCLK_GPIO,
        .ws   = CC_SPK_WS_GPIO,
        .dout = CC_SPK_DIN_GPIO,   /* MAX98357A DIN */
        .din  = I2S_GPIO_UNUSED,
        .invert_flags = { 0 },
    };

    i2s_std_config_t spk_std = {
        .clk_cfg = spk_clk,
        .slot_cfg = spk_slot,
        .gpio_cfg = spk_gpio,
    };
    ret = i2s_channel_init_std_mode(s_spk, &spk_std);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spk i2s_channel_init_std_mode: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t audio_init(void)
{
    esp_err_t ret = audio_init_mic();
    if (ret != ESP_OK) {
        return ret;
    }
    return audio_init_spk();
}

/* ------------------------------------------------------------------ */
/* 录音                                                               */
/* ------------------------------------------------------------------ */
esp_err_t audio_mic_start(void)
{
    esp_err_t ret = i2s_channel_enable(s_mic);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mic enable: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t audio_mic_stop(void)
{
    esp_err_t ret = i2s_channel_disable(s_mic);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mic disable: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t audio_mic_read(int16_t *dst, size_t max_samples, size_t *got_samples)
{
    size_t out = 0;
    const size_t scratch_words = sizeof(s_mic_scratch) / sizeof(s_mic_scratch[0]);

    while (out < max_samples) {
        size_t block = max_samples - out;
        if (block > scratch_words) {
            block = scratch_words;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_mic, s_mic_scratch,
                                         block * sizeof(uint32_t), &bytes_read,
                                         portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mic read err: %s", esp_err_to_name(err));
            break;
        }
        size_t words = bytes_read / sizeof(uint32_t);
        if (words == 0) {
            break;
        }

        /* 每个 32bit 槽取高 16bit（INMP441 数据高位对齐） */
        for (size_t i = 0; i < words; i++) {
            dst[out++] = (int16_t)(s_mic_scratch[i] >> 16);
        }

        if (words < block) {
            break;   /* 数据流中断，避免死等 */
        }
    }

    if (got_samples) {
        *got_samples = out;
    }
    return (out > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* 播放                                                               */
/* ------------------------------------------------------------------ */
static esp_err_t spk_apply_rate(uint32_t rate_hz)
{
    if (rate_hz == s_spk_rate) {
        return ESP_OK;
    }
    /* 重配时钟要求通道处于 READY（已停止）状态 */
    if (s_spk_enabled) {
        i2s_channel_disable(s_spk);
        s_spk_enabled = false;
    }

    i2s_std_clk_config_t clk = {
        .sample_rate_hz = rate_hz,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    };
    esp_err_t ret = i2s_channel_reconfig_std_clock(s_spk, &clk);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spk reconfig clk: %s", esp_err_to_name(ret));
        return ret;
    }
    s_spk_rate = rate_hz;
    return ESP_OK;
}

esp_err_t audio_play_wav(const uint8_t *wav, size_t len)
{
    if (wav == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wav_info_t wi;
    esp_err_t ret = wav_parse(wav, len, &wi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WAV 解析失败: %s", esp_err_to_name(ret));
        return ret;
    }
    if (wi.audio_format != 1) {
        ESP_LOGE(TAG, "非 PCM WAV (format=%u)，不播放", wi.audio_format);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (wi.bits_per_sample != 16 && wi.bits_per_sample != 8) {
        ESP_LOGE(TAG, "位深 %u 不支持（仅 8/16bit）", wi.bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* data 区域，按缓冲长度再夹一次 */
    const uint8_t *p = wav + wi.data_offset;
    size_t n = wi.data_len;
    if ((size_t)wi.data_offset + wi.data_len > len) {
        n = len - wi.data_offset;
    }

    ESP_LOGI(TAG, "播放 %uch %luHz %ubit，PCM %u 字节",
             wi.channels, (unsigned long)wi.sample_rate,
             wi.bits_per_sample, (unsigned)n);

    ret = spk_apply_rate(wi.sample_rate);
    if (ret != ESP_OK) {
        return ret;
    }
    i2s_channel_enable(s_spk);
    s_spk_enabled = true;

    size_t off = 0;
    while (off < n) {
        size_t out16 = 0;   /* 已填入 s_spk_scratch 的 int16 个数 */

        if (wi.channels == 1) {
            /* 单声道 -> 复制到 L/R */
            if (wi.bits_per_sample == 16) {
                size_t frames = (n - off) / 2;
                if (frames > 512) frames = 512;
                for (size_t i = 0; i < frames; i++) {
                    int16_t s;
                    memcpy(&s, p + off + i * 2, 2);   /* 防未对齐 */
                    s_spk_scratch[out16++] = s;
                    s_spk_scratch[out16++] = s;
                }
                off += frames * 2;
            } else { /* 8bit 单声道：无符号 -> int16 */
                size_t frames = n - off;
                if (frames > 1024) frames = 1024;
                for (size_t i = 0; i < frames; i++) {
                    int16_t s = (int16_t)(((int)p[off + i] - 128) << 8);
                    s_spk_scratch[out16++] = s;
                    s_spk_scratch[out16++] = s;
                }
                off += frames;
            }
        } else {
            /* 双声道 */
            if (wi.bits_per_sample == 16) {
                size_t bytes = n - off;
                if (bytes > sizeof(s_spk_scratch)) {
                    bytes = sizeof(s_spk_scratch);
                }
                bytes &= ~3u;                    /* 对齐到完整帧(4B) */
                if (bytes == 0) break;
                memcpy(s_spk_scratch, p + off, bytes);
                out16 = bytes / 2;
                off += bytes;
            } else { /* 8bit 双声道 -> int16 立体声 */
                size_t bytes = n - off;
                if (bytes > sizeof(s_spk_scratch)) {
                    bytes = sizeof(s_spk_scratch);
                }
                bytes &= ~1u;
                if (bytes == 0) break;
                for (size_t i = 0; i < bytes / 2; i++) {
                    s_spk_scratch[out16++] = (int16_t)(((int)p[off + i * 2] - 128) << 8);
                    s_spk_scratch[out16++] = (int16_t)(((int)p[off + i * 2 + 1] - 128) << 8);
                }
                off += bytes;
            }
        }

        if (out16 == 0) {
            break;
        }

        /* 整块写入 I2S TX */
        size_t written = 0;
        const size_t total_bytes = out16 * sizeof(int16_t);
        while (written < total_bytes) {
            size_t w = 0;
            ret = i2s_channel_write(s_spk, (uint8_t *)s_spk_scratch + written,
                                    total_bytes - written, &w, portMAX_DELAY);
            if (ret != ESP_OK || w == 0) {
                ESP_LOGE(TAG, "spk write err: %s", esp_err_to_name(ret));
                goto done;
            }
            written += w;
        }
    }
    ret = ESP_OK;

done:
    /* 等 DMA 里最后的尾巴播完再关，避免截尾 */
    vTaskDelay(pdMS_TO_TICKS(60));
    i2s_channel_disable(s_spk);
    s_spk_enabled = false;
    return ret;
}
