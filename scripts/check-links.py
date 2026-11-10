#!/usr/bin/env python3
"""全仓库 Markdown 链接校验器（SUMMARY + 正文交叉引用）

用法：python3 scripts/check-links.py
规则：
  1. 校验所有 .md 文件正文中的相对 .md 链接（跨分册交叉引用）
  2. 校验所有 SUMMARY.md 的目录条目
  3. 跳过符号链接别名目录（memory/、LSM/——canonical 为 mm/、admin-guide/LSM/）
     与 bpfman/（用户遗留区）
退出码：发现断链时非零（便于 CI/定时轮次使用）
"""
import os
import re
import sys
import glob

LINK_PAT = re.compile(r'\]\(([^)#?\s]+\.md)\)')
# 符号链接别名（正文链接在 canonical 路径下校验）
ALIAS_DIRS = ('memory/', 'LSM/')
SKIP_PARTS = ('/bpfman/',)


def check():
    bad, checked = [], 0
    for f in glob.glob('**/*.md', recursive=True):
        if any(f.startswith(a) for a in ALIAS_DIRS):
            continue
        if any(p in f for p in SKIP_PARTS):
            continue
        try:
            text = open(f, encoding='utf-8').read()
        except OSError:
            continue
        for m in LINK_PAT.finditer(text):
            target = m.group(1)
            if target.startswith(('http', '/')):
                continue
            checked += 1
            p = os.path.normpath(os.path.join(os.path.dirname(f), target))
            if not os.path.exists(p):
                line = text[:m.start()].count('\n') + 1
                bad.append(f'{f}:{line} -> {target}')
    print(f'checked {checked} markdown links')
    if bad:
        print('\n'.join(bad))
        print(f'BROKEN: {len(bad)}')
        return 1
    print('ALL LINKS OK')
    return 0


if __name__ == '__main__':
    sys.exit(check())
