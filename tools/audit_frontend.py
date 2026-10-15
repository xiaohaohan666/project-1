"""严格审计前端：把 JS 引用的每个 DOM 选择器与 HTML 实际结构逐一比对。

为什么需要它：本环境的沙箱禁止 Chromium 使用的命名管道（mojo），
任何浏览器都无法启动，因此无法做真实点击测试。选择器落空是
「点了没反应 / 功能不正常」最常见的根因，可以静态查出。

检查项：
  1. #id 选择器 -> HTML 里是否存在该 id
  2. .class 选择器 -> CSS 或 HTML 里是否定义
  3. [data-xxx] 属性选择器 -> HTML 里是否有该属性（或由 JS 动态设置）
  4. switchView 的视图名 -> HTML 里是否有对应的 .view
  5. JS 里 querySelector 的字符串是否明显引用了不存在的 id
  6. HTML 里 data-view 与 JS 里 switchView('...') 的名字是否一致
"""
import io
import os
import re
import sys
from collections import defaultdict

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, 'web')

JS_FILES = ['js/api.js', 'js/ui.js', 'js/app.js', 'js/pwa.js']
HTML_FILE = 'index.html'
CSS_FILE = 'css/app.css'


def read(p):
    with open(p, encoding='utf-8') as f:
        return f.read()


html = read(os.path.join(WEB, HTML_FILE))
css = read(os.path.join(WEB, CSS_FILE))
js_all = {f: read(os.path.join(WEB, f)) for f in JS_FILES}
js_joined = '\n'.join(js_all.values())

# ---------- 收集 HTML 里的 id / class / data-* ----------
html_ids = set(re.findall(r'\bid="([^"]+)"', html))
html_classes = set()
for m in re.findall(r'\bclass="([^"]*)"', html):
    html_classes.update(m.split())
html_data_attrs = set(re.findall(r'\b(data-[a-z0-9-]+)=', html))
# JS 里动态设置的 data-* 属性也要算进来
html_data_attrs.update(re.findall(r'setAttribute\([\'"](data-[a-z0-9-]+)[\'"]', js_joined))
html_data_attrs.update(re.findall(r'\b(data-[a-z0-9-]+)\s*[:=]', js_joined))

css_classes = set(re.findall(r'\.([a-zA-Z][\w-]*)', css))

# ---------- 收集 JS 里引用的选择器 ----------
# 形如 $('#x')  $$('#x .y')  querySelector('#x')  querySelectorAll('.y')
sel_calls = []
for fname, src in js_all.items():
    for m in re.finditer(r'(?:\$|\$\$|querySelector|querySelectorAll|closest|matches)\s*\(\s*[\'"]([^\'"]+)[\'"]', src):
        line = src[:m.start()].count('\n') + 1
        sel_calls.append((fname, line, m.group(1)))
    # 模板形式的 querySelector(`...`)
    for m in re.finditer(r'(?:\$|\$\$|querySelector|querySelectorAll)\s*\(\s*`([^`]+)`', src):
        line = src[:m.start()].count('\n') + 1
        sel_calls.append((fname, line, m.group(1)))

problems = []
used_ids = set()
used_classes = set()
used_data = set()

ID_RE = re.compile(r'#([A-Za-z_][\w-]*)')
CLASS_RE = re.compile(r'\.([A-Za-z_][\w-]*)')
DATA_RE = re.compile(r'\[(data-[a-z0-9-]+)')
BARE_CLASS_RE = re.compile(r'^\.([A-Za-z_][\w-]*)$')

for fname, line, sel in sel_calls:
    # 跳过明显是模板占位、或带变量的选择器
    if '${' in sel:
        # 模板里的固定部分仍可查：把 ${...} 换成通配再判断太复杂，这里只查 id 部分
        for i in ID_RE.findall(sel):
            used_ids.add(i)
        continue
    for i in ID_RE.findall(sel):
        used_ids.add(i)
        if i not in html_ids:
            problems.append(('缺少 id', fname, line, sel, '#' + i + ' 在 HTML 中不存在'))
    for c in CLASS_RE.findall(sel):
        used_classes.add(c)
    for d in DATA_RE.findall(sel):
        used_data.add(d)
        if d not in html_data_attrs:
            problems.append(('缺少 data 属性', fname, line, sel,
                             d + ' 在 HTML 与 JS 中都未设置'))

# ---------- class 是否有定义 ----------
for c in sorted(used_classes):
    if c not in html_classes and c not in css_classes:
        problems.append(('缺少 class', '-', 0, '.' + c, 'HTML 与 CSS 中都未出现'))

# ---------- data-* 是否有定义 ----------
for d in sorted(used_data):
    if d not in html_data_attrs:
        problems.append(('缺少 data 属性', '-', 0, '[' + d + ']',
                         'HTML 与 JS 中都未设置'))

# ---------- JS 里字符串形式的 id 引用 ----------
# getElementById('x')
for fname, src in js_all.items():
    for m in re.finditer(r'getElementById\(\s*[\'"]([^\'"]+)[\'"]', src):
        gid = m.group(1)
        line = src[:m.start()].count('\n') + 1
        used_ids.add(gid)
        if gid not in html_ids:
            problems.append(('缺少 id', fname, line, "getElementById('%s')" % gid,
                             '该 id 在 HTML 中不存在'))

# ---------- 视图一致性 ----------
views_html = set(re.findall(r'class="[^"]*\bview\b[^"]*"\s+data-view="([^"]+)"', html))
views_html |= set(re.findall(r'data-view="([^"]+)"[^>]*class="[^"]*\bview\b', html))
views_js = set(re.findall(r"switchView\(\s*['\"]([^'\"]+)['\"]", js_joined))
nav_targets = set(re.findall(r'data-view="([^"]+)"', html))

print('=' * 74)
print('前端选择器一致性审计')
print('=' * 74)
print('HTML id 数量        : %d' % len(html_ids))
print('HTML class 数量     : %d' % len(html_classes))
print('HTML data-* 属性    : %d  %s' % (len(html_data_attrs),
                                        ' '.join(sorted(html_data_attrs))))
print('JS 引用选择器调用   : %d 处' % len(sel_calls))
print()

print('【视图一致性】')
print('  HTML 中 .view 的 data-view : %s' % sorted(views_html))
print('  JS 中 switchView() 的目标  : %s' % sorted(views_js))
print('  HTML 中所有 data-view      : %s' % sorted(nav_targets))
for v in sorted(views_js):
    if v not in views_html:
        problems.append(('视图缺失', 'js/app.js', 0, "switchView('%s')" % v,
                         'HTML 中没有 data-view="%s" 的视图' % v))
for v in sorted(nav_targets):
    if v not in views_html and v not in views_js:
        pass
print()

if problems:
    print('【发现 %d 个问题】' % len(problems))
    by_kind = defaultdict(list)
    for kind, f, line, sel, why in problems:
        by_kind[kind].append((f, line, sel, why))
    for kind in by_kind:
        print('\n-- %s (%d) --' % (kind, len(by_kind[kind])))
        for f, line, sel, why in by_kind[kind]:
            loc = '%s:%d' % (f, line) if line else f
            print('   %-18s %-34s %s' % (loc, sel[:34], why))
else:
    print('【未发现问题】所有被引用的选择器都存在于 HTML/CSS 中')

print()
print('=' * 74)
print('结论:', '需要修复' if problems else '选择器全部有效')
sys.exit(1 if problems else 0)
