#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""加载仓库根目录的 .env 到环境变量（已存在的变量不覆盖），无第三方依赖"""

import os


def load_dotenv(filename='.env'):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), filename)
    if not os.path.exists(path):
        return
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or '=' not in line:
                continue
            key, _, value = line.partition('=')
            os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))
