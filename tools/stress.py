"""大样本压测：容量上限、性能、静默截断、边界条件。"""
import io
import sys
import json
import time
import urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
BASE = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8091'


def call(path, method='GET', body=None, hdr=None, timeout=120):
    h = dict(hdr or {})
    data = json.dumps(body, ensure_ascii=False).encode() if body is not None else None
    if data:
        h['Content-Type'] = 'application/json'
    req = urllib.request.Request(BASE + path, data=data, headers=h, method=method)
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
        return time.time() - t0, len(raw), json.loads(raw or b'{}')
    except Exception as e:
        return time.time() - t0, 0, {'__error__': '%s: %s' % (type(e).__name__, str(e)[:70])}


A = {'X-Token': 'ADMIN', 'X-Pwd': 'admin123'}
S = {'X-Token': 'C0001', 'X-Pwd': '123456'}

print('压测地址:', BASE)
print('=' * 70)

print('【1】容量与分页')
for label, path, hdr in [
        ('全部卡片', '/api/admin/cards', A),
        ('学生流水(全部)', '/api/ledger?period=all', S),
        ('管理端流水(当天)', '/api/admin/ledger?period=day', A),
        ('门禁记录', '/api/admin/access?id=', A)]:
    dt, size, d = call(path, hdr=hdr)
    if '__error__' in d:
        print('  %-18s 失败 %s' % (label, d['__error__']))
        continue
    total = d.get('total')
    arr = d.get('records') if 'records' in d else d.get('cards', [])
    recs = len(arr)
    note = ''
    if total is not None and recs and recs < total:
        # 服务端分页属于设计行为，不算缺陷；只有返回畸形数据才是问题
        note = '  （分页：本页 %d / 共 %d，hasMore=%s）' % (
            recs, total, d.get('hasMore', d.get('truncated')))
    print('  %-18s %6.2fs  %8d 字节  total=%s 本页=%d%s' % (
        label, dt, size, total, recs, note))

print()
print('【2】报表性能')
for label, path in [('当天全站报表', '/api/admin/report'),
                    ('当天机房收入', '/api/admin/income')]:
    dt, size, d = call(path, hdr=A)
    if '__error__' in d:
        print('  %-18s 失败 %s' % (label, d['__error__']))
    else:
        print('  %-18s %6.2fs  %8d 字节  消费=%s 笔数=%s 充值=%s' % (
            label, dt, size, d.get('consumeTotal'), d.get('consumeTimes'),
            d.get('rechargeTotal')))

print()
print('【3】单卡查询性能（40 张卡有上机记录）')
for cid in ['C0001', 'C0020', 'C0040']:
    h = {'X-Token': 'ADMIN', 'X-Pwd': 'admin123'}
    dt, size, d = call('/api/admin/records?id=' + cid, hdr=h)
    print('  %s 上机记录  %6.2fs  total=%s' % (cid, dt, d.get('total')))

print()
print('【4】消费扣款与限额（卡片状态正确性）')
_, _, d = call('/api/login', 'POST', {'id': 'C0001', 'pwd': '123456'})
if d.get('ok'):
    card = d['card']
    print('  C0001 余额=%.2f 单笔限额=%.2f 日限额=%.2f 今日已用=%.2f 有效期=%s' % (
        card['remain'], card['limitOnce'], card['limitDay'], card['usedDay'], card['expire']))
    # 用正确的交易密码扣款
    pw = card['number'][-6:]
    dt, size, r = call('/api/pay', 'POST',
                       {'merchant': '食堂', 'amount': 1, 'payPwd': pw}, S)
    print('  用学号后 6 位(%s)扣款 1 元: %s  %.2fs' % (
        pw, r.get('message') or r.get('error'), dt))
else:
    print('  登录失败:', d)

print()
print('【5】边界条件')
tests = [
    ('金额为 0 的消费', 'POST', '/api/pay',
     {'merchant': '食堂', 'amount': 0, 'payPwd': '010001'}, S),
    ('金额为负的消费', 'POST', '/api/pay',
     {'merchant': '食堂', 'amount': -5, 'payPwd': '010001'}, S),
    ('金额为字母', 'POST', '/api/pay',
     {'merchant': '食堂', 'amount': 'abc', 'payPwd': '010001'}, S),
    ('商户为空', 'POST', '/api/pay',
     {'merchant': '', 'amount': 1, 'payPwd': '010001'}, S),
    ('不存在的卡登录', 'POST', '/api/login', {'id': 'C9999', 'pwd': 'x'}, None),
    ('重复卡号注册', 'POST', '/api/register',
     {'id': 'C0001', 'name': '重复', 'number': 'B1', 'pwd': '123456'}, None),
    ('超长卡号注册', 'POST', '/api/register',
     {'id': 'C123456789012', 'name': '太长', 'number': 'B2', 'pwd': '123456'}, None),
    ('不存在的卡查流水', 'GET', '/api/admin/records?id=C9999', None, A),
]
for label, method, path, body, hdr in tests:
    dt, size, d = call(path, method, body, hdr)
    msg = d.get('error') or ('ok' if d.get('ok') else str(d)[:50])
    print('  %-20s %-6.2fs -> %s' % (label, dt, msg))
