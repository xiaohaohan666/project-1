"""校验大样本下卡片解析是否正确（对照 cards.dat 原始字段）。"""
import io
import sys
import json
import urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
BASE = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8091'
DATA = sys.argv[2] if len(sys.argv) > 2 else 'big/data'


def login(cid):
    body = json.dumps({'id': cid, 'pwd': '123456'}).encode()
    req = urllib.request.Request(BASE + '/api/login', data=body,
                                 headers={'Content-Type': 'application/json'},
                                 method='POST')
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read()).get('card')


print('=' * 70)
print('卡片解析校验：把服务返回的字段与 cards.dat 原始行逐项对比')
print('=' * 70)

lines = open(DATA + '/cards.dat', encoding='utf-8').read().splitlines()
bad = 0
checked = 0
for line in lines[:30]:                 # 抽查前 30 张
    f = line.split('|')
    cid = f[0]
    c = login(cid)
    if not c:
        print('  %s 登录失败' % cid)
        bad += 1
        continue
    # 期望值来自原始文件
    exp = {
        'id': f[0], 'name': f[1], 'number': f[2],
        'recharge': float(f[5]), 'remain': float(f[6]),
        'limitDay': float(f[11]), 'usedDay': float(f[12]),
        'limitOnce': float(f[14]), 'expire': f[15],
    }
    errs = []
    for k, v in exp.items():
        got = c.get(k)
        if isinstance(v, float):
            if abs(float(got) - v) > 0.001:
                errs.append('%s 期望 %.2f 实得 %s' % (k, v, got))
        elif str(got) != str(v):
            errs.append('%s 期望 [%s] 实得 [%s]' % (k, v, got))
    checked += 1
    if errs:
        bad += 1
        print('  [FAIL] %s: %s' % (cid, '; '.join(errs)))
    else:
        print('  [OK ] %s %s 余额 %.2f 限额 %.2f/%.2f 有效期 %s' % (
            cid, c['name'], c['remain'], c['limitOnce'], c['limitDay'], c['expire']))

print('=' * 70)
print('抽查 %d 张，字段错乱 %d 张' % (checked, bad))
sys.exit(1 if bad else 0)
