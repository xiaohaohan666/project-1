"""读取 probe-report 上的冒烟测试细节。"""
import io, sys, json, time, urllib.request, urllib.error
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

API = 'https://api.github.com/repos/xiaohaohan666/project-1'

def get(path):
    req = urllib.request.Request(API + path,
                                 headers={'User-Agent': 'probe/1.0',
                                          'Accept': 'application/vnd.github+json'})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        return e.code, None

for i in range(60):
    st, d = get('/commits?sha=probe-report&per_page=1')
    if st == 200 and d:
        print('=' * 62)
        print(d[0]['commit']['message'][:8000])
        print('=' * 62)
        break
    time.sleep(10)
else:
    print('probe-report 未更新')
    st, d = get('/actions/runs?per_page=5')
    for r in (d or {}).get('workflow_runs', []):
        if r['name'] == 'probe':
            print(f"  probe #{r['run_number']} {r['status']}/{r['conclusion']}")
