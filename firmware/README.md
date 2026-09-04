# OpenClaw ESP32-S3 语音陪伴设备固件 (`firmware/`)

ESP32-S3 的语音对讲（PTT）固件：按住按键说话 → 上传裸 PCM 到仓库
`openclaw_subagent_server.py` 的 `POST /voice` → 接收返回的 `audio/wav` 并播放。

- 麦克风：INMP441（I2S MEMS）
- 功放：MAX98357A（I2S 标准）+ 4Ω 2W 喇叭
- 协议：`POST {server}/voice`，`Content-Type: application/octet-stream`，
  body = 16 kHz / 16 bit / 单声道裸 PCM；响应 `audio/wav` + 头 `X-AI-Reply`
- 芯片 / 工具链：ESP32-S3，ESP-IDF ≥ 5.0（用新 I2S API `driver/i2s_std.h`，
  不是旧的 `driver/i2s.h`）。已在本地 ESP-IDF v5.4 上核对 API 签名。

> 本固件当前在 `/tmp/openclaw-esp32-bridge-fw/firmware`（独立克隆），尚未编译烧录，
> 也未 push/commit。

---

## 1. 烧录命令模板

```bash
# 0) 进入 ESP-IDF 环境（zsh）
#    若已安装到 ~/Documents/esp/esp-idf：
#    . ~/Documents/esp/esp-idf/export.sh
#    或按你机器上的惯例 get_idf

cd firmware

# 1) 设目标芯片（首次会生成 sdkconfig）
idf.py set-target esp32s3

# 2) 配置：WiFi / 服务器地址端口 / Token / 是否校验证书 / 引脚等
idf.py menuconfig
#    → OpenClaw 语音陪伴设备

# 3) 编译
idf.py build

# 4) 烧录 + 串口监视（把 XXXXX 换成你的串口号，如 cu.usbserial-1420）
idf.py -p /dev/cu.usbserial-XXXX flash monitor

# 退出监视 Ctrl+]
```

常用排障：

- 查看可用串口：`ls /dev/cu.usbserial-*`
- 只编译不烧：`idf.py build`
- 烧录时不要按住 BOOT(GPIO0)——那是我们的 PTT 键，复位瞬间按住会进下载模式
  （正常上电后按键使用无碍，因为 flash 走板载 USB-JTAG）。

---

## 2. 目录结构

```text
firmware/
├── CMakeLists.txt          # 工程入口（project openclaw_companion）
├── sdkconfig.defaults      # 锁 esp32s3；开证书包；加大主任务栈
├── main/
│   ├── CMakeLists.txt      # 组件注册（REQUIRES 兼容 IDF v5.0 / v5.2+）
│   ├── Kconfig.projbuild   # menuconfig 全部配置项
│   ├── board.h             # 引脚映射 + I2S 外设分配（I2S0=mic / I2S1=spk）
│   ├── app_main.c          # 状态机：待机→录音→上传→播放
│   ├── wifi_mgr.c/.h       # WiFi STA + 断线退避重连
│   ├── i2s_audio.c/.h      # I2S 录音/播放（新 API）
│   ├── wav_parse.c/.h      # RIFF/WAV 头解析
│   └── voice_client.c/.h   # HTTPS POST /voice（两段式，省内存）
└── README.md
```

---

## 3. 默认引脚表（全部可在 menuconfig 改）

| 功能 | 信号 | 默认 GPIO | 说明 |
|---|---|---|---|
| PTT 按键 | 按下接地 | **GPIO0** | 开发板 BOOT 键；内部上拉，低电平=录音 |
| 麦克风 INMP441 | BCLK(SCK) | GPIO4 | |
| 麦克风 INMP441 | WS(LRCK) | GPIO5 | |
| 麦克风 INMP441 | SD(数据出→ESP32 DIN) | GPIO6 | 接 ESP32 I2S RX |
| 功放 MAX98357A | BCLK | GPIO15 | |
| 功放 MAX98357A | LRC(WS) | GPIO16 | |
| 功放 MAX98357A | DIN(数据入) | GPIO17 | ESP32 I2S TX DOUT |
| 功放 MAX98357A | SD / GAIN | — | 需按模块接好（GAIN 决定增益） |

