"""死代码审计：找出定义但从未被调用的函数、以及不再使用的文件。

用 C 语言的简单启发式：把每个源文件里定义的函数名收集起来，
再在整个代码库中搜索该名字的出现次数。只出现一次（即定义处）
就说明没有被调用（除非是被 main 之外的外部入口引用）。

前端同理：收集 web/js 里定义的顶层函数与 api.js 的方法名，
检查是否在别处被引用。

用法：python tools/audit_deadcode.py
"""
import io
import os
import re
import sys
from collections import defaultdict

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

C_FILES = ['src/campus.h', 'src/campus.c', 'src/server.c', 'main.c',
           'src/launcher.c']
JS_FILES = ['web/js/api.js', 'web/js/ui.js', 'web/js/app.js', 'web/js/pwa.js',
            'web/sw.js']
HTML_FILE = 'web/index.html'


def read(p):
    with open(os.path.join(ROOT, p), encoding='utf-8', errors='replace') as f:
        return f.read()


c_src = {f: read(f) for f in C_FILES}
js_src = {f: read(f) for f in JS_FILES}
html = read(HTML_FILE)

# ============================================================
#                          C 部分
# ============================================================
print('=' * 74)
print('死代码审计')
print('=' * 74)
print('\n【C 语言】')

# 收集函数定义：返回类型 + 名字 + ( 。排除控制语句
CTRL = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'else', 'do',
        'defined', 'catch'}
func_defs = {}
for f, src in c_src.items():
    # 去掉行注释与字符串里的干扰（粗略）
    clean = re.sub(r'//[^\n]*', '', src)
    clean = re.sub(r'/\*.*?\*/', '', clean, flags=re.S)
    for m in re.finditer(
            r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;{)]*\)\s*\{',
            clean, re.M):
        name = m.group(1)
        if name in CTRL:
            continue
        line = clean[:m.start()].count('\n') + 1
        func_defs.setdefault(name, (f, line))

# 统计每个函数名在全部源码里的出现次数
all_c = '\n'.join(c_src.values())
dead_c = []
for name, (f, line) in sorted(func_defs.items()):
    # 排除 main / WinMain / 回调（WndProc 由系统调用）
    if name in ('main', 'WinMain', 'WndProc'):
        continue
    n = len(re.findall(r'\b' + re.escape(name) + r'\b', all_c))
    if n <= 1:
        dead_c.append((name, f, line))

if dead_c:
    print('  定义但未被调用的函数（%d 个）：' % len(dead_c))
    for name, f, line in dead_c:
        print('    %-32s %s:%d' % (name, f, line))
else:
    print('  没有发现未被调用的函数')

# 头文件里声明但从未定义的（可能是残留声明）
print('\n  头文件声明与实现是否配平：')
hdr_funcs = set(re.findall(r'^[A-Za-z_][\w \t\*]*?\b(campus_\w+)\s*\(', read('src/campus.h'), re.M))
impl_funcs = set(re.findall(r'^[A-Za-z_][\w \t\*]*?\b(campus_\w+)\s*\([^;{)]*\)\s*\{',
                            read('src/campus.c'), re.M))
missing = sorted(hdr_funcs - impl_funcs)
extra = sorted(impl_funcs - hdr_funcs)
print('    头文件声明 %d 个，实现 %d 个' % (len(hdr_funcs), len(impl_funcs)))
if missing:
    print('    [!] 声明了但没有实现: %s' % missing)
if extra:
    print('    [i] 实现了但未在头文件声明（可能是内部函数）: %s' % extra)

# ============================================================
#                        前端部分
# ============================================================
print('\n【前端 JavaScript】')
all_js = '\n'.join(js_src.values()) + html

# api.js 暴露的方法名
api_src = js_src['web/js/api.js']
api_methods = re.findall(r'^\s{4}(\w+)\s*[:(]', api_src, re.M)
unused_api = []
for m in sorted(set(api_methods)):
    if m in ('get', 'post', 'req', 'setAuth', 'setPayPwd', 'setApiBase'):
        continue
    # 在 api.js 之外查找 API.xxx
    used = len(re.findall(r'API\.' + re.escape(m) + r'\b', all_js))
    if used == 0:
        unused_api.append(m)
print('  api.js 暴露方法 %d 个；未被调用的：%s'
      % (len(set(api_methods)), unused_api if unused_api else '无'))

# ui.js 的导出
ui_src = js_src['web/js/ui.js']
ui_names = re.findall(r'^\s{4}(\w+)\s*[:(]', ui_src, re.M)
unused_ui = []
for m in sorted(set(ui_names)):
    if m in ('el', 'els'):
        continue
    used = len(re.findall(r'\bUI\.' + re.escape(m) + r'\b', all_js))
    if used == 0:
        unused_ui.append(m)
print('  ui.js 暴露方法 %d 个；未被调用的：%s'
      % (len(set(ui_names)), unused_ui if unused_ui else '无'))

# app.js 顶层函数
app_src = js_src['web/js/app.js']
app_funcs = re.findall(r'^\s{2}function\s+(\w+)\s*\(', app_src, re.M)
dead_js = []
for m in sorted(set(app_funcs)):
    n = len(re.findall(r'\b' + re.escape(m) + r'\b', all_js))
    if n <= 1:
        dead_js.append(m)
print('  app.js 顶层函数 %d 个；未被调用的：%s'
      % (len(set(app_funcs)), dead_js if dead_js else '无'))

# ============================================================
#                      文件级
# ============================================================
print('\n【文件级】')
# 找出仓库里存在但从构建与文档中都未引用的文件
import subprocess
try:
    out = subprocess.run(['git', 'ls-files'], cwd=ROOT, capture_output=True,
                         text=True, encoding='utf-8').stdout
    tracked = [x for x in out.splitlines() if x.strip()]
except Exception:
    tracked = []

readme = read('README.md')
# 构建脚本里出现的文件名
build_refs = read('.github/workflows/build.yml')
suspicious = []
for f in tracked:
    base = os.path.basename(f)
    if f.startswith(('.github/', '.vscode/', 'web/')):
        continue
    if base in ('README.md', '.gitignore', '.gitattributes', 'Dockerfile',
                'render.yaml'):
        continue
    # 在 README、CI、Dockerfile 里是否被提到
    refs = (base in readme) + (base in build_refs)
    if refs == 0 and not f.endswith(('.c', '.h')):
        suspicious.append(f)
print('  未被文档/CI 引用的文件：')
if suspicious:
    for f in suspicious:
        print('    %s' % f)
else:
    print('    无')
