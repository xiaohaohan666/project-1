"""通过公网隧道地址验证服务是否真的可从外网访问。"""
import io, sys, json, urllib.request
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

BASE = sys.argv[1] if len(sys.argv) > 1 else 'https://a96d6e6b16e9c5.lhr.life'

def req(path, method='GET', body=None, headers=None):
    h = dict(headers or {})
    data = None
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode()
        h['Content-Type'] = 'application/json'
    r = urllib.request.Request(BASE + path, data=data, headers=h, method=method)
    with urllib.request.urlopen(r, timeout=40) as resp:
        raw = resp.read()
        try:
            return resp.status, json.loads(raw)
        except Exception:
            return resp.status, raw

print('公网地址:', BASE)
print('=' * 60)
st, h = req('/api/health')
print(f'1) 健康检查  HTTP {st}  {h}')
st, pg = req('/')
print(f'2) 首页      HTTP {st}  {len(pg)} 字节  含"模拟校园卡": {"模拟校园卡".encode() in pg}')
for p in ['/css/app.css', '/js/api.js', '/js/ui.js', '/js/app.js']:
    st, b = req(p)
    print(f'3) 资源 {p:<14} HTTP {st}  {len(b)} 字节')
st, lg = req('/api/login', 'POST', {'id': 'C001', 'pwd': '123456'})
print(f'4) 学生登录  HTTP {st}  卡号={lg.get("token")}  姓名={lg["card"]["name"]}  余额={lg["card"]["remain"]}')
S = {'X-Token': 'C001', 'X-Pwd': '123456'}
st, on = req('/api/machine', 'POST', {'action': 'online', 'place': 'A102 机房'}, S)
print(f'5) 上机登记  HTTP {st}  {on.get("message")}  地点={on.get("card", {}).get("address")}  err={on.get("error")}')
st, off = req('/api/machine', 'POST', {'action': 'offline'}, S)
print(f'6) 下机结账  HTTP {st}  {off.get("message")}  费用={off["cost"]}  余额={off["card"]["remain"]}')
st, stt = req('/api/stats?period=all', headers=S)
print(f'7) 上机统计  HTTP {st}  {stt["count"]} 次 / {stt["minutes"]} 分钟 / {stt["cost"]} 元')
A = {'X-Token': 'ADMIN', 'X-Pwd': 'admin123'}
st, inc = req('/api/admin/income', headers=A)
print(f'8) 当天收入  HTTP {st}  上机费={inc["feeIncome"]}  人次={inc["times"]}')
st, rc = req('/api/admin/recharge', 'POST', {'id': 'C003', 'amount': 20}, A)
print(f'9) 充值 C003 HTTP {st}  余额={rc["card"]["remain"]}')
print('=' * 60)
print('公网访问验证完成')
