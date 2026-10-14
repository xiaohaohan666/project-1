"""校验 PWA 产物：manifest 结构、图标尺寸、JS 括号配对。"""
import io
import os
import sys
import json
import struct
import urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

BASE = 'http://127.0.0.1:8090'
WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web')

print('=== 1) manifest 必备字段 ===')
m = json.loads(urllib.request.urlopen(BASE + '/manifest.webmanifest').read().decode('utf-8'))
required = ['name', 'short_name', 'start_url', 'scope', 'display',
            'background_color', 'theme_color', 'icons']
for k in required:
    v = m.get(k)
    ok = 'OK ' if v else 'MISSING'
    print(f'  [{ok}] {k:<17} = {v if not isinstance(v, list) else str(len(v)) + " 项"}')
maskable = [i['src'] for i in m['icons'] if i.get('purpose') == 'maskable']
print('  maskable 图标:', maskable)
print('  图标尺寸清单:', [i['sizes'] for i in m['icons']])
print('  快捷方式:', [s['name'] for s in m.get('shortcuts', [])])

print('=== 2) 图标文件名与声明是否一致 + 实际尺寸 ===')


def png_size(path):
    with open(path, 'rb') as f:
        head = f.read(24)
    if head[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    w, h = struct.unpack('>II', head[16:24])
    return w, h


for item in m['icons'] + [{'src': 'icons/apple-touch-icon.png', 'sizes': '180x180'}]:
    p = os.path.join(WEB, item['src'].replace('/', os.sep))
    if not os.path.exists(p):
        print(f'  [MISSING] {item["src"]}')
        continue
    size = png_size(p)
    declared = item.get('sizes', '?')
    real = f'{size[0]}x{size[1]}' if size else '不是PNG'
    match = 'OK ' if real == declared else 'MISMATCH'
    print(f'  [{match}] {item["src"]:<28} 声明 {declared:<9} 实际 {real}')

print('=== 3) 预缓存清单里的文件是否都存在 ===')
sw = open(os.path.join(WEB, 'sw.js'), encoding='utf-8').read()
start = sw.index('SHELL_FILES = [')
end = sw.index('];', start)
files = [ln.strip().strip(',').strip("'") for ln in sw[start:end].splitlines()[1:]]
for f in files:
    if f == '/':
        continue
    p = os.path.join(WEB, f.lstrip('/').replace('/', os.sep))
    print(f'  [{"OK " if os.path.exists(p) else "MISSING"}] {f}')

print('=== 4) JS 括号配对检查 ===')
for name in ['js/api.js', 'js/ui.js', 'js/app.js', 'js/pwa.js', 'sw.js']:
    src = open(os.path.join(WEB, name), encoding='utf-8').read()
    # 粗略剔除字符串与注释后统计括号
    out, i, n = [], 0, len(src)
    state = None
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ''
        if state is None:
            if c == '/' and nxt == '/':
                while i < n and src[i] != '\n':
                    i += 1
                continue
            if c == '/' and nxt == '*':
                i += 2
                while i < n - 1 and not (src[i] == '*' and src[i + 1] == '/'):
                    i += 1
                i += 2
                continue
            if c in '\'"`':
                state = c
                i += 1
                continue
            out.append(c)
        else:
            if c == '\\':
                i += 2
                continue
            if c == state:
                state = None
        i += 1
    text = ''.join(out)
    counts = {ch: text.count(ch) for ch in '(){}[]'}
    balanced = (counts['('] == counts[')'] and counts['{'] == counts['}']
                and counts['['] == counts[']'])
    print(f'  [{"OK " if balanced else "BAD"}] {name:<12} ()={counts["("]}/{counts[")"]} '
          f'{{}}={counts["{"]}/{counts["}"]} []={counts["["]}/{counts["]"]}'
          + ('' if balanced else '   <-- 括号不配对'))
