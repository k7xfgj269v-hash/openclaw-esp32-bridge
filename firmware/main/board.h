/*
 * board.h — 板级常量与引脚映射
 *
 * 说明：所有引脚都来自 menuconfig（sdkconfig.h 的 CONFIG_CC_*），
 * 这里只做类型转换与少量固定分配（I2S 外设号）。
 *
 * I2S 外设分配：
 *   - I2S_NUM_0 → 麦克风 INMP441（仅 RX）
 *   - I2S_NUM_1 → 功放 MAX98357A（仅 TX）
 * 分开两个控制器，避免双工共时钟的相互牵制；也天然错开数据方向。
 * 两块默认引脚彼此不冲突，改 menuconfig 时自行保证不重叠。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"
#include "hal/gpio_types.h"
#include "hal/i2s_types.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 固定采样参数：与服务端 /voice 协议一致 —— 16kHz / 16bit / 单声道 */
#define CC_AUDIO_SAMPLE_RATE      (16000)
#define CC_AUDIO_BYTES_PER_SEC    (CC_AUDIO_SAMPLE_RATE * 2)  /* 每秒钟 PCM 字节数 */

/* I2S 外设号（S3 有两路 I2S） */
#define CC_MIC_I2S_PORT           I2S_NUM_0
#define CC_SPK_I2S_PORT           I2S_NUM_1

/* 引脚（menuconfig 配置 -> gpio_num_t） */
#define CC_MIC_BCLK_GPIO          ((gpio_num_t)CONFIG_CC_MIC_BCLK)
#define CC_MIC_WS_GPIO            ((gpio_num_t)CONFIG_CC_MIC_WS)
#define CC_MIC_DIN_GPIO           ((gpio_num_t)CONFIG_CC_MIC_DIN)

#define CC_SPK_BCLK_GPIO          ((gpio_num_t)CONFIG_CC_SPK_BCLK)
#define CC_SPK_WS_GPIO            ((gpio_num_t)CONFIG_CC_SPK_WS)
#define CC_SPK_DIN_GPIO           ((gpio_num_t)CONFIG_CC_SPK_DIN)

#define CC_PTT_GPIO               ((gpio_num_t)CONFIG_CC_PTT_GPIO)

/* PTT 按下判定：按键接地为低电平（内部上拉），低=按下录音 */
static inline bool board_ptt_pressed(void)
{
    return gpio_get_level(CC_PTT_GPIO) == 0;
}

#ifdef __cplusplus
}
#endif
