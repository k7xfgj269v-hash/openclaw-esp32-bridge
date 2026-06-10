import http.client
import json
import os
import threading
import urllib.request
from http.server import ThreadingHTTPServer

import openclaw_agent_server as oas
import openclaw_subagent_server as oss
import voice_server as vs
import voice_server_enhanced as vse
from envcfg import load_dotenv


def start_server(handler_cls):
    server = ThreadingHTTPServer(('127.0.0.1', 0), handler_cls)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, server.server_address[1]


def post_json(port, payload, path='/'):
    req = urllib.request.Request(
        f'http://127.0.0.1:{port}{path}',
        data=json.dumps(payload).encode('utf-8'),
        headers={'Content-Type': 'application/json'},
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read().decode('utf-8'))


# ---------- envcfg ----------

def test_envcfg_loads_without_overriding(tmp_path, monkeypatch):
    f = tmp_path / 'x.env'
    f.write_text('OCB_NEW=bar\n# kommentar\nOCB_EXISTING=new\nOCB_QUOTED="qv"\nkaputt\n')
    monkeypatch.setenv('OCB_EXISTING', 'old')
    os.environ.pop('OCB_NEW', None)
    os.environ.pop('OCB_QUOTED', None)
    try:
        load_dotenv(str(f))
        assert os.environ['OCB_NEW'] == 'bar'
        assert os.environ['OCB_EXISTING'] == 'old'
        assert os.environ['OCB_QUOTED'] == 'qv'
    finally:
        os.environ.pop('OCB_NEW', None)
        os.environ.pop('OCB_QUOTED', None)


# ---------- voice_server_enhanced：@ki 路由与会话隔离 ----------

def test_enhanced_session_isolation_between_agents():
    m = vse.OpenClawManager()
    r1 = m.send_to_agent('hello', device_id='dev1')
    assert r1['success']
    assert 'agent=esp32-voice' in r1['reply'] and 'session=none' in r1['reply']
    assert ';p2' in r1['reply']  # 多个 payload 全部拼接

    r2 = m.send_to_agent('again', device_id='dev1')
    assert 'session=sess-esp32-voice' in r2['reply']  # 子智能体会话延续

    r3 = m.send_to_agent('@ki status', device_id='dev1')
    assert r3['agent'] == 'main' and 'agent=main' in r3['reply']
    assert 'session=none' in r3['reply']  # 不得带入子智能体的会话

    r4 = m.send_to_agent('@ki more', device_id='dev1')
    assert 'session=sess-main' in r4['reply']  # 主智能体自己的会话延续

    r5 = m.send_to_agent('back', device_id='dev1')
    assert 'session=sess-esp32-voice' in r5['reply']  # 子智能体会话未被覆盖


def test_enhanced_ki_without_message_is_error():
    m = vse.OpenClawManager()
    r = m.send_to_agent('@ki', device_id='dev2')
    assert r['success'] is False


def test_enhanced_http_roundtrip():
    server, port = start_server(vse.ESP32RequestHandler)
    try:
        data = post_json(port, {'device_id': 'hdev', 'message': 'ping'})
        assert data['success'] and data['agent'] == 'esp32-voice'
        assert 'msg=ping' in data['reply']
        with urllib.request.urlopen(f'http://127.0.0.1:{port}/status', timeout=15) as r:
            status = json.loads(r.read().decode('utf-8'))
        assert status['status'] == 'online'
    finally:
        server.shutdown()


# ---------- voice_server：action API 与会话管理 ----------

def test_voice_server_session_continuity():
    server, port = start_server(vs.ESP32RequestHandler)
    try:
        d1 = post_json(port, {'action': 'send_message', 'device_id': 'vdev', 'message': 'hi'})
        assert d1['success'] and d1['session_id'] == 'sess-esp32-voice'
        assert 'agent=esp32-voice' in d1['reply']
        d2 = post_json(port, {'action': 'send_message', 'device_id': 'vdev', 'message': 'again'})
        assert 'session=sess-esp32-voice' in d2['reply']
        unknown = post_json(port, {'action': 'nope'})
        assert unknown['success'] is False
    finally:
        server.shutdown()


# ---------- openclaw_agent_server：--agent 与 payload 拼接 ----------

def test_agent_server_targets_subagent_and_joins_payloads():
    server, port = start_server(oas.OpenClawHandler)
    try:
        data = post_json(port, {'device_id': 'd1', 'message': 'hi'})
        assert data['success']
        assert 'agent=esp32-voice' in data['reply']
        assert 'session=esp32_voice_d1' in data['reply']
        assert ';p2' in data['reply']
    finally:
        server.shutdown()


def test_agent_server_rejects_oversized_body():
    server, port = start_server(oas.OpenClawHandler)
    try:
        conn = http.client.HTTPConnection('127.0.0.1', port, timeout=15)
        conn.putrequest('POST', '/')
        conn.putheader('Content-Length', str(100 * 1024 * 1024))
        conn.endheaders()
        resp = conn.getresponse()
        assert resp.status == 413
        conn.close()
    finally:
        server.shutdown()


# ---------- openclaw_subagent_server：系统提示词只注入一次 ----------

def test_subagent_prompt_primed_once():
    oss.OpenClawSubagentHandler.primed_sessions.clear()
    server, port = start_server(oss.OpenClawSubagentHandler)
    try:
        d1 = post_json(port, {'device_id': 'p1', 'message': '你好'})
        assert d1['success']
        assert '严格遵守' in d1['reply']  # 首条消息带系统提示词
        d2 = post_json(port, {'device_id': 'p1', 'message': '再来'})
        assert '严格遵守' not in d2['reply']  # 后续消息不再重复注入
        assert 'msg=再来' in d2['reply']
        assert 'session=esp32_p1' in d2['reply']
        d3 = post_json(port, {'device_id': 'p2', 'message': '新设备'})
        assert '严格遵守' in d3['reply']  # 新会话重新注入
    finally:
        server.shutdown()
