#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESP32 语音助手服务器 - 使用 OpenClaw esp32-voice 子智能体"""

import os
import json
import struct
import subprocess
import threading
import tempfile
import shutil
import asyncio
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from datetime import datetime

from envcfg import load_dotenv

load_dotenv()

SERVER_HOST = os.environ.get('SERVER_HOST', '0.0.0.0')
SERVER_PORT = int(os.environ.get('SERVER_PORT', '8080'))

OPENCLAW_BIN = os.environ.get('OPENCLAW_BIN', '/home/ubuntu/.npm-global/bin/openclaw')
AGENT_NAME = os.environ.get('OPENCLAW_AGENT_ID', 'esp32-voice')
SESSION_PREFIX = 'esp32_'
OPENCLAW_TIMEOUT = int(os.environ.get('OPENCLAW_TIMEOUT', '60'))
MAX_TEXT_BYTES = 1024 * 1024        # 文本请求体上限
MAX_AUDIO_BYTES = 20 * 1024 * 1024  # PCM 音频上限（16kHz 16bit 单声道约 10 分钟）

SYSTEM_PROMPT = """你是ESP32语音助手。用户通过语音交互，你的回复会被语音播报。

严格遵守以下规则：
1. 回复必须是纯文本，不使用任何Markdown格式
2. 不使用表情符号
3. 使用口语化、自然的表达
4. 直接回答，不要客套话
"""

# 语音识别模型（懒加载一次；启动时在 __main__ 预热，保持模块可被测试导入）
whisper_model = None
_whisper_lock = threading.Lock()


def get_whisper():
    global whisper_model
    with _whisper_lock:
        if whisper_model is None:
            from faster_whisper import WhisperModel
            print("[初始化] 正在加载 Whisper 模型（tiny）...")
            whisper_model = WhisperModel("tiny", device="cpu", compute_type="int8")
            print("[初始化] Whisper 模型加载完成")
    return whisper_model


def openclaw_env():
    """继承当前环境，仅覆盖 PATH，保留 HOME 等 openclaw 依赖的变量"""
    env = dict(os.environ)
    path_env = os.environ.get('OPENCLAW_PATH_ENV')
    if path_env:
        env['PATH'] = path_env
    return env


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
    import edge_tts
    communicate = edge_tts.Communicate(text, voice="zh-CN-XiaoxiaoNeural")
    await communicate.save(output_file)


