#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESP32 语音助手服务器 - 使用 OpenClaw esp32-voice 子智能体"""

import os
import json
import struct
import subprocess
import asyncio
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime

# 语音识别模型（模块级别加载一次）
from faster_whisper import WhisperModel
print("[初始化] 正在加载 Whisper 模型（tiny）...")
whisper_model = WhisperModel("tiny", device="cpu", compute_type="int8")
print("[初始化] Whisper 模型加载完成")

import edge_tts

SERVER_HOST = '0.0.0.0'
SERVER_PORT = int(os.environ.get('SERVER_PORT', '8080'))

OPENCLAW_BIN = os.environ.get('OPENCLAW_BIN', '/home/ubuntu/.npm-global/bin/openclaw')
AGENT_NAME = os.environ.get('OPENCLAW_AGENT_ID', 'esp32-voice')
SESSION_PREFIX = 'esp32_'

SYSTEM_PROMPT = """你是ESP32语音助手。用户通过语音交互，你的回复会被语音播报。

严格遵守以下规则：
1. 回复必须是纯文本，不使用任何Markdown格式
2. 不使用表情符号
3. 使用口语化、自然的表达
4. 直接回答，不要客套话
"""


def build_wav_header(pcm_data, sample_rate=16000, channels=1, bits=16):
    data_size = len(pcm_data)
    header = struct.pack('<4sI4s4sIHHIIHH4sI',
        b'RIFF', 36 + data_size, b'WAVE',
        b'fmt ', 16, 1, channels, sample_rate,
        sample_rate * channels * bits // 8,
        channels * bits // 8, bits,
        b'data', data_size)
    return header + pcm_data


async def _tts_async(text, output_file):
    communicate = edge_tts.Communicate(text, voice="zh-CN-XiaoxiaoNeural")
    await communicate.save(output_file)


class OpenClawSubagentHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        print(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] {format % args}")

    def do_GET(self):
        response = json.dumps({'status': 'ok', 'service': 'OpenClaw ESP32-Voice Agent Server'})
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(response)))
        self.end_headers()
        self.wfile.write(response.encode())

    def do_POST(self):
        if self.path == '/voice':
            self.handle_voice_request()
        else:
            self.handle_text_request()

    def handle_voice_request(self):
        """处理 /voice 接口：PCM输入 → STT → AI → TTS → WAV输出"""
        try:
            length = int(self.headers.get('Content-Length', 0))
            pcm_data = self.rfile.read(length)
            print(f"[语音请求] 收到 PCM 数据: {len(pcm_data)} 字节")

            # 1. 加 WAV 头写入临时文件
            wav_data = build_wav_header(pcm_data, sample_rate=16000, channels=1, bits=16)
            input_wav = "/tmp/esp32_input.wav"
            with open(input_wav, 'wb') as f:
                f.write(wav_data)

            # 2. Whisper 语音识别
            segments, _ = whisper_model.transcribe(input_wav, language="zh")
            recognized_text = "".join([seg.text for seg in segments]).strip()
            print(f"[语音识别] {recognized_text!r}")
            if not recognized_text:
                recognized_text = "你好"

            # 3. OpenClaw AI 处理
            session_id = f"{SESSION_PREFIX}voice"
            reply_text = self.call_openclaw_agent(session_id, recognized_text)
            print(f"[AI回复] {reply_text[:100]}")

            # 4. edge-tts 转语音
            mp3_output = "/tmp/esp32_output.mp3"
            asyncio.run(_tts_async(reply_text, mp3_output))

            # 5. ffmpeg 转为 16kHz 16bit 单声道 WAV
            wav_output = "/tmp/esp32_output.wav"
            result = subprocess.run(
                ["ffmpeg", "-y", "-i", mp3_output,
                 "-ar", "16000", "-ac", "1", "-sample_fmt", "s16", wav_output],
                capture_output=True, timeout=30
            )
            if result.returncode != 0:
                raise RuntimeError(f"ffmpeg失败: {result.stderr.decode()}")

            # 6. 返回 WAV 音频
            with open(wav_output, 'rb') as f:
                wav_bytes = f.read()

            print(f"[语音响应] WAV 大小: {len(wav_bytes)} 字节")
            self.send_response(200)
            self.send_header('X-AI-Reply', reply_text.encode('utf-8').decode('latin-1', errors='replace')[:200])
            self.send_header('Content-Type', 'audio/wav')
            self.send_header('Content-Length', str(len(wav_bytes)))
            self.end_headers()
            self.wfile.write(wav_bytes)

        except Exception as e:
            print(f"[语音接口错误] {e}")
            import traceback; traceback.print_exc()
            error_msg = f"语音处理失败: {str(e)}".encode('utf-8')
            self.send_response(500)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.send_header('Content-Length', str(len(error_msg)))
            self.end_headers()
            self.wfile.write(error_msg)

    def handle_text_request(self):
        """原有 / 文本接口"""
        try:
            length = int(self.headers.get('Content-Length', 0))
            data = self.rfile.read(length)
            json_data = json.loads(data.decode('utf-8'))
            device_id = json_data.get('device_id', 'unknown')
            message = json_data.get('message', '')
            print(f"[收到消息] 设备: {device_id}, 内容: {message}")

            session_id = f"{SESSION_PREFIX}{device_id}"
            ai_reply = self.call_openclaw_agent(session_id, message)

            response = {
                'success': True,
                'reply': ai_reply,
                'timestamp': datetime.now().isoformat(),
                'device_id': device_id,
                'agent': AGENT_NAME
            }
            print(f"[AI回复] {ai_reply[:100]}...")
            response_bytes = json.dumps(response, ensure_ascii=False).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(response_bytes)))
            self.end_headers()
            self.wfile.write(response_bytes)

        except Exception as e:
            print(f"[错误] {e}")
            import traceback; traceback.print_exc()
            error_bytes = json.dumps({'success': False, 'error': str(e), 'reply': '处理错误'}, ensure_ascii=False).encode('utf-8')
            self.send_response(500)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(error_bytes)))
            self.end_headers()
            self.wfile.write(error_bytes)

    def call_openclaw_agent(self, session_id, message):
        try:
            full_message = f"{SYSTEM_PROMPT}\n\n用户消息：{message}"
            cmd = [OPENCLAW_BIN, 'agent', '--agent', AGENT_NAME,
                   '--session-id', session_id, '--message', full_message, '--json']
            print(f"[OpenClaw] session: {session_id}")
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            if result.returncode != 0:
                return f"调用失败: {result.stderr}"
            response_data = json.loads(result.stdout)
            if response_data.get('status') == 'ok':
                payloads = response_data.get('result', {}).get('payloads', [])
                return payloads[0].get('text', '无响应') if payloads else '无响应'
            return f"错误: {response_data.get('summary', 'unknown')}"
        except subprocess.TimeoutExpired:
            return "请求超时"
        except Exception as e:
            return f"调用失败: {str(e)}"


if __name__ == '__main__':
    print(f"服务器启动: {SERVER_HOST}:{SERVER_PORT}")
    print(f"接口: POST / (JSON文本)  POST /voice (PCM音频)")
    print("等待 ESP32 连接...\n")
    try:
        HTTPServer((SERVER_HOST, SERVER_PORT), OpenClawSubagentHandler).serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止")
