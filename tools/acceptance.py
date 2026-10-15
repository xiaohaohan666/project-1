"""一卡通端到端验收：自带独立实例与干净数据，不依赖外部服务。

用法：
    python tools/acceptance.py            # 自建实例，用完即停
    python tools/acceptance.py <URL>      # 对已有服务验收（需自行保证初始状态）

自建实例会：在临时目录生成全新数据 → 用空闲端口启动 server.exe →
跑完 23 项 → 关闭服务并删除临时数据。
这样无论跑多少次、机器上有没有别的服务在跑，结果都一致。
"""
import io
import os
import sys
import json
import random
import time
import socket
import shutil
import tempfile
import subprocess
import urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'server.exe')


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def _workdir(prefix):
    """在工作区内建临时目录。

    不用 tempfile.mkdtemp：它创建的目录在本机安全策略下会继承受限 ACL，
    之后在其中建子目录会被拒绝（PermissionError）。手工拼路径则正常。
    """
    base = os.path.join(ROOT, '.accept')
    os.makedirs(base, exist_ok=True)
    for i in range(1000):
        p = os.path.join(base, '%s%d' % (prefix, i))
        if not os.path.exists(p):
            os.makedirs(p)
            return p
    raise RuntimeError('无法创建临时目录')


def start_server():
    """在临时目录启动一个全新实例，返回 (进程, 基地址, 临时目录)"""
    if not os.path.exists(EXE):
        print('找不到 server.exe，请先编译：')
        print('  gcc -std=c11 -Wall -Wextra -O2 -fexec-charset=UTF-8 '
              '-Isrc src/server.c src/campus.c -o server.exe -lws2_32')
        sys.exit(2)
    tmp = _workdir('run-')
    data = os.path.join(tmp, 'data')
    os.makedirs(data, exist_ok=True)          # 空目录 -> 服务会自动播种 3 张演示卡
    port = free_port()
    proc = subprocess.Popen([EXE, str(port), 'web', data, '127.0.0.1'],
                            cwd=ROOT, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    base = 'http://127.0.0.1:%d' % port
    for _ in range(40):
        try:
            urllib.request.urlopen(base + '/api/health', timeout=2)
            return proc, base, tmp
        except Exception:
            time.sleep(0.25)
    proc.kill()
    print('服务启动超时')
    sys.exit(2)


def call(base, path, method='GET', body=None, hdr=None):
    h = dict(hdr or {})
    data = json.dumps(body, ensure_ascii=False).encode() if body is not None else None
    if data:
        h['Content-Type'] = 'application/json'
    req = urllib.request.Request(base + path, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read() or b'{}')
    except Exception as e:
        return None, {'error': str(e)[:80]}


def run(base):
    A = {'X-Token': 'ADMIN', 'X-Pwd': 'admin123'}
    S = {'X-Token': 'C001', 'X-Pwd': '123456'}
    fails = []

    def check(label, cond, extra=''):
        print('  [%s] %-26s %s' % ('OK ' if cond else 'FAIL', label, extra))
        if not cond:
            fails.append(label)
            # 失败时把服务端原始返回打出来，便于定位是逻辑错还是断言错
            print('       ^ 判定为假，最后响应: %s' % str(d)[:200])
        return cond

    st, d = call(base, '/api/health')
    check('健康检查', st == 200 and d.get('ok'), '卡片 %s 张' % d.get('cards'))

    st, d = call(base, '/api/merchants')
    check('商户列表（7 个消费点）', st == 200 and len(d.get('merchants', [])) == 7,
          str(d.get('merchants')))

    # 每次用不同卡号，避免与历史数据冲突。
    # 用时间戳低位会在一秒内重复运行验收时撞号，故用随机数。
    cid = 'C9%03d' % random.randint(0, 999)
    st, d = call(base, '/api/register', 'POST',
                 {'id': cid, 'name': '验收同学', 'number': 'B9900100', 'pwd': 'abcd12'})
    check('新卡注册', st == 200 and d.get('ok'), cid)

    st, d = call(base, '/api/admin/recharge', 'POST', {'id': 'C001', 'amount': 100}, A)
    check('管理员充值 100 元', st == 200 and d.get('ok'),
          '余额 %.2f' % d.get('card', {}).get('remain', 0))

    st, d = call(base, '/api/admin/pay', 'POST',
                 {'id': 'C001', 'merchant': '食堂', 'amount': 15, 'payPwd': '010101'}, A)
    check('商户终端代扣（食堂 15）', st == 200 and d.get('ok'),
          '余额 %.2f' % d.get('card', {}).get('remain', 0))

    st, d = call(base, '/api/admin/pay', 'POST',
                 {'id': 'C001', 'merchant': '超市', 'amount': 5, 'payPwd': '999999'}, A)
    check('交易密码错误被拦', d.get('error') == '交易密码错误')

    # 演示卡单笔限额 20，先用 25 触发拦截
    st, d = call(base, '/api/admin/pay', 'POST',
                 {'id': 'C001', 'merchant': '超市', 'amount': 25, 'payPwd': '010101'}, A)
    check('超单笔限额被拦', '限额' in str(d.get('error')), str(d.get('error')))

    st, d = call(base, '/api/admin/limits', 'POST',
                 {'id': 'C001', 'limitOnce': 50, 'limitDay': 100}, A)
    check('调整限额为 50/100', st == 200 and d.get('ok'))

    st, d = call(base, '/api/admin/pay', 'POST',
                 {'id': 'C001', 'merchant': '文印店', 'amount': 30, 'payPwd': '010101'}, A)
    check('限额提高后 30 元通过', st == 200 and d.get('ok'))

    st, d = call(base, '/api/ledger?period=day', hdr=S)
    check('学生查交易流水', st == 200 and d.get('total', 0) >= 3,
          '共 %s 条' % d.get('total'))

    st, d = call(base, '/api/consume?period=day', hdr=S)
    check('学生消费统计', st == 200 and d.get('amount', 0) > 0,
          '%.2f 元 / %s 笔' % (d.get('amount', 0), d.get('count')))

    st, d = call(base, '/api/access', 'POST',
                 {'place': '图书馆', 'dir': 0, 'kind': 1}, S)
    check('门禁考勤签到', st == 200 and d.get('ok'), d.get('message', ''))

    st, d = call(base, '/api/admin/report', hdr=A)
    check('当天全站报表', st == 200 and d.get('consumeTotal', 0) > 0,
          '消费 %.2f / %s 笔 / 充值 %.2f' % (d.get('consumeTotal', 0),
                                             d.get('consumeTimes'),
                                             d.get('rechargeTotal', 0)))
    check('各消费点分列', len(d.get('merchants', [])) == 7)

    st, d = call(base, '/api/admin/lost', 'POST', {'id': 'C002'}, A)
    check('管理员挂失 C002', st == 200 and d.get('card', {}).get('stateText') == '挂失')

    st, d = call(base, '/api/admin/pay', 'POST',
                 {'id': 'C002', 'merchant': '食堂', 'amount': 5, 'payPwd': '010102'}, A)
    check('挂失卡消费被拦', '挂失' in str(d.get('error')), str(d.get('error')))

    st, d = call(base, '/api/admin/unlost', 'POST', {'id': 'C002'}, A)
    check('解除挂失', st == 200 and d.get('card', {}).get('stateText') == '正常')

    # C002 初始余额 30，扣 5 元工本费后应为 25
    st, d = call(base, '/api/admin/reissue', 'POST', {'id': 'C002', 'fee': 5}, A)
    check('补办扣工本费 5 元', st == 200 and abs(d.get('card', {}).get('remain', -1) - 25) < 0.01,
          '余额 %.2f' % d.get('card', {}).get('remain', -1))

    st, d = call(base, '/api/admin/grant', 'POST',
                 {'id': 'C003', 'amount': 50, 'type': 4, 'memo': '新生补助'}, A)
    check('发放补助 50 元', st == 200 and d.get('ok'),
          '余额 %.2f' % d.get('card', {}).get('remain', 0))

    st, d = call(base, '/api/admin/expire', 'POST', {'id': 'C003', 'date': '2032-12-31'}, A)
    check('卡片延期', st == 200 and d.get('card', {}).get('expire') == '2032-12-31')

    st, d = call(base, '/api/admin/resetpay', 'POST', {'id': 'C003'}, A)
    check('重置交易密码', st == 200 and d.get('ok'))

    st, d = call(base, '/api/admin/access?id=', hdr=A)
    check('管理端查门禁记录', st == 200 and d.get('total', 0) >= 1,
          '共 %s 条' % d.get('total'))

    st, d = call(base, '/api/admin/cards', hdr=A)
    check('全部卡片列表', st == 200 and d.get('total', 0) >= 4,
          '%s 张，余额合计 %.2f' % (d.get('total'), d.get('remainTotal', 0)))

    return fails


def main():
    if len(sys.argv) > 1:
        base = sys.argv[1]
        print('对已有服务验收:', base)
        print('（注意：此模式假定服务处于初始状态，重复运行可能因数据变化而失败）')
        print('=' * 66)
        fails = run(base)
    else:
        proc, base, tmp = start_server()
        print('自建独立实例:', base)
        print('临时数据目录:', tmp)
        print('=' * 66)
        try:
            fails = run(base)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except Exception:
                proc.kill()
            shutil.rmtree(tmp, ignore_errors=True)
            print('已关闭实例并清理临时数据')

    print('=' * 66)
    total = 23
    print('通过 %d / %d 项' % (total - len(fails), total))
    if fails:
        print('失败项:', '、'.join(fails))
        sys.exit(1)
    print('全部通过')


if __name__ == '__main__':
    main()
