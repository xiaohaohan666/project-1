"""一卡通最终验收：走完整业务链路，覆盖所有新增功能。"""
import io
import sys
import json
import urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
BASE = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8090'
print('验收地址:', BASE)
print('=' * 64)


def call(path, method='GET', body=None, hdr=None):
    h = dict(hdr or {})
    data = json.dumps(body, ensure_ascii=False).encode() if body is not None else None
    if data:
        h['Content-Type'] = 'application/json'
    req = urllib.request.Request(BASE + path, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read() or b'{}')
    except Exception as e:
        return None, {'error': str(e)[:80]}


def check(label, cond, extra=''):
    print('  [%s] %s %s' % ('OK ' if cond else 'FAIL', label, extra))
    return cond


fails = []
A = {'X-Token': 'ADMIN', 'X-Pwd': 'admin123'}
S = {'X-Token': 'C001', 'X-Pwd': '123456'}

st, d = call('/api/health')
fails.append(check('健康检查', st == 200 and d.get('ok'), '卡片 %s 张' % d.get('cards')))

st, d = call('/api/merchants')
ok = st == 200 and len(d.get('merchants', [])) == 7
fails.append(check('商户列表（7 个消费点）', ok, str(d.get('merchants'))))

st, d = call('/api/register', 'POST',
             {'id': 'C400', 'name': '李思思', 'number': 'B17010500', 'pwd': 'abcd12'})
fails.append(check('新卡注册', st == 200 and d.get('ok'),
                   '交易密码默认取学号后 6 位'))

st, d = call('/api/admin/recharge', 'POST', {'id': 'C001', 'amount': 100}, A)
fails.append(check('管理员充值 100 元', st == 200 and d.get('ok'),
                   '余额 %.2f' % d.get('card', {}).get('remain', 0)))

st, d = call('/api/admin/pay', 'POST',
             {'id': 'C001', 'merchant': '食堂', 'amount': 15, 'payPwd': '010101'}, A)
fails.append(check('商户终端代扣（食堂 15 元）', st == 200 and d.get('ok'),
                   '余额 %.2f' % d.get('card', {}).get('remain', 0)))

st, d = call('/api/admin/pay', 'POST',
             {'id': 'C001', 'merchant': '超市', 'amount': 5, 'payPwd': '999999'}, A)
fails.append(check('交易密码错误被拦', d.get('error') == '交易密码错误'))

st, d = call('/api/admin/pay', 'POST',
             {'id': 'C001', 'merchant': '超市', 'amount': 30, 'payPwd': '010101'}, A)
fails.append(check('超单笔限额被拦', '限额' in str(d.get('error')), str(d.get('error'))))

st, d = call('/api/admin/limits', 'POST',
             {'id': 'C001', 'limitOnce': 50, 'limitDay': 100}, A)
fails.append(check('调整限额为 50/100', st == 200 and d.get('ok')))

st, d = call('/api/admin/pay', 'POST',
             {'id': 'C001', 'merchant': '文印店', 'amount': 30, 'payPwd': '010101'}, A)
fails.append(check('限额提高后 30 元通过', st == 200 and d.get('ok')))

st, d = call('/api/ledger?period=day', hdr=S)
fails.append(check('学生查交易流水', st == 200 and d.get('total', 0) >= 3,
                   '共 %s 条' % d.get('total')))

st, d = call('/api/consume?period=day', hdr=S)
fails.append(check('学生消费统计', st == 200 and d.get('amount', 0) > 0,
                   '%.2f 元 / %s 笔' % (d.get('amount', 0), d.get('count'))))

st, d = call('/api/access', 'POST', {'place': '图书馆', 'dir': 0, 'kind': 1}, S)
fails.append(check('门禁考勤签到', st == 200 and d.get('ok'), d.get('message', '')))

st, d = call('/api/admin/report', hdr=A)
m = d.get('merchants', [])
fails.append(check('当天全站报表', st == 200 and d.get('consumeTotal', 0) > 0,
                   '消费 %.2f 元 / %s 笔 / 充值 %.2f' % (
                       d.get('consumeTotal', 0), d.get('consumeTimes'),
                       d.get('rechargeTotal', 0))))
fails.append(check('各消费点分列', len(m) == 7))

st, d = call('/api/admin/lost', 'POST', {'id': 'C002'}, A)
fails.append(check('管理员挂失 C002', st == 200 and d.get('card', {}).get('stateText') == '挂失'))

st, d = call('/api/admin/pay', 'POST',
             {'id': 'C002', 'merchant': '食堂', 'amount': 5, 'payPwd': '010102'}, A)
fails.append(check('挂失卡消费被拦', '挂失' in str(d.get('error')), str(d.get('error'))))

st, d = call('/api/admin/unlost', 'POST', {'id': 'C002'}, A)
fails.append(check('解除挂失', st == 200 and d.get('card', {}).get('stateText') == '正常'))

st, d = call('/api/admin/reissue', 'POST', {'id': 'C002', 'fee': 5}, A)
ok = st == 200 and d.get('ok') and abs(d.get('card', {}).get('remain', 0) - 25) < 0.01
fails.append(check('补办扣工本费 5 元', ok, '余额 %.2f' % d.get('card', {}).get('remain', 0)))

st, d = call('/api/admin/grant', 'POST',
             {'id': 'C003', 'amount': 50, 'type': 4, 'memo': '新生补助'}, A)
fails.append(check('发放补助 50 元', st == 200 and d.get('ok'),
                   '余额 %.2f' % d.get('card', {}).get('remain', 0)))

st, d = call('/api/admin/expire', 'POST', {'id': 'C003', 'date': '2032-12-31'}, A)
fails.append(check('卡片延期', st == 200 and d.get('card', {}).get('expire') == '2032-12-31'))

st, d = call('/api/admin/resetpay', 'POST', {'id': 'C003'}, A)
fails.append(check('重置交易密码', st == 200 and d.get('ok')))

st, d = call('/api/admin/access?id=', hdr=A)
fails.append(check('管理端查门禁记录', st == 200 and d.get('total', 0) >= 1,
                   '共 %s 条' % d.get('total')))

st, d = call('/api/admin/cards', hdr=A)
fails.append(check('全部卡片列表', st == 200 and d.get('total', 0) >= 4,
                   '%s 张，余额合计 %.2f' % (d.get('total'), d.get('remainTotal', 0))))

print('=' * 64)
print('通过 %d / %d 项' % (len(fails) - sum(1 for f in fails if not f), len(fails)))
if any(not f for f in fails):
    print('存在失败项')
    sys.exit(1)
print('全部通过')
