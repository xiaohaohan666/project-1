"""等待 Pages 部署完成，并验证站点可访问。"""
import io
import sys
import json
import time
import urllib.request
import urllib.error

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
API = 'https://api.github.com/repos/xiaohaohan666/project-1'
SITE = 'https://xiaohaohan666.github.io/project-1/'


def api(path):
    req = urllib.request.Request(API + path,
                                 headers={'User-Agent': 'probe',
                                          'Accept': 'application/vnd.github+json'})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read() or b'{}')
    except Exception as e:
        return {'__err__': f'{type(e).__name__}: {e}'}


def web(url, timeout=30):
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.headers.get('Content-Type', ''), r.read()
    except urllib.error.HTTPError as e:
        return e.code, '', b''
    except Exception as e:
        return None, str(e)[:60], b''


print('等待 pages 工作流...')
run = None
for _ in range(60):
    runs = api('/actions/runs?per_page=15').get('workflow_runs', [])
    for r in runs:
        if r['name'] == 'pages':
            run = r
            break
    if run and run['status'] == 'completed':
        break
    time.sleep(10)

if not run:
    print('未找到 pages 运行')
else:
    print('pages #%d  %s/%s' % (run['run_number'], run['status'], run['conclusion']))
    print(' ', run['html_url'])
    for j in api("/actions/runs/%s/jobs" % run['id']).get('jobs', []):
        print('  作业 %s -> %s' % (j['name'], j['conclusion']))
        for s in j.get('steps', []):
            if s.get('conclusion') not in ('success',):
                print('     [%s] %s' % (s.get('conclusion'), s.get('name')))

print()
print('Pages 访问状态:')
d = api('/pages')
if isinstance(d, dict) and d.get('html_url'):
    print('  html_url =', d['html_url'], ' status =', d.get('status'))
else:
    print('  /pages 查询:', d)

print()
print('站点连通性:')
for url in [SITE, SITE + 'index.html', SITE + 'manifest.webmanifest', SITE + 'sw.js']:
    st, ct, body = web(url)
    print('  %-52s %s  %6d 字节  %s' % (url, st, len(body), ct))
