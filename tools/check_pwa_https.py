"""通过公网 HTTPS 地址验证 PWA 资源（Service Worker 需要安全上下文）。"""
import io
import re
import os
import sys
import json
import urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')


def live_url():
    """从隧道日志里找出当前存活的地址。"""
    pat = re.compile(r'https://[a-z0-9]+\.lhr\.life')
    urls = []
    for p in [os.path.join('deploy', 'lhr.log'), 'lhr.log']:
        if os.path.exists(p):
            with open(p, encoding='utf-8', errors='replace') as f:
                for u in pat.findall(f.read()):
                    if u not in urls:
                        urls.append(u)
    for u in reversed(urls[-8:]):
        try:
            urllib.request.urlopen(u + '/api/health', timeout=10)
            return u
        except Exception:
            continue
    return None


base = sys.argv[1] if len(sys.argv) > 1 else live_url()
if not base:
    print('没有存活的隧道地址')
    sys.exit(1)
print('公网地址:', base)
print('=' * 62)

paths = ['/', '/manifest.webmanifest', '/sw.js', '/js/pwa.js',
         '/icons/icon-192.png', '/icons/icon-512.png',
         '/icons/maskable-512.png', '/icons/apple-touch-icon.png',
         '/css/app.css']

for p in paths:
    try:
        r = urllib.request.urlopen(base + p, timeout=30)
        body = r.read()
        ct = r.headers.get('Content-Type', '')
        print('  {:<28} {} {:>6} 字节  {}'.format(p, r.status, len(body), ct))
    except Exception as e:
        print('  {:<28} 失败 {}'.format(p, type(e).__name__))

print('=' * 62)
m = json.loads(urllib.request.urlopen(base + '/manifest.webmanifest', timeout=30).read())
print('  manifest: display={} scope={} 图标 {} 个'.format(
    m['display'], m['scope'], len(m['icons'])))
sw = urllib.request.urlopen(base + '/sw.js', timeout=30)
print('  Service-Worker-Allowed =', sw.headers.get('Service-Worker-Allowed'))
html = urllib.request.urlopen(base + '/', timeout=30).read().decode('utf-8', 'replace')
print('  首页含 manifest 引用:', 'manifest.webmanifest' in html)
print('  首页含 apple-touch-icon:', 'apple-touch-icon.png' in html)
print('  首页含 pwa.js:', 'js/pwa.js' in html)
print('  首页含安装按钮:', 'btnInstall' in html)
