# esp32-voice-server

HTTP server that bridges ESP32-S3 hardware with OpenClaw AI agents, providing voice interaction capabilities including speech synthesis (TTS) and optional speech recognition (STT).

## Prerequisites

- Python 3.8 or later
- [OpenClaw](https://github.com/openclaw) CLI installed and accessible on the server
- An OpenClaw agent named `esp32-voice` (or whichever ID you configure via `OPENCLAW_AGENT_ID`)

## Environment Setup

Copy the example file and fill in your values:

```bash
cp .env.example .env
```

Edit `.env` with the paths and settings appropriate for your environment. See the [Environment Variables](#environment-variables) section below for a description of each variable.

## Installation

```bash
pip install -r requirements.txt
```

For the subagent server (`openclaw_subagent_server.py`), additional packages are required:

```bash
pip install faster-whisper edge-tts
```

## Usage

Choose the script that matches your use case:

| Script | Use Case |
|---|---|
| `voice_server.py` | Basic voice server — receives ESP32 messages, routes them through a persistent OpenClaw agent (`esp32-voice`), and uses `pyttsx3` for local TTS playback. |
| `voice_server_enhanced.py` | Enhanced version — same as above but also supports routing `@ki` prefixed messages to the primary (non-sub) OpenClaw agent. Recommended for most deployments. |
| `openclaw_agent_server.py` | Minimal agent server — thin HTTP wrapper around OpenClaw, no TTS. Useful for debugging or headless deployments. |
| `openclaw_subagent_server.py` | Full voice pipeline — adds Whisper-based STT (speech-to-text) and edge-tts for high-quality TTS. Requires `faster-whisper` and `edge-tts`. |

Start the server (replace the script name as needed):

```bash
python voice_server_enhanced.py
```

The server listens on `0.0.0.0:SERVER_PORT` (default `8080`).

### ESP32 Request Format

Send an HTTP POST to `http://<server-ip>:<port>/` with JSON body:

```json
{
  "device_id": "esp32-01",
  "message": "your voice command text here"
}
```

In `voice_server_enhanced.py`, prefix the message with `@ki ` to route it to the primary OpenClaw agent instead of the sub-agent.

## Environment Variables

All configuration is read from environment variables. Copy `.env.example` to `.env` and set values there; the scripts load them via `os.environ.get()`.

See `.env.example` for the full list with inline descriptions. Summary:

| Variable | Default | Description |
|---|---|---|
| `SERVER_PORT` | `8080` | Port the HTTP server listens on |
| `OPENCLAW_BIN` | `/home/ubuntu/.npm-global/bin/openclaw` | Absolute path to the `openclaw` executable |
| `OPENCLAW_PATH_ENV` | `/home/ubuntu/.npm-global/bin:/usr/bin:/bin` | `PATH` value injected into subprocess environment |
| `OPENCLAW_AGENT_ID` | `esp32-voice` | OpenClaw agent/sub-agent ID used for voice sessions |

## Architecture Overview

```
ESP32-S3 device
      |  HTTP POST /  (JSON: device_id, message)
      v
voice_server*.py  (Python HTTP server, port 8080)
      |
      |  subprocess call
      v
openclaw CLI  --->  OpenClaw AI agent (esp32-voice)
      |                      |
      |    AI response text  |
      |<---------------------|
      |
      |  (optional) TTS: pyttsx3 / edge-tts
      v
HTTP response  --->  ESP32-S3 device
```

- The server is single-process with a threading lock around OpenClaw calls to serialise concurrent ESP32 requests.
- Session IDs are derived from `device_id` so each device maintains conversation context across turns.
- The `@ki` prefix in `voice_server_enhanced.py` bypasses the sub-agent and addresses the primary agent directly.