class OpenClawSubagentHandler(BaseHTTPRequestHandler):
    openclaw_lock = threading.Lock()   # 串行化智能体调用
    primed_lock = threading.Lock()
    primed_sessions = set()            # 已注入过系统提示词的会话

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
        tmp_files = []
        try:
            length = int(self.headers.get('Content-Length', 0))
            if length > MAX_AUDIO_BYTES:
                self.send_error(413, "Payload Too Large")
                return
            pcm_data = self.rfile.read(length)
            print(f"[语音请求] 收到 PCM 数据: {len(pcm_data)} 字节")

            # 1. 加 WAV 头写入临时文件（每请求唯一文件，避免并发互相覆盖）
            wav_data = build_wav_header(pcm_data, sample_rate=16000, channels=1, bits=16)
            fd, input_wav = tempfile.mkstemp(prefix='esp32_in_', suffix='.wav', dir='/tmp')
            tmp_files.append(input_wav)
            with os.fdopen(fd, 'wb') as f:
                f.write(wav_data)

            # 2. Whisper 语音识别（模型推理加锁）
            model = get_whisper()
            with _whisper_lock:
                segments, _ = model.transcribe(input_wav, language="zh")
                recognized_text = "".join([seg.text for seg in segments]).strip()
            print(f"[语音识别] {recognized_text!r}")
            if not recognized_text:
                recognized_text = "你好"

            # 3. OpenClaw AI 处理
            session_id = f"{SESSION_PREFIX}voice"
            reply_text = self.call_openclaw_agent(session_id, recognized_text)
            print(f"[AI回复] {reply_text[:100]}")

            # 4. edge-tts 转语音
            fd, mp3_output = tempfile.mkstemp(prefix='esp32_out_', suffix='.mp3', dir='/tmp')
            os.close(fd)
            tmp_files.append(mp3_output)
            asyncio.run(_tts_async(reply_text, mp3_output))

            # 5. ffmpeg 转为 16kHz 16bit 单声道 WAV
            fd, wav_output = tempfile.mkstemp(prefix='esp32_out_', suffix='.wav', dir='/tmp')
            os.close(fd)
            tmp_files.append(wav_output)
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
            # 先按字符截断再编码，避免把 UTF-8 多字节字符切断
            self.send_header('X-AI-Reply', reply_text[:60].encode('utf-8').decode('latin-1'))
            self.send_header('Content-Type', 'audio/wav')
            self.send_header('Content-Length', str(len(wav_bytes)))
            self.end_headers()
            self.wfile.write(wav_bytes)

        except Exception as e:
            print(f"[语音接口错误] {e}")
            import traceback; traceback.print_exc()
            error_msg = "语音处理失败".encode('utf-8')
            self.send_response(500)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.send_header('Content-Length', str(len(error_msg)))
            self.end_headers()
            self.wfile.write(error_msg)
        finally:
            for path in tmp_files:
                try:
                    os.unlink(path)
                except OSError:
                    pass

    def handle_text_request(self):
        """原有 / 文本接口"""
        try:
            length = int(self.headers.get('Content-Length', 0))
            if length > MAX_TEXT_BYTES:
                self.send_error(413, "Payload Too Large")
                return
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
            error_bytes = json.dumps({'success': False, 'error': 'internal error', 'reply': '处理错误'}, ensure_ascii=False).encode('utf-8')
            self.send_response(500)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(error_bytes)))
            self.end_headers()
            self.wfile.write(error_bytes)

    def call_openclaw_agent(self, session_id, message):
        try:
            # 系统提示词只在会话首条消息注入，避免每轮重复污染对话历史
            with self.primed_lock:
                primed = session_id in self.primed_sessions
            full_message = message if primed else f"{SYSTEM_PROMPT}\n\n用户消息：{message}"
            cmd = [OPENCLAW_BIN, 'agent', '--agent', AGENT_NAME,
                   '--session-id', session_id, '--message', full_message, '--json']
            print(f"[OpenClaw] session: {session_id}")
            with self.openclaw_lock:
                result = subprocess.run(cmd, capture_output=True, text=True,
                                        timeout=OPENCLAW_TIMEOUT, env=openclaw_env())
            if result.returncode != 0:
                print(f"[OpenClaw错误] stderr: {result.stderr}")
                return "智能体调用失败"
            response_data = json.loads(result.stdout)
            if response_data.get('status') == 'ok':
                if not primed:
                    with self.primed_lock:
                        self.primed_sessions.add(session_id)
                payloads = response_data.get('result', {}).get('payloads', [])
                reply_text = ''.join(p.get('text', '') for p in payloads)
                return reply_text if reply_text else '无响应'
            print(f"[OpenClaw返回错误] {response_data.get('summary', 'unknown')}")
            return "智能体返回错误"
        except subprocess.TimeoutExpired:
            return "请求超时"
        except Exception as e:
            print(f"[OpenClaw调用异常] {e}")
            return "调用失败"


if __name__ == '__main__':
    if shutil.which('ffmpeg') is None:
        print("[警告] 未找到 ffmpeg，/voice 接口将不可用（brew/apt install ffmpeg）")
    get_whisper()
    print(f"服务器启动: {SERVER_HOST}:{SERVER_PORT}")
    print(f"接口: POST / (JSON文本)  POST /voice (PCM音频)")
    print("等待 ESP32 连接...\n")
    try:
        ThreadingHTTPServer((SERVER_HOST, SERVER_PORT), OpenClawSubagentHandler).serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止")
