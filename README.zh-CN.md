# OpenClaw ESP32 Bridge

连接 ESP32-S3 硬件与 OpenClaw AI 代理的 HTTP 桥接服务 — 支持 TTS、STT 与双代理路由（@ki）。

[English README](README.md)

`OpenClaw ESP32 Bridge` 是一个 Python HTTP 服务器，让 ESP32-S3 微控制器能通过纯 JSON 与 [OpenClaw](https://github.com/openclaw) AI 代理对话。项目内置多种可运行的服务器变体——从极简纯文本封装到完整的语音处理管线（本地 Whisper 语音识别 + Edge TTS 语音合成）——并支持按设备隔离会话，以及通过 `@ki` 前缀实现双代理路由。

```text
ESP32-S3 设备
      |  HTTP POST（JSON / PCM 音频）
      v
桥接服务器（Python, ThreadingHTTPServer）
      |  子进程（openclaw agent --message ...）
      v
OpenClaw AI 代理（esp32-voice / 主代理）
      |  回复文本
      v
TTS 合成 -> HTTP 响应 -> ESP32
```

## 功能特性

- **四种服务器变体**按需选用：基础版（`pyttsx3` TTS）、增强版（双代理路由）、极简文本版（无 TTS）、完整语音管线版（Whisper STT + TTS + PCM 音频/`/voice` 接口）。
- **STT + TTS 管线**：`POST /voice` 接收原始 16 kHz / 16-bit / 单声道 PCM 音频，经本地 `faster-whisper` 转写文字，交给代理处理，最后用 `edge-tts` 合成并以 16 kHz WAV 返回。
- **双代理路由**：消息加 `@ki ...` 前缀时转发给主 OpenClaw 代理；其余消息默认发给专用子代理（默认 `esp32-voice`）。
- **会话隔离**：会话按「设备 ID + 目标代理」双键区分，子代理会话不会混入主代理，反之亦然。
- **零依赖 `.env` 加载**：`envcfg.py` 在启动时读取 `SERVER_*` / `OPENCLAW_*` 配置（仅用标准库）；已存在的环境变量优先。
- **并发安全**：`ThreadingHTTPServer` 让 `/status` 与其他设备不会因一次代理调用而阻塞；代理子进程调用由锁串行化。
- **请求上限与干净的错误输出**：文本 1 MB / 音频 20 MB（超限返回 `413`）；代理 stderr 只留在服务器日志中，绝不以 TTS 方式读给设备。

## 快速开始

```bash
# 1. 克隆并安装依赖
git clone https://github.com/k7xfgj269v-hash/openclaw-esp32-bridge.git
cd openclaw-esp32-bridge
pip install -r requirements.txt

# 2. 可选 — 完整语音管线（STT + TTS；pyttsx3 已在 requirements.txt 中）
pip install faster-whisper edge-tts

# 3. 配置环境变量
cp .env.example .env

# 4. 运行推荐的服务器（双代理路由）
python voice_server_enhanced.py
```

> 要求 Python 3.8+、OpenClaw CLI（或兼容代理）以及 ESP32-S3 微控制器。
> 子代理服务器的 `/voice` 接口还需要系统安装 `ffmpeg`（`brew install ffmpeg` / `apt install ffmpeg`）。

## 使用说明

### 环境变量

`.env` 文件在启动时由 `envcfg.py` 的标准库加载器自动读取（无额外依赖）。已存在的环境变量优先。详见 [.env.example](.env.example)。

| 变量 | 默认值 | 说明 |
|----------|---------|-------------|
| `SERVER_HOST` | `0.0.0.0` | 绑定地址（仅限局域网可设为局域网 IP） |
| `SERVER_PORT` | `8080` | 服务器端口 |
| `OPENCLAW_BIN` | `/home/ubuntu/.npm-global/bin/openclaw` | OpenClaw 代理可执行文件路径 |
| `OPENCLAW_AGENT_ID` | `esp32-voice` | 语音会话使用的子代理 ID |
| `OPENCLAW_PATH_ENV` | *（继承）* | 覆盖子进程的 `PATH`；其余环境（`HOME` 等）保持不变 |
| `OPENCLAW_TIMEOUT` | `60` | 每次代理调用的超时（秒） |

### 服务器变体

| 脚本 | 说明 |
|--------|-------------|
| `voice_server.py` | 基础服务器 · `pyttsx3` 本地 TTS · 基于 action 的接口（`send_message`、`speak`、`command`、`verify_agent`） |
| `voice_server_enhanced.py` | 增强服务器 · 双代理路由（`@ki` → 主代理）— 大多数场景推荐 |
| `openclaw_agent_server.py` | 极简 HTTP 封装，对接子代理 · 无 TTS |
| `openclaw_subagent_server.py` | 完整管线 · Whisper STT + Edge TTS · `POST /voice`（PCM 进 → WAV 出） |

**大多数场景推荐：**

```bash
python voice_server_enhanced.py
```

### HTTP API

所有服务器都通过 `POST` 接收 JSON 请求体并返回 JSON（`Content-Type: application/json; charset=utf-8`）。`GET /status` 返回服务在线状态。

**文本请求（增强 / 代理服务器）：**

```http
POST /
Content-Type: application/json

{ "device_id": "esp32-01", "message": "显示温度" }
```

响应（增强服务器）：

```json
{ "success": true, "reply": "...", "agent": "esp32-voice", "timestamp": "..." }
```

**双代理路由：** 在消息前加 `@ki ` 前缀即可发给主代理而非子代理，例如 `{ "message": "@ki 将该问题升级给主代理" }`。

**会话管理：** 每个 `device_id` 拥有独立的持久会话——上下文会在请求之间保持。会话 ID 由「设备 ID **和**目标代理」共同推导，因此 `@ki` 消息与子代理不共享会话。

**基础服务器（`voice_server.py`）action 接口** — 向 `POST /` 发送含 `action` 字段的 JSON 请求体：

| Action | 载荷 | 用途 |
|--------|---------|---------|
| `send_message` | `{ "device_id", "message" }` | 发送消息给代理（保持会话连续） |
| `speak` | `{ "text" }` | 本地用 `pyttsx3` 合成并播放文本 |
| `command` | `{ "command" }` | 播报命令、发送给代理处理并播报回复 |
| `verify_agent` | — | 校验 `OPENCLAW_AGENT_ID` 子代理是否存在 |

**完整语音管线（子代理服务器）— PCM 音频进，WAV 出：**

```http
POST /voice
Content-Type: application/octet-stream   # 原始 16 kHz / 16-bit / 单声道 PCM

<音频字节>
```

响应头：`Content-Type: audio/wav` 与 `X-AI-Reply`（纯文本代理回复）。管线为：PCM → WAV → `faster-whisper`（本地 STT，默认中文）→ OpenClaw 代理 → `edge-tts` → `ffmpeg` → 16 kHz 单声道 WAV。

### 技术细节

- **并发**：`ThreadingHTTPServer` — `/status` 与其他设备不会因运行中的代理调用而阻塞；代理调用本身由锁串行化。
- **会话 ID**：由「设备 ID **和目标代理**」推导 — 发给主代理的 `@ki` 消息绝不会与子代理共享会话。
- **双路由**：`@ki` 前缀把消息转发给主代理而非子代理（仅 `voice_server_enhanced.py`）。
- **系统提示词**：只在会话首条消息注入，不每轮重复（子代理服务器）。
- **输入上限**：JSON 请求 1 MB，PCM 音频 20 MB（超限返回 `413`）。
- **错误输出**：代理 stderr 只留在服务器日志中，绝不返回给设备（否则会被 TTS 读出来）。
- **TTS 选项**：`pyttsx3`（本地离线）或 Edge TTS（微软 Azure，质量更高）。

## 测试

测试套件把 `openclaw` 二进制替换为桩（stub），校验路由、会话隔离、提示词注入与请求上限——不依赖真实代理或音频：

```bash
python3 -m venv .venv && ./.venv/bin/pip install pytest
./.venv/bin/python -m pytest tests/ -v
```

## 项目结构

```text
openclaw-esp32-bridge/
├── envcfg.py                    # 标准库 .env 加载器（无第三方依赖）
├── voice_server.py              # 基础服务器：pyttsx3 TTS + action 接口
├── voice_server_enhanced.py     # 双代理路由（@ki -> 主代理）
├── openclaw_agent_server.py     # 极简 HTTP 封装（无 TTS）
├── openclaw_subagent_server.py  # 完整管线：Whisper STT + Edge TTS + /voice
├── conftest.py                  # pytest 的 openclaw 桩二进制
├── tests/
│   └── test_servers.py          # 路由 / 会话 / 提示词 / 上限测试
├── requirements.txt
├── .env.example
└── .gitignore
```

## 许可证

MIT License — 见 [LICENSE](LICENSE)。
