"""下载失败的 CI 作业日志，提取编译错误行。"""
import io, sys, json, urllib.request, urllib.error
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

API = 'https://api.github.com/repos/xiaohaohan666/project-1'

def raw(url, accept='application/vnd.github+json'):
    req = urllib.request.Request(url, headers={'User-Agent': 'probe/1.0', 'Accept': accept})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()

runs = json.loads(raw(API + '/actions/runs?per_page=1'))['workflow_runs']
run = runs[0]
print('运行:', run['name'], '#%d' % run['run_number'], run['html_url'])
jobs = json.loads(raw(f"{API}/actions/runs/{run['id']}/jobs"))['jobs']
for j in jobs:
    if j['conclusion'] != 'failure':
        continue
    print('=' * 60)
    print('失败作业:', j['name'])
    log = raw(f"{API}/actions/jobs/{j['id']}/logs", 'application/vnd.github.raw').decode('utf-8', 'replace')
    lines = log.splitlines()
    # 打印含 error / warning / 编译命令的上下文
    for i, ln in enumerate(lines):
        if ('error:' in ln or 'Error' in ln or 'undefined' in ln
                or 'gcc ' in ln or 'warning:' in ln):
            print('  ', ln.strip()[:200])
    print('  ---- 日志末尾 30 行 ----')
    for ln in lines[-30:]:
        print('  ', ln.rstrip()[:200])
