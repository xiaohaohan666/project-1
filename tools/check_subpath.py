"""验证子路径部署（模拟 GitHub Pages 的 /project-1/）是否可用。"""
import io
import sys
import json
import urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
BASE = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8093/project-1'
print('子路径地址:', BASE)
print('=' * 66)


def get(path, timeout=25):
    try:
        req = urllib.request.Request(BASE + path, headers={'User-Agent': 'probe'})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.headers.get('Content-Type', ''), r.read()
    except Exception as e:
        return None, None, str(e)[:70].encode()


ok = True
print('【1】子路径下的静态资源')
# 目录请求由 Pages 自动回落到 index.html；本地 C 服务不做目录索引，
# 故这里以 /index.html 为准，目录请求单独说明
for p in ['/index.html', '/manifest.webmanifest', '/sw.js', '/404.html',
          '/css/app.css', '/js/api.js', '/js/ui.js', '/js/app.js', '/js/pwa.js',
          '/icons/icon-192.png', '/icons/icon-512.png',
          '/icons/maskable-512.png', '/icons/apple-touch-icon.png']:
    st, ct, body = get(p)
    good = st == 200 and len(body) > 0
    if not good:
        ok = False
    print('  [%s] %-28s %s  %6d 字节  %s' % (
        'OK ' if good else 'FAIL', p, st, len(body), ct))

print()
print('【2】清单与 Service Worker 内容')
st, ct, body = get('/manifest.webmanifest')
m = json.loads(body)
print('  manifest  start_url=%s scope=%s icons=%d shortcuts=%d' % (
    m['start_url'], m['scope'], len(m['icons']), len(m.get('shortcuts', []))))
print('  manifest MIME =', ct)
st, ct, body = get('/sw.js')
sw = body.decode('utf-8', 'replace')
print('  sw.js 已去掉硬编码根路径:', "'/index.html'" not in sw)
print('  sw.js 按作用域推导:', 'const BASE' in sw and 'const SCOPE' in sw)

print()
print('【3】首页引用的资源是否都能取到（模拟浏览器加载）')
import re
st, ct, body = get('/index.html')
html = body.decode('utf-8', 'replace')
refs = [r for r in re.findall(r'(?:src|href)="([^"]+)"', html)
        if not r.startswith(('data:', 'http', '#'))]
print('  首页共引用 %d 个资源' % len(refs))
for ref in refs:
    p = '/' + ref.lstrip('./') if not ref.startswith('/') else ref
    st, ct, b = get(p)
    mark = 'OK ' if st == 200 else 'FAIL'
    if st != 200:
        ok = False
    print('  [%s] %-24s -> %s' % (mark, ref, st))

print()
print('=' * 66)
print('子路径部署验证:', '全部通过' if ok else '存在问题')
sys.exit(0 if ok else 1)
