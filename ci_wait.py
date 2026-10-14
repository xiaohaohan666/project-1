"""轮询 CI 工作流运行状态，直到完成或超时。"""
import io, sys, json, time, urllib.request, urllib.error
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

API = 'https://api.github.com/repos/xiaohaohan666/project-1'

def get(path):
    req = urllib.request.Request(API + path,
                                 headers={'User-Agent': 'probe/1.0',
                                          'Accept': 'application/vnd.github+json'})
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        return e.code, None

deadline = time.time() + 420          # 最多等 7 分钟
last = None
while time.time() < deadline:
    st, d = get('/actions/runs?per_page=5')
    if st != 200 or not d.get('workflow_runs'):
        print('  查询失败或暂无运行, HTTP', st)
        time.sleep(10)
        continue
    runs = d['workflow_runs']
    line = ' | '.join(f"#{r['run_number']} {r['status']}/{r['conclusion']}" for r in runs[:3])
    if line != last:
        print('  ', line)
        last = line
    if all(r['status'] == 'completed' for r in runs[:1]):
        break
    time.sleep(10)

print('=== 最终结果 ===')
st, d = get('/actions/runs?per_page=5')
for r in (d or {}).get('workflow_runs', [])[:3]:
    print(f"  {r['name']} #{r['run_number']}  状态={r['status']}  结论={r['conclusion']}")
    print(f"     {r['html_url']}")
    st2, jobs = get(f"/actions/runs/{r['id']}/jobs")
    if st2 == 200:
        for j in jobs.get('jobs', []):
            print(f"     - {j['name']}: {j['status']} / {j['conclusion']}")
            for s in j.get('steps', []):
                if s.get('conclusion') not in (None, 'success', 'skipped'):
                    print(f"         失败步骤: {s['name']} ({s['conclusion']})")
