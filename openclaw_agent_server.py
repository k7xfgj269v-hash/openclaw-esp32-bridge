#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESP32 语音助手服务器 - 连接 OpenClaw 子智能体"""

import os
import json
import subprocess
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime

SERVER_HOST = '0.0.0.0'
SERVER_PORT = int(os.environ.get('SERVER_PORT', '8080'))

# OpenClaw 配置
OPENCLAW_SESSION_PREFIX = 'esp32_voice_'
OPENCLAW_BIN = os.environ.get('OPENCLAW_BIN', '/home/ubuntu/.npm-global/bin/openclaw')

class OpenClawHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        print(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] {format % args}")

    def do_GET(self):
        response = json.dumps({'status': 'ok', 'service': 'OpenClaw Agent Server'})
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(response)))
        self.end_headers()
        self.wfile.write(response.encode())

    def do_POST(self):
        try:
            length = int(self.headers.get('Content-Length', 0))
            data = self.rfile.read(length)
            
            # 解析请求
            json_data = json.loads(data.decode('utf-8'))
            device_id = json_data.get('device_id', 'unknown')
            message = json_data.get('message', '')
            
            print(f"[收到消息] 设备: {device_id}, 内容: {message}")
            
            # 调用 OpenClaw agent
            session_id = f"{OPENCLAW_SESSION_PREFIX}{device_id}"
            ai_reply = self.call_openclaw_agent(session_id, message)
            
            # 构建响应
            response = {
                'success': True,
                'reply': ai_reply,
                'timestamp': datetime.now().isoformat(),
                'session_id': session_id
            }
            
            print(f"[AI回复] {ai_reply[:100]}...")
            
            # 发送响应
            response_json = json.dumps(response, ensure_ascii=False)
            response_bytes = response_json.encode('utf-8')
            
            self.send_response(200)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(response_bytes)))
            self.end_headers()
            self.wfile.write(response_bytes)
            
        except Exception as e:
            print(f"[错误] {e}")
            import traceback
            traceback.print_exc()
            
            error_response = json.dumps({
                'success': False,
                'error': str(e),
                'reply': '抱歉，处理您的请求时出现错误'
            }, ensure_ascii=False)
            error_bytes = error_response.encode('utf-8')
            
            self.send_response(500)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(error_bytes)))
            self.end_headers()
            self.wfile.write(error_bytes)

    def call_openclaw_agent(self, session_id, message):
        """调用 OpenClaw agent API"""
        try:
            # 构建 openclaw 命令
            cmd = [
                OPENCLAW_BIN, 'agent',
                '--session-id', session_id,
                '--message', message,
                '--json'
            ]
            
            print(f"[OpenClaw] 执行命令: {' '.join(cmd)}")
            
            # 执行命令
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=60
            )
            
            if result.returncode != 0:
                print(f"[OpenClaw错误] stderr: {result.stderr}")
                return f"OpenClaw 调用失败: {result.stderr}"
            
            # 解析 JSON 输出
            try:
                response_data = json.loads(result.stdout)
                
                # 提取 AI 回复文本
                if response_data.get('status') == 'ok':
                    payloads = response_data.get('result', {}).get('payloads', [])
                    if payloads and len(payloads) > 0:
                        reply_text = payloads[0].get('text', '无响应')
                        return reply_text
                    else:
                        return '无响应'
                else:
                    return f"OpenClaw 返回错误: {response_data.get('summary', 'unknown')}"
                
            except json.JSONDecodeError as e:
                print(f"[JSON解析错误] {e}")
                print(f"[OpenClaw原始输出] {result.stdout[:500]}")
                return "解析 AI 响应失败"
                
        except subprocess.TimeoutExpired:
            return "请求超时，请稍后再试"
        except Exception as e:
            print(f"[OpenClaw调用异常] {e}")
            import traceback
            traceback.print_exc()
            return f"调用 AI 服务失败: {str(e)}"

if __name__ == '__main__':
    print(f"OpenClaw Agent 服务器启动在 {SERVER_HOST}:{SERVER_PORT}")
    print(f"Session 前缀: {OPENCLAW_SESSION_PREFIX}")
    print(f"OpenClaw 路径: {OPENCLAW_BIN}")
    print("等待 ESP32 连接...\n")
    
    try:
        HTTPServer((SERVER_HOST, SERVER_PORT), OpenClawHandler).serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止")
