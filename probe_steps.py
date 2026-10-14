"""等待 probe 运行完成，打印每个步骤的结论以定位 Linux 编译失败位置。"""
import io, sys, json, time, urllib.request
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

API = 'https://api.github.com/repos/xiaohaohan666/project-1'

def get(path):
    req = urllib.request.Request(API + path,
                                 headers={'User-Agent': 'probe/1.0',
                                          'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read() or b'{}')

run = None
for _ in range(72):
    for r in get('/actions/runs?per_page=10')['workflow_runs']:
        if r['name'] == 'probe':
            run = r
            break
    if run and run['status'] == 'completed':
        break
    time.sleep(10)

print(f"probe #{run['run_number']}  {run['status']}/{run['conclusion']}")
print(' ', run['html_url'])
for j in get(f"/actions/runs/{run['id']}/jobs")['jobs']:
    print('作业:', j['name'], '->', j['conclusion'])
    for s in j.get('steps', []):
        print(f"   [{s['conclusion']:<8}] {s['number']}. {s['name']}")
