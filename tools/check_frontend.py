"""前端一致性检查：JS 引用的 DOM id / class 是否都存在于 HTML 与 CSS。"""
import io
import os
import re
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web')

html = open(os.path.join(WEB, 'index.html'), encoding='utf-8').read()
css = open(os.path.join(WEB, 'css', 'app.css'), encoding='utf-8').read()
js_files = ['js/api.js', 'js/ui.js', 'js/app.js', 'js/pwa.js']
js = {n: open(os.path.join(WEB, n), encoding='utf-8').read() for n in js_files}

html_ids = set(re.findall(r'id="([\w-]+)"', html))
# JS 里通过 $('#xxx') 或 getElementById 引用的 id
js_ids = set()
for src in js.values():
    js_ids |= set(re.findall(r"\$\('#([\w-]+)'", src))
    js_ids |= set(re.findall(r"getElementById\('([\w-]+)'\)", src))
# 运行时由 JS 动态创建的元素，不在静态 HTML 中，属正常
#   offlineBar  —— pwa.js 离线时创建
#   mCost       —— app.js 计时器每帧重建
#   pf_<字段>   —— ui.js 输入弹窗按字段名生成
DYNAMIC_IDS = {'offlineBar', 'mCost'}
DYNAMIC_PREFIX = ('pf_',)

missing_ids = sorted(
    i for i in js_ids
    if i not in html_ids
    and i not in DYNAMIC_IDS
    and not any(i.startswith(p) for p in DYNAMIC_PREFIX)
)
skipped = sorted(i for i in js_ids if i not in html_ids and i not in missing_ids)
print('=== JS 引用的 DOM id 检查 ===')
print('  HTML 中的 id 数:', len(html_ids))
print('  JS 引用的 id 数:', len(js_ids))
print('  动态创建（正常跳过）:', ', '.join(skipped) if skipped else '无')
if missing_ids:
    for i in missing_ids:
        print('  [MISSING]', i)
else:
    print('  [OK] 静态引用的 id 全部存在')

# class 检查：JS 里 classList.add/remove/toggle 用到的类名
js_classes = set()
for src in js.values():
    js_classes |= set(re.findall(r"classList\.(?:add|remove|toggle)\('([\w-]+)'", src))
    js_classes |= set(re.findall(r"className\s*=\s*'([\w\s-]+)'", src))
classes = set()
for c in js_classes:
    classes |= set(c.split())
# 动态拼接的类名（如 'toast ' + kind）
classes |= {'ok', 'err'}
css_classes = set(re.findall(r'\.([\w-]+)', css))
html_classes = set()
for m in re.findall(r'class="([^"]+)"', html):
    html_classes |= set(m.split())

print('=== JS 使用的 class 检查 ===')
missing_cls = []
for c in sorted(classes):
    in_css = c in css_classes
    in_html = c in html_classes
    if not (in_css or in_html):
        missing_cls.append(c)
    print('  [{}] {:<18} CSS:{}  HTML:{}'.format(
        'OK ' if (in_css or in_html) else 'MISS', c, 'Y' if in_css else '-',
        'Y' if in_html else '-'))

print('=== CSS 变量使用是否都有定义 ===')
defined = set(re.findall(r'(--[\w-]+)\s*:', css))
used = set(re.findall(r'var\((--[\w-]+)', css))
undef = sorted(used - defined)
print('  定义', len(defined), '个，使用', len(used), '个 ->',
      '全部有定义' if not undef else '未定义: ' + ', '.join(undef))

print('=== 汇总 ===')
print('  DOM id:', 'OK' if not missing_ids else '有缺失')
print('  class :', 'OK' if not missing_cls else '缺失 ' + ', '.join(missing_cls))
