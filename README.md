# OpenClaw ESP32 Bridge

HTTP bridge between ESP32-S3 hardware and OpenClaw AI agents — supports TTS, STT, dual-agent routing (@ki).

[中文版 README](README.zh-CN.md)

`OpenClaw ESP32 Bridge` is a Python HTTP server that lets an ESP32-S3 microcontroller talk to [OpenClaw](https://github.com/openclaw) AI agents over plain JSON. It ships several runnable server variants — from a minimal text-only wrapper up to a full voice pipeline (local Whisper speech-to-text + Edge TTS text-to-speech) — with per-device conversation sessions and dual-agent routing via the `@ki` prefix.

```text
ESP32-S3 device
      |  HTTP POST (JSON / PCM audio)
      v
Bridge server (Python, ThreadingHTTPServer)
      |  subprocess (openclaw agent --message ...)
      v
OpenClaw AI agent (esp32-voice / main)
      |  reply text
      v
TTS synthesis -> HTTP response -> ESP32
```

## Features

- **Four server variants** to match your needs: base (`pyttsx3` TTS), enhanced (dual-agent routing), minimal text-only (no TTS), and full voice pipeline (Whisper STT + TTS + PCM/`/voice` endpoint).
- **STT + TTS pipeline**: HTTP `POST /voice` accepts raw 16 kHz / 16-bit / mono PCM audio, transcribes it locally with `faster-whisper`, sends it to the agent, and returns synthesized speech as a 16 kHz WAV via `edge-tts`.
- **Dual-agent routing**: an `@ki ...` message prefix routes to the primary OpenClaw agent; all other messages go to a dedicated sub-agent (default `esp32-voice`).
- **Per-session isolation**: sessions are keyed by both `device_id` and the target agent, so a sub-agent conversation never leaks into the main agent and vice versa.
- **Zero-dependency `.env` loading**: `envcfg.py` loads `SERVER_*` / `OPENCLAW_*` at startup (stdlib only); already-set environment variables take precedence.
- **Concurrent-safe**: `ThreadingHTTPServer` keeps `/status` and other devices responsive while an agent call runs; agent subprocess calls are serialized by a lock.
- **Request limits & clean errors**: 1 MB text / 20 MB audio body caps (`413` on overflow); agent stderr stays in the server log and is never read back over TTS.

## Quick Start

```bash
# 1. Clone and install
git clone https://github.com/k7xfgj269v-hash/openclaw-esp32-bridge.git
cd openclaw-esp32-bridge
pip install -r requirements.txt

# 2. Optional — full voice pipeline (STT + TTS; pyttsx3 is already in requirements.txt)
pip install faster-whisper edge-tts

# 3. Configure environment
cp .env.example .env

# 4. Run the recommended server (dual-agent routing)
python voice_server_enhanced.py
```

> Requires Python 3.8+, the OpenClaw CLI (or a compatible agent), and an ESP32-S3 microcontroller.
> The `/voice` endpoint (sub-agent server) also needs `ffmpeg` installed on the system (`brew install ffmpeg` / `apt install ffmpeg`).

## Usage

### Environment variables

The `.env` file is loaded automatically at startup by the stdlib loader in `envcfg.py` (no extra dependency). Already-set environment variables take precedence. See [.env.example](.env.example).

| Variable | Default | Description |
|----------|---------|-------------|
| `SERVER_HOST` | `0.0.0.0` | Bind address (set to your LAN IP to restrict access) |
| `SERVER_PORT` | `8080` | Server port |
| `OPENCLAW_BIN` | `/home/ubuntu/.npm-global/bin/openclaw` | Path to the OpenClaw agent binary |
| `OPENCLAW_AGENT_ID` | `esp32-voice` | Agent ID used for sub-agent voice sessions |
| `OPENCLAW_PATH_ENV` | *(inherited)* | Overrides `PATH` in the subprocess; the rest of the environment (`HOME`, etc.) is preserved |
| `OPENCLAW_TIMEOUT` | `60` | Timeout (seconds) per agent call |

### Server variants

| Script | Description |
|--------|-------------|
| `voice_server.py` | Base server · `pyttsx3` local TTS · action-based API (`send_message`, `speak`, `command`, `verify_agent`) |
| `voice_server_enhanced.py` | Enhanced server · dual-agent routing (`@ki` → main agent) — recommended for most use cases |
| `openclaw_agent_server.py` | Minimal HTTP wrapper around a sub-agent · no TTS |
| `openclaw_subagent_server.py` | Full pipeline · Whisper STT + Edge TTS · `POST /voice` (PCM in → WAV out) |

**Recommended for most use cases:**

```bash
python voice_server_enhanced.py
```

### HTTP API

All servers receive requests as `POST` with a JSON body and return JSON (`Content-Type: application/json; charset=utf-8`). GET `GET /status` reports the service as online.

**Text request (enhanced / agent servers):**

```http
POST /
Content-Type: application/json

{ "device_id": "esp32-01", "message": "show temperature" }
```

Response (enhanced server):

```json
{ "success": true, "reply": "...", "agent": "esp32-voice", "timestamp": "..." }
```

**Dual-agent routing:** prefix the message with `@ki ` to send it to the main agent instead of the sub-agent, e.g. `{ "message": "@ki escalate this to the main agent" }`.

**Session management:** every `device_id` gets its own persistent conversation session — context is kept between requests. Session IDs are derived from both the `device_id` **and** the target agent, so `@ki` messages share no session with the sub-agent.

**Base server (`voice_server.py`) actions** — send `POST /` with a JSON body containing an `action` field:

| Action | Payload | Purpose |
|--------|---------|---------|
| `send_message` | `{ "device_id", "message" }` | Send a message to the agent (keeps session continuity) |
| `speak` | `{ "text" }` | Synthesize and play text via `pyttsx3` locally |
| `command` | `{ "command" }` | Speak the command, send it to the agent, and speak the reply |
| `verify_agent` | — | Verify the `OPENCLAW_AGENT_ID` sub-agent exists |

**Full voice pipeline (sub-agent server) — PCM audio in, WAV out:**

```http
POST /voice
Content-Type: application/octet-stream   # raw 16 kHz / 16-bit / mono PCM

<audio bytes>
```

Response headers: `Content-Type: audio/wav` and `X-AI-Reply` (the plain-text agent reply). The pipeline is: PCM → WAV → `faster-whisper` (local STT, default Chinese) → OpenClaw agent → `edge-tts` → `ffmpeg` → 16 kHz mono WAV.

### Technical details

- **Concurrency:** `ThreadingHTTPServer` — `/status` and other devices do not block behind a running agent call; agent calls themselves are serialized by a lock.
- **Session IDs:** derived from the `device_id` **and** the target agent — `@ki` messages to the main agent never share a session with the sub-agent.
- **Dual routing:** the `@ki` prefix routes a message to the primary agent instead of the sub-agent (`voice_server_enhanced.py` only).
- **System prompt:** injected only on a session's first message, not every turn (sub-agent server).
- **Input limits:** 1 MB for JSON requests, 20 MB for PCM audio (`413` when exceeded).
- **Error output:** agent stderr stays in the server log and is never returned to the device (it would otherwise be read aloud by TTS).
- **TTS options:** `pyttsx3` (local, offline) or Edge TTS (Microsoft Azure, higher quality).

## Testing

The test suite replaces the `openclaw` binary with a stub and checks routing, session isolation, prompt injection, and request limits — without real agents or audio dependencies:

```bash
python3 -m venv .venv && ./.venv/bin/pip install pytest
./.venv/bin/python -m pytest tests/ -v
```

## Project Structure

```text
openclaw-esp32-bridge/
├── envcfg.py                    # stdlib .env loader (no third-party deps)
├── voice_server.py              # base server: pyttsx3 TTS + action API
├── voice_server_enhanced.py     # dual-agent routing (@ki -> main agent)
├── openclaw_agent_server.py     # minimal HTTP wrapper (no TTS)
├── openclaw_subagent_server.py  # full pipeline: Whisper STT + Edge TTS + /voice
├── conftest.py                  # pytest stub for the openclaw binary
├── tests/
│   └── test_servers.py          # routing / session / prompt / limit tests
├── requirements.txt
├── .env.example
└── .gitignore
```

## License

MIT License — see [LICENSE](LICENSE).
