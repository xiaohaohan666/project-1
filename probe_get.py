"""等待 probe 工作流完成，下载构建产物并打印 Linux 编译报错。"""
import io, sys, json, time, zipfile, urllib.request, urllib.error
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

API = 'https://api.github.com/repos/xiaohaohan666/project-1'

def get(path, raw=False):
    req = urllib.request.Request(API + path,
                                 headers={'User-Agent': 'probe/1.0',
                                          'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req, timeout=60) as r:
        data = r.read()
        return data if raw else json.loads(data or b'{}')

print('等待 probe 工作流...')
run = None
for _ in range(60):
    runs = get('/actions/runs?per_page=10')['workflow_runs']
    for r in runs:
        if r['name'] == 'probe':
            run = r
            break
    if run and run['status'] == 'completed':
        break
    time.sleep(10)

if not run:
    print('未找到 probe 运行')
    sys.exit(1)
print(f"probe #{run['run_number']}  {run['status']}/{run['conclusion']}")
print(' ', run['html_url'])

arts = get(f"/actions/runs/{run['id']}/artifacts").get('artifacts', [])
if not arts:
    print('没有产物（工作流可能在编译步骤前就失败了）')
    sys.exit(1)

a = arts[0]
print('产物:', a['name'], a['size_in_bytes'], '字节')
data = get(f"/actions/artifacts/{a['id']}/zip", raw=True)
open('probe.zip', 'wb').write(data)
with zipfile.ZipFile(io.BytesIO(data)) as z:
    print('包内文件:', z.namelist()[:10])
    if 'out/build.log' in z.namelist():
        log = z.read('out/build.log').decode('utf-8', 'replace')
        print('=' * 62)
        print('Linux gcc 完整报错：')
        print('=' * 62)
        print(log[:4000])
