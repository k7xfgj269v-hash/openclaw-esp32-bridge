"""pytest 根配置：在测试模块导入服务器之前，把 openclaw 替换为桩二进制"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_STUB = '''#!/usr/bin/env python3
import json, sys

def arg(flag):
    return sys.argv[sys.argv.index(flag) + 1] if flag in sys.argv else None

if sys.argv[1:2] == ['agents']:
    print(json.dumps([{"id": "esp32-voice", "name": "esp32-voice"}]))
    sys.exit(0)

agent = arg('--agent') or 'main'
session = arg('--session-id') or 'none'
message = arg('--message') or ''
out = {
    "status": "ok",
    "result": {
        "payloads": [{"text": f"agent={agent};session={session};msg={message}"}, {"text": ";p2"}],
        "meta": {"agentMeta": {"sessionId": f"sess-{agent}"}},
    },
}
print(json.dumps(out))
'''


def _make_stub():
    d = tempfile.mkdtemp(prefix='ocb_stub_')
    path = os.path.join(d, 'openclaw')
    with open(path, 'w') as f:
        f.write(_STUB)
    os.chmod(path, 0o755)
    return path


os.environ['OPENCLAW_BIN'] = _make_stub()
os.environ['OPENCLAW_AGENT_ID'] = 'esp32-voice'
os.environ['OPENCLAW_TIMEOUT'] = '10'
os.environ.pop('OPENCLAW_PATH_ENV', None)
os.environ['SERVER_TOKEN'] = 'test-server-token'
