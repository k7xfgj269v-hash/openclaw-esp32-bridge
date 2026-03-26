#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32-S3 语音交互服务器 v2.0
功能：
1. HTTP服务器接收ESP32请求
2. 使用pyttsx3进行语音合成
3. 使用永久的OpenClaw智能体 (esp32-voice)
4. 与ESP32双向通信
"""

import os
import json
import subprocess
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime
import pyttsx3

# 配置
SERVER_HOST = '0.0.0.0'
SERVER_PORT = int(os.environ.get('SERVER_PORT', '8080'))
OPENCLAW_PATH = os.environ.get('OPENCLAW_BIN', '/home/ubuntu/.npm-global/bin/openclaw')
OPENCLAW_AGENT_ID = os.environ.get('OPENCLAW_AGENT_ID', 'esp32-voice')  # 永久智能体ID

# OpenClaw管理器
class OpenClawManager:
    def __init__(self, agent_id):
        self.agent_id = agent_id
        self.lock = threading.Lock()
        self.session_counter = 0

    def verify_agent(self):
        """验证智能体是否存在"""
        try:
            cmd = [OPENCLAW_PATH, 'agents', 'list', '--json']
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=10,
                                  env={'PATH': os.environ.get('OPENCLAW_PATH_ENV', '/home/ubuntu/.npm-global/bin:/usr/bin:/bin')})

            if result.returncode == 0:
                agents = json.loads(result.stdout)
                for agent in agents:
                    if agent.get('id') == self.agent_id or agent.get('name') == self.agent_id:
                        print(f"[OpenClaw] ✓ 智能体已存在: {self.agent_id}")
                        return True

                print(f"[OpenClaw] ✗ 智能体不存在: {self.agent_id}")
                return False
            else:
                print(f"[OpenClaw] 验证失败: {result.stderr}")
                return False
        except Exception as e:
            print(f"[OpenClaw] 验证异常: {e}")
            return False

    def send_message(self, message, session_id=None):
        """向OpenClaw智能体发送消息"""
        with self.lock:
            try:
                # 构建命令
                cmd = [
                    OPENCLAW_PATH, 'agent',
                    '--agent', self.agent_id,
                    '--message', message,
                    '--json'
                ]

                # 如果提供了session_id，使用它来保持会话连续性
                if session_id:
                    cmd.extend(['--session-id', session_id])

                print(f"[OpenClaw] 发送消息: {message[:50]}...")

                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=60,
                    env={'PATH': os.environ.get('OPENCLAW_PATH_ENV', '/home/ubuntu/.npm-global/bin:/usr/bin:/bin')}
                )

                if result.returncode == 0:
                    response = json.loads(result.stdout)

                    # 提取智能体的回复文本
                    reply_text = ""
                    if 'result' in response and 'payloads' in response['result']:
                        for payload in response['result']['payloads']:
                            if 'text' in payload:
                                reply_text += payload['text']

                    # 提取session_id用于后续请求
                    new_session_id = None
                    if 'result' in response and 'meta' in response['result']:
                        agent_meta = response['result']['meta'].get('agentMeta', {})
                        new_session_id = agent_meta.get('sessionId')

                    print(f"[OpenClaw] ✓ 收到回复: {reply_text[:100]}...")

                    return {
                        'success': True,
                        'reply': reply_text,
                        'session_id': new_session_id,
                        'full_response': response
                    }
                else:
                    print(f"[OpenClaw] ✗ 发送失败: {result.stderr}")
                    return {
                        'success': False,
                        'error': result.stderr
                    }
            except subprocess.TimeoutExpired:
                print(f"[OpenClaw] ✗ 超时")
                return {
                    'success': False,
                    'error': 'Timeout'
                }
            except Exception as e:
                print(f"[OpenClaw] ✗ 异常: {e}")
                return {
                    'success': False,
                    'error': str(e)
                }

# 语音合成管理
class VoiceManager:
    def __init__(self):
        try:
            self.engine = pyttsx3.init()
            self.engine.setProperty('rate', 150)
            self.engine.setProperty('volume', 0.9)
            self.lock = threading.Lock()
            self.available = True
            print("[语音] ✓ pyttsx3初始化成功")
        except Exception as e:
            print(f"[语音] ✗ pyttsx3初始化失败: {e}")
            self.available = False

    def speak(self, text):
        """语音合成并播放"""
        if not self.available:
            print(f"[语音] 跳过（不可用）: {text}")
            return False

        with self.lock:
            try:
                print(f"[语音] 播放: {text}")
                self.engine.say(text)
                self.engine.runAndWait()
                return True
            except Exception as e:
                print(f"[语音] 播放失败: {e}")
                return False

# 会话管理器
class SessionManager:
    def __init__(self):
        self.sessions = {}  # {device_id: session_id}
        self.lock = threading.Lock()

    def get_session(self, device_id):
        """获取设备的会话ID"""
        with self.lock:
            return self.sessions.get(device_id)

    def set_session(self, device_id, session_id):
        """设置设备的会话ID"""
        with self.lock:
            self.sessions[device_id] = session_id
            print(f"[会话] 设备 {device_id} -> 会话 {session_id}")

# HTTP请求处理器
class ESP32RequestHandler(BaseHTTPRequestHandler):
    openclaw_manager = OpenClawManager(OPENCLAW_AGENT_ID)
    voice_manager = VoiceManager()
    session_manager = SessionManager()

    def log_message(self, format, *args):
        """自定义日志格式"""
        print(f"[HTTP] {self.address_string()} - {format % args}")

    def do_GET(self):
        """处理GET请求"""
        if self.path == '/status':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()

            status = {
                'status': 'online',
                'timestamp': datetime.now().isoformat(),
                'agent_id': OPENCLAW_AGENT_ID,
                'voice_available': self.voice_manager.available
            }
            self.wfile.write(json.dumps(status, ensure_ascii=False).encode('utf-8'))
        else:
            self.send_error(404, "Not Found")

    def do_POST(self):
        """处理POST请求"""
        content_length = int(self.headers['Content-Length'])
        post_data = self.rfile.read(content_length)

        try:
            data = json.loads(post_data.decode('utf-8'))
            action = data.get('action')
            device_id = data.get('device_id', 'default')

            if action == 'verify_agent':
                # 验证智能体是否存在
                exists = self.openclaw_manager.verify_agent()
                response = {
                    'success': exists,
                    'agent_id': OPENCLAW_AGENT_ID
                }

            elif action == 'send_message':
                # 向智能体发送消息（保持会话连续性）
                message = data.get('message')
                if not message:
                    response = {'success': False, 'error': '缺少message参数'}
                else:
                    # 获取该设备的会话ID
                    session_id = self.session_manager.get_session(device_id)

                    # 发送消息
                    result = self.openclaw_manager.send_message(message, session_id)

                    # 保存新的会话ID
                    if result.get('session_id'):
                        self.session_manager.set_session(device_id, result['session_id'])

                    response = {
                        'success': result['success'],
                        'reply': result.get('reply', ''),
                        'session_id': result.get('session_id')
                    }

            elif action == 'speak':
                # 语音合成
                text = data.get('text')
                if not text:
                    response = {'success': False, 'error': '缺少text参数'}
                else:
                    success = self.voice_manager.speak(text)
                    response = {'success': success, 'text': text}

            elif action == 'command':
                # 处理ESP32命令：语音播报 + OpenClaw处理
                command = data.get('command')
                if not command:
                    response = {'success': False, 'error': '缺少command参数'}
                else:
                    # 语音播报
                    self.voice_manager.speak(f"收到命令：{command}")

                    # 获取会话ID
                    session_id = self.session_manager.get_session(device_id)

                    # 发送给OpenClaw处理
                    result = self.openclaw_manager.send_message(command, session_id)

                    # 保存会话ID
                    if result.get('session_id'):
                        self.session_manager.set_session(device_id, result['session_id'])

                    # 语音播报回复（如果有）
                    if result.get('reply'):
                        reply_preview = result['reply'][:100]
                        self.voice_manager.speak(f"智能体回复：{reply_preview}")

                    response = {
                        'success': result['success'],
                        'command': command,
                        'reply': result.get('reply', ''),
                        'session_id': result.get('session_id')
                    }

            else:
                response = {'success': False, 'error': f'未知操作: {action}'}

            # 发送响应
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(response, ensure_ascii=False).encode('utf-8'))

        except Exception as e:
            print(f"[HTTP] 处理请求异常: {e}")
            import traceback
            traceback.print_exc()
            self.send_error(500, f"Internal Server Error: {str(e)}")

def main():
    print("=" * 70)
    print("ESP32-S3 语音交互服务器 v2.0")
    print("=" * 70)
    print(f"服务器地址: http://{SERVER_HOST}:{SERVER_PORT}")
    print(f"OpenClaw智能体: {OPENCLAW_AGENT_ID}")
    print(f"时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("=" * 70)

    # 验证OpenClaw智能体
    manager = OpenClawManager(OPENCLAW_AGENT_ID)
    if manager.verify_agent():
        print(f"[启动] ✓ OpenClaw智能体 '{OPENCLAW_AGENT_ID}' 已就绪")
    else:
        print(f"[启动] ✗ 警告: OpenClaw智能体 '{OPENCLAW_AGENT_ID}' 不存在")
        print(f"[启动] 请先创建智能体:")
        print(f"[启动]   openclaw agents add {OPENCLAW_AGENT_ID}")

    # 创建HTTP服务器
    server = HTTPServer((SERVER_HOST, SERVER_PORT), ESP32RequestHandler)

    print("[服务器] 启动成功，等待ESP32连接...")
    print("[服务器] 按Ctrl+C停止服务器")
    print("=" * 70)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[服务器] 正在关闭...")
        server.shutdown()
        print("[服务器] 已停止")

if __name__ == '__main__':
    main()