I2S 外设分配：麦克风走 **I2S_NUM_0**，功放走 **I2S_NUM_1**（分开控制器，互不牵制）。

接线要点：

- INMP441 的 `L/R` 接地时数据在左声道（WS 低电平期间输出），menuconfig 默认取左声道；
  若录到静音/噪声，把 `CC_MIC_LR` 切到右声道即可，不必改线。
- INMP441 供电 3.3V；功放供电按喇叭/模块规格（常为 5V）。
- 默认 GPIO0 是 strapping 脚：复位瞬间别按住；如果不想用 BOOT 键，外接按键改
  `CC_PTT_GPIO` 到任意空闲 GPIO。

---

## 4. menuconfig 配置项（`OpenClaw 语音陪伴设备`）

| 配置项 | 默认 | 说明 |
|---|---|---|
| `CC_WIFI_SSID` | `myssid` | 路由器 SSID（占位，必改） |
| `CC_WIFI_PASSWORD` | `mypassword` | WiFi 密码（占位，必改） |
| `CC_SERVER_SCHEME` | `https` | `http` 或 `https` |
| `CC_SERVER_HOST` | `<DE_IP>` | 服务器主机/IP（占位，**必改**，不含协议端口） |
| `CC_SERVER_PORT` | `8443` | 端口；直连 Python 服务(无 TLS)改 `8080` |
| `CC_SERVER_PATH` | `/voice` | 语音接口路径 |
| `CC_AUTH_TOKEN` | *(空)* | 非空则带 `Authorization: Bearer <token>` |
| `CC_TLS_SKIP_VERIFY` | `y` | `y`=跳过证书校验(自签调试) / `n`=校验证书 |
| `CC_HTTP_TIMEOUT_MS` | `120000` | HTTP 单次网络超时（服务端要等 AI，别设太小） |
| `CC_REC_MAX_SEC` | `10` | 最长录音；无 PSRAM 时内存不够会自动缩到 5s/2s |
| `CC_RESP_MAX_KB` | `512` | 服务器返回 WAV 上限，超出报错 |
| `CC_PTT_GPIO` | `0` | PTT 按键引脚 |
| `CC_MIC_BCLK / WS / DIN` | `4 / 5 / 6` | 麦克风引脚 |
| `CC_SPK_BCLK / WS / DIN` | `15 / 16 / 17` | 功放引脚 |
| `CC_MIC_LR` | 左声道 | INMP441 L/R 接法对应的槽位 |

### 协议与服务器形态

仓库里的 `openclaw_subagent_server.py` 是**纯 HTTP**（默认 `0.0.0.0:8080`，无鉴权）。
两种跑法：

1. **直连**：`CC_SERVER_SCHEME=http`、`CC_SERVER_PORT=8080`，不开鉴权。
2. **HTTPS 反代**：若你在服务器用 nginx/caddy 给 `/voice` 套了 TLS，或服务端自带 TLS，
   用 `https` + 对应端口。DE 那台如果是 IP + 自签证书，先保持
   `CC_TLS_SKIP_VERIFY=y` 跑通链路。

