"""尝试通过公开入口获取 CI 日志（多条路径依次尝试）。"""
import io, sys, json, zipfile, urllib.request, urllib.error
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

REPO = 'xiaohaohan666/project-1'
API = 'https://api.github.com/repos/' + REPO

def get(path):
    req = urllib.request.Request(API + path,
                                 headers={'User-Agent': 'probe/1.0',
                                          'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read() or b'{}')

runs = [r for r in get('/actions/runs?per_page=15')['workflow_runs'] if r['name'] == 'build']
run = runs[0]
jobs = get(f"/actions/runs/{run['id']}/jobs")['jobs']
fail = [j for j in jobs if j['conclusion'] == 'failure']
print(f"build #{run['run_number']} 失败作业: {[j['name'] for j in fail]}")

urls = [
    f"https://github.com/{REPO}/actions/runs/{run['id']}/logs",
    f"https://api.github.com/repos/{REPO}/actions/runs/{run['id']}/logs",
]
for u in urls:
    print('尝试:', u)
    try:
        req = urllib.request.Request(u, headers={'User-Agent': 'Mozilla/5.0',
                                                'Accept': '*/*'})
        with urllib.request.urlopen(req, timeout=90) as resp:
            data = resp.read()
        print('  得到', len(data), '字节')
        try:
            zf = zipfile.ZipFile(io.BytesIO(data))
            print('  zip 内文件数:', len(zf.namelist()))
            for n in zf.namelist():
                if 'server' in n.lower() or 'Linux' in n or 'linux' in n:
                    txt = zf.read(n).decode('utf-8', 'replace')
                    print('  ===== ', n)
                    for ln in txt.splitlines():
                        if 'error' in ln.lower() or 'warning' in ln.lower():
                            print('   ', ln.strip()[:220])
        except zipfile.BadZipFile:
            print('  非 zip，前 500 字节:')
            print('  ', data[:500])
    except urllib.error.HTTPError as e:
        print('  HTTP', e.code)
    except Exception as e:
        print('  失败', type(e).__name__, e)
