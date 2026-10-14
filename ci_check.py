"""等待 build 工作流完成并报告结果。"""
import io, sys, json, time, urllib.request
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

API = 'https://api.github.com/repos/xiaohaohan666/project-1'

def get(path):
    req = urllib.request.Request(API + path,
                                 headers={'User-Agent': 'probe/1.0',
                                          'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read() or b'{}')

run = None
for _ in range(60):
    runs = [r for r in get('/actions/runs?per_page=15')['workflow_runs'] if r['name'] == 'build']
    if runs:
        run = runs[0]
        if run['status'] == 'completed':
            break
    time.sleep(10)

print(f"build #{run['run_number']}  {run['status']}/{run['conclusion']}  {run['head_sha'][:7]}")
print(' ', run['html_url'])
for j in get(f"/actions/runs/{run['id']}/jobs")['jobs']:
    print(f"  作业: {j['name']} -> {j['conclusion']}")
    for s in j.get('steps', []):
        if s['conclusion'] not in ('success',):
            print(f"     [{s['conclusion']}] {s['name']}")
    if j['conclusion'] == 'success':
        print('     （全部步骤通过）')
