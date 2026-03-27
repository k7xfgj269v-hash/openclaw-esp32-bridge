# ESP32 Voice Bridge — KI-Sprachschnittstelle für Mikrocontroller

HTTP-Bridge zwischen ESP32-S3-Hardware und KI-Agenten mit vollständiger Sprachverarbeitungs-Pipeline (STT + TTS).

```
ESP32-S3 Gerät
      │  HTTP POST (JSON)
      ▼
Voice Bridge Server (Python)
      │  Subprocess
      ▼
KI-Agent (OpenClaw / beliebiger Agent)
      │  Antworttext
      ▼
TTS-Synthese → HTTP-Antwort → ESP32
```

---

## Einsatzbereiche

- Sprachgesteuerte Embedded-Systeme
- IoT-Geräte mit KI-Assistenz
- Industrielle Mensch-Maschine-Schnittstellen
- Prototypen für sprachbasierte Steuerung

---

## Voraussetzungen

- Python 3.8 oder höher
- OpenClaw CLI (oder kompatibler KI-Agent)
- ESP32-S3 Mikrocontroller

---

## Installation

```bash
# Repository klonen
git clone https://github.com/k7xfgj269v-hash/openclaw-esp32-bridge.git
cd openclaw-esp32-bridge

# Abhängigkeiten installieren
pip install -r requirements.txt

# Für vollständige Sprachpipeline (STT + TTS)
pip install faster-whisper edge-tts
```

---

## Konfiguration

```bash
cp .env.example .env
```

| Variable | Standard | Beschreibung |
|----------|----------|--------------|
| `SERVER_PORT` | `8080` | Server-Port |
| `OPENCLAW_BIN` | `/usr/bin/openclaw` | Pfad zum KI-Agent-Binary |
| `OPENCLAW_AGENT_ID` | `esp32-voice` | Agent-ID für Sprachsitzungen |

---

## Verfügbare Server-Varianten

| Skript | Beschreibung |
|--------|--------------|
| `voice_server.py` | Basis-Server · pyttsx3 TTS |
| `voice_server_enhanced.py` | Erweiterter Server · Dual-Agent-Routing |
| `openclaw_agent_server.py` | Minimale HTTP-Wrapper (kein TTS) |
| `openclaw_subagent_server.py` | Vollständige Pipeline · Whisper STT + Edge TTS |

### Empfohlen für die meisten Anwendungsfälle:

```bash
python voice_server_enhanced.py
```

---

## ESP32-Anforderungsformat

HTTP POST an `http://<server-ip>:8080/`:

```json
{
  "device_id": "esp32-01",
  "message": "Temperatur anzeigen"
}
```

**Sitzungsverwaltung:** Jedes Gerät (`device_id`) erhält eine eigene Konversationssitzung — Kontext bleibt zwischen Anfragen erhalten.

---

## Technische Details

- **Nebenläufigkeit:** Threading-Lock serialisiert KI-Aufrufe, verhindert Race Conditions
- **Sitzungs-IDs:** Abgeleitet aus `device_id`, persistente Gesprächshistorie pro Gerät
- **Dual-Routing:** Präfix `@ki` leitet Nachrichten an primären Agenten statt Sub-Agenten
- **TTS-Optionen:** pyttsx3 (lokal, offline) oder Edge TTS (Microsoft Azure, höhere Qualität)

---

## Sprachverarbeitungs-Pipeline (Subagent-Server)

```
ESP32 sendet Audio-Transkription
    │
    ▼
faster-whisper (lokale STT)
    │  Transkribierter Text
    ▼
KI-Agent (Verarbeitung)
    │  Antworttext
    ▼
edge-tts (Sprachsynthese)
    │  Audio-Stream
    ▼
ESP32 empfängt Sprachantwort
```

---

<details>
<summary>English</summary>

# ESP32 Voice Bridge — AI Voice Interface for Microcontrollers

HTTP bridge connecting ESP32-S3 hardware to AI agents with full voice pipeline (STT + TTS).

**Use cases:** Voice-controlled embedded systems · IoT devices with AI assistance · Industrial HMI prototypes

**Quick start:**
```bash
pip install -r requirements.txt
python voice_server_enhanced.py
```

**ESP32 request format:**
```json
{ "device_id": "esp32-01", "message": "your command here" }
```

Each `device_id` gets its own persistent conversation session.

**Server variants:** Basic · Enhanced (dual-agent routing) · Minimal (no TTS) · Full pipeline (Whisper STT + Edge TTS)

</details>
