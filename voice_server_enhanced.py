#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32-S3 语音交互服务器 v3.0
功能：
1. 接收ESP32消息
2. 默认转发给子智能体 esp32-voice
3. @ki 命令转发给主智能体
4. 自动回复ESP32
"""

import os
import json
import subprocess
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime

# 配置
SERVER_HOST = '0.0.0.0'
SERVER_PORT = int(os.environ.get('SERVER_PORT', '8080'))
OPENCLAW_PATH = os.environ.get('OPENCLAW_BIN', '/home/ubuntu/.npm-global/bin/openclaw')
SUB_AGENT_ID = os.environ.get('OPENCLAW_AGENT_ID', 'esp32-voice')  # 子智能体

class OpenClawManager:
    def __init__(self):
        self.lock = threading.Lock()
        self.sessions = {}  # {device_id: session_id}

    def send_to_agent(self, message, agent_id=None, device_id='default'):
        """发送消息到OpenClaw智能体"""
        with self.lock:
            try:
                # 检查是否是@ki命令（转发给主智能体）
                if message.startswith('@ki '):
                    target_agent = None  # 主智能体
                    actual_message = message[4:]  # 去掉@ki前缀
                    print(f"[OpenClaw] 转发给主智能体: {actual_message[:50]}...")
                else:
                    target_agent = agent_id or SUB_AGENT_ID
                    actual_message = message
                    print(f"[OpenClaw] 转发给子智能体 {target_agent}: {actual_message[:50]}...")

                # 构建命令
                cmd = [OPENCLAW_PATH, 'agent', '--message', actual_message, '--json']
                
                if target_agent:
                    cmd.extend(['--agent', target_agent])

                # 获取会话ID
                session_id = self.sessions.get(device_id)
                if session_id:
                    cmd.extend(['--session-id', session_id])

                # 执行命令
                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=30,
                    env={'PATH': os.environ.get('OPENCLAW_PATH_ENV', '/home/ubuntu/.npm-global/bin:/usr/bin:/bin')}
                )

                if result.returncode == 0:
                    response = json.loads(result.stdout)
                    
                    # 提取回复文本
                    reply_text = ""
                    if 'result' in response and 'payloads' in response['result']:
                        for payload in response['result']['payloads']:
                            if 'text' in payload:
                                reply_text += payload['text']

                    # 保存会话ID
                    if 'result' in response and 'meta' in response['result']:
                        agent_meta = response['result']['meta'].get('agentMeta', {})
                        new_session_id = agent_meta.get('sessionId')
                        if new_session_id:
                            self.sessions[device_id] = new_session_id

                    print(f"[OpenClaw] ✓ 收到回复: {reply_text[:100]}...")
                    
                    return {
                        'success': True,
                        'reply': reply_text,
                        'agent': target_agent or 'main'
                    }
                else:
                    print(f"[OpenClaw] ✗ 失败: {result.stderr}")
                    return {
                        'success': False,
                        'error': result.stderr
                    }
            except subprocess.TimeoutExpired:
                print(f"[OpenClaw] ✗ 超时")
                return {'success': False, 'error': 'Timeout'}
            except Exception as e:
                print(f"[OpenClaw] ✗ 异常: {e}")
                return {'success': False, 'error': str(e)}

class ESP32RequestHandler(BaseHTTPRequestHandler):
    openclaw_manager = OpenClawManager()

    def log_message(self, format, *args):
        """自定义日志"""
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        print(f"[{timestamp}] {self.address_string()} - {format % args}")

    def do_GET(self):
        """处理GET请求"""
        if self.path == '/status':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            
            status = {
                'status': 'online',
                'timestamp': datetime.now().isoformat(),
                'sub_agent': SUB_AGENT_ID,
                'version': '3.0'
            }
            self.wfile.write(json.dumps(status, ensure_ascii=False).encode('utf-8'))
        else:
            self.send_error(404)

    def do_POST(self):
        """处理POST请求"""
        content_length = int(self.headers.get('Content-Length', 0))
        post_data = self.rfile.read(content_length)

        try:
            data = json.loads(post_data.decode('utf-8'))
            device_id = data.get('device_id', 'default')
            message = data.get('message', '')

            print(f"[ESP32] 收到消息 from {device_id}: {message}")

            if not message:
                response = {'success': False, 'error': '消息为空'}
            else:
                # 转发给OpenClaw
                result = self.openclaw_manager.send_to_agent(
                    message, 
                    device_id=device_id
                )

                response = {
                    'success': result['success'],
                    'reply': result.get('reply', ''),
                    'agent': result.get('agent', 'unknown'),
                    'timestamp': datetime.now().isoformat()
                }

                if result['success']:
                    print(f"[ESP32] 回复 to {device_id}: {result.get('reply', '')[:50]}...")

            # 发送响应
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(response, ensure_ascii=False).encode('utf-8'))

        except Exception as e:
            print(f"[错误] {e}")
            import traceback
            traceback.print_exc()
            self.send_error(500, str(e))

def main():
    print("=" * 70)
    print("ESP32-S3 语音交互服务器 v3.0")
    print("=" * 70)
    print(f"监听地址: {SERVER_HOST}:{SERVER_PORT}")
    print(f"子智能体: {SUB_AGENT_ID}")
    print(f"主智能体: 使用 @ki 前缀访问")
    print("=" * 70)

    server = HTTPServer((SERVER_HOST, SERVER_PORT), ESP32RequestHandler)
    
    print("[服务器] 启动成功，等待ESP32连接...")
    print("[服务器] 按Ctrl+C停止")
    print("=" * 70)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[服务器] 正在关闭...")
        server.shutdown()
        print("[服务器] 已停止")

if __name__ == '__main__':
    main()
