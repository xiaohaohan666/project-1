"""等待 probe 运行完成，读取工作流摘要中的 Linux 编译报错。"""
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
for _ in range(60):
    for r in get('/actions/runs?per_page=10')['workflow_runs']:
        if r['name'] == 'probe':
            run = r
            break
    if run and run['status'] == 'completed':
        break
    time.sleep(10)

print(f"probe #{run['run_number']}  {run['status']}/{run['conclusion']}")
print(' ', run['html_url'])
jobs = get(f"/actions/runs/{run['id']}/jobs")['jobs']
for j in jobs:
    print('作业:', j['name'], '->', j['conclusion'], '| id =', j['id'])
# 摘要不需要认证也能读：抓取作业页面并从 HTML 中提取摘要正文
try:
    req2 = urllib.request.Request(
        f"https://github.com/xiaohaohan666/project-1/actions/runs/{run['id']}/job/{jobs[0]['id']}",
        headers={'User-Agent': 'Mozilla/5.0'})
    html = urllib.request.urlopen(req2, timeout=60).read().decode('utf-8', 'replace')
    import re, html as H
    print('页面长度:', len(html))
    for kw in ['gcc', 'error', 'probe:', 'exit code']:
        idx = html.find(kw)
        print(f'  关键词 {kw!r} 位置: {idx}')
    # 找出所有 pre/代码块
    blocks = re.findall(r'<pre[^>]*>(.*?)</pre>', html, re.S)
    print('代码块数量:', len(blocks))
    for b in blocks[:3]:
        txt = H.unescape(re.sub(r'<[^>]+>', '', b))
        print('-' * 62)
        print(txt[:4000])
except Exception as e:
    print('读取摘要失败:', type(e).__name__, e)