证书校验（`CC_TLS_SKIP_VERIFY=n`）默认走 mbedTLS 系统根证书（sdkconfig.defaults
已开 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`），只对公共 CA 签发的证书有效；私有 CA
需要把 CA 填进固件（改 `voice_client.c` 用 `cfg.cert_pem`）。

---

## 5. 内存与录音时长

- 采样协议固定 16 kHz / 16 bit / 单声道 → **32 KB/s**。
- 10s ≈ 320 KB。有 PSRAM 的 S3（如 WROOM-1 N8R8 等）无压力；
  无 PSRAM 的 DevKitC-1 内部 RAM 紧张，`alloc_pcm_buf` 会自动降级：
  10s → 5s → 2s，仍失败就报错。
- 上传与响应采用**两段式**（见 `voice_client.h`）：上传完 PCM 立刻 `free`，
  再去分配响应 WAV，二者不会同时占大块内存。

---

## 6. 线程模型 / 状态机（一句话）

单主任务顺序跑「IDLE → 按住录音(I2S RX→PCM) → 松开→等WiFi→HTTPS上传→收WAV→I2S TX播放 → IDLE」；
WiFi 由独立任务负责断线退避重连，HTTP 在 app_main 内阻塞调用。

---

## 7. 未验证假设与边界情况清单

以下是我写代码时明确知道的假设，**尚未上真机验证**，烧录后如异常优先核对：

1. **未编译**：本机有 ESP-IDF v5.4 源码用于核对 API 签名，但没跑 `idf.py build`
   编译验证；个别头/宏以你本地的 IDF 版本为准。
2. **INMP441 槽位/对齐**：按“32bit 槽高位 24bit、取高 16bit = 有效音频”处理，
   且默认左声道。若录到的是噪声/静音，先在 menuconfig 切 `CC_MIC_LR`，再查接线与供电。
3. **`>> 16` 的位深假设**：INMP441 在 32bit 槽内高位对齐是业界惯例但未实测；
   若电平正常但声音“破/小”，可能需改 `>>14` 或数据位宽（在 `i2s_audio.c`）。
   另：Philips 模式带 1-bit shift，INMP441 若实测波形异常（首拍偏移/偏小），
   可把麦克风那路改用 `I2S_STD_MSB_SLOT_DEFAULT_CONFIG`（左对齐、无 shift）对比。
4. **MAX98357A 模式**：按 I2S 标准（Philips）16bit 立体声驱动；若你的模块 SD_MODE
   被接成左对齐/TDM，需要改 slot 配置。
5. **录音截断**：松开 PTT 后最多多采约一个 DMA 缓冲（≈15ms），未做精细排空。
6. **WiFi**：按 WPA2 阈值连网，纯 WPA3(SAE) 或开放网络未适配（代码注释有指引）；
   断线重连永不放弃、退避封顶 10s。
7. **TLS**：`skip_cert_common_name_check=true` 且不挂 CA 时按 esp-tls 行为会整体跳过
   校验，但不同 IDF 小版本细节可能有差异；如校验模式下握手失败，检查证书链/时间。
8. **回声/啸叫**：无 AEC，喇叭与麦克风拉开距离或调低功放增益（MAX98357A GAIN 脚）。
9. **HTTP 超时**：`CC_HTTP_TIMEOUT_MS` 是单次网络读写超时，需大于服务端 AI 处理耗时；
   若服务端偶发 >120s 请调大。
10. **Kconfig 数字**：GPIO 未做 strapping/占用校验，改引脚时避免与 flash/PSRAM/USB/晶振
    冲突（参考 ESP32-S3 数据手册）。
11. **响应格式**：假定服务器返回标准 PCM WAV(16k/1ch/16bit)。解析器支持任意 chunk
    顺序、LIST 等附加块；8/16bit 可播，24/32bit 或非 PCM 会拒绝播放并打日志。
12. **内存峰值**：录音 10s+512KB 响应同时出现在**无 PSRAM** 板子上仍可能紧张——
    建议响应 512KB 上限按需调小，或换带 PSRAM 的板子。

---

## 8. 只做 MVP 的刻意取舍

- 状态灯只预留了概念（可把 PTT/播放状态映射到 GPIO），未做 LED 控制。
- 上传是整包 PCM（Content-Length 固定），服务端 `BaseHTTPRequestHandler` 不支持
  chunked 上传，故没有做流式分块；内存方案见上文。
- 未引入第三方组件，全部基于 ESP-IDF 自带。
