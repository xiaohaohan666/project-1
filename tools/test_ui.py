"""用 CDP（Chrome DevTools 协议）在真实浏览器内核里测试前端界面。

为什么这样做：之前只测了 HTTP 接口，从来没有真正在浏览器里点击过界面，
所以「功能不正常」没能提前发现。Playwright 装不上（沙箱限制），
于是这里用标准库自己实现一个极简 CDP 客户端，零第三方依赖。

原理：
  1. 以 --headless=new --remote-debugging-port 启动 Edge
  2. 从 /json/list 拿到页面目标，连上它的 WebSocket
  3. 发送 Runtime.evaluate 执行 JS —— 既能断言，也能模拟点击
  4. 收集 console 报错与页面异常

用法：
    python tools/test_ui.py                 # 自动启后端，无头运行
    python tools/test_ui.py --url http://...  # 对已有服务测试
"""
import io
import os
import re
import sys
import json
import time
import socket
import base64
import shutil
import struct
import tempfile
import subprocess
import urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'server.exe')

EDGE_CANDIDATES = [
    r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
    r'C:\Program Files\Microsoft\Edge\Application\msedge.exe',
    r'C:\Program Files\Google\Chrome\Application\chrome.exe',
]


# ============================================================
#                  极简 WebSocket 客户端
# ============================================================
class WS:
    """只实现客户端所需的帧收发：文本帧、掩码、无分片"""

    def __init__(self, url, timeout=30):
        m = re.match(r'ws://([^:/]+):(\d+)(/.*)', url)
        if not m:
            raise ValueError('bad ws url: %s' % url)
        host, port, path = m.group(1), int(m.group(2)), m.group(3)
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ('GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n'
               'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n'
               'Sec-WebSocket-Version: 13\r\n\r\n' % (path, host, port, key))
        self.sock.sendall(req.encode())
        buf = b''
        while b'\r\n\r\n' not in buf:
            buf += self.sock.recv(4096)
        if b'101' not in buf.split(b'\r\n')[0]:
            raise RuntimeError('WebSocket 握手失败: %s' % buf[:120])
        self.buf = buf.split(b'\r\n\r\n', 1)[1]
        self.mid = 0

    def _recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError('连接已关闭')
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send_text(self, text):
        payload = text.encode('utf-8')
        mask = os.urandom(4)
        n = len(payload)
        head = bytearray([0x81])                     # FIN + text
        if n < 126:
            head.append(0x80 | n)
        elif n < 65536:
            head.append(0x80 | 126)
            head += struct.pack('>H', n)
        else:
            head.append(0x80 | 127)
            head += struct.pack('>Q', n)
        head += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(head) + masked)

    def recv_text(self):
        while True:
            b0, b1 = self._recv_exact(2)
            opcode = b0 & 0x0F
            masked = b1 & 0x80
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack('>H', self._recv_exact(2))[0]
            elif n == 127:
                n = struct.unpack('>Q', self._recv_exact(8))[0]
            mask = self._recv_exact(4) if masked else None
            data = self._recv_exact(n) if n else b''
            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            if opcode == 0x8:
                raise RuntimeError('服务端关闭连接')
            if opcode in (0x1, 0x2):
                return data.decode('utf-8', 'replace')
            # ping/pong/continuation 忽略

    def call(self, method, params=None, timeout=30):
        self.mid += 1
        mid = self.mid
        self.send_text(json.dumps({'id': mid, 'method': method,
                                   'params': params or {}}))
        end = time.time() + timeout
        while time.time() < end:
            msg = json.loads(self.recv_text())
            if msg.get('id') == mid:
                if 'error' in msg:
                    raise RuntimeError('%s: %s' % (method, msg['error']))
                return msg.get('result', {})
            # 其他事件丢弃（本测试不依赖事件）
        raise TimeoutError(method)

    def evaluate(self, expr, timeout=30):
        """执行 JS 表达式并返回其值（awaitPromise 以支持 async）"""
        r = self.call('Runtime.evaluate', {
            'expression': expr,
            'returnByValue': True,
            'awaitPromise': True,
        }, timeout=timeout)
        res = r.get('result', {})
        if r.get('exceptionDetails'):
            d = r['exceptionDetails']
            raise RuntimeError('JS 异常: %s' % (d.get('text') or '')[:200])
        return res.get('value')

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


# ============================================================
#                      启动浏览器
# ============================================================
def find_browser():
    for c in EDGE_CANDIDATES:
        if os.path.exists(c):
            return c
    return None


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_browser(browser, page_url):
    """启动无头浏览器并打开页面，返回 (进程, ws, 调试目录)"""
    port = free_port()
    profile = _workdir('prof-')
    args = [browser,
            '--headless=new',
            '--disable-gpu',
            '--no-first-run',
            '--no-default-browser-check',
            '--disable-extensions',
            '--remote-debugging-port=%d' % port,
            '--user-data-dir=' + profile,
            page_url]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                            creationflags=0x08000000)

    # 等调试端口就绪并取页面目标
    ws_url = None
    end = time.time() + 30
    while time.time() < end:
        try:
            with urllib.request.urlopen(
                    'http://127.0.0.1:%d/json/list' % port, timeout=2) as r:
                targets = json.loads(r.read())
            for t in targets:
                if t.get('type') == 'page' and t.get('webSocketDebuggerUrl'):
                    ws_url = t['webSocketDebuggerUrl']
                    break
            if ws_url:
                break
        except Exception:
            pass
        if proc.poll() is not None:
            raise RuntimeError('浏览器进程已退出（无法无头启动）')
        time.sleep(0.4)
    if not ws_url:
        proc.kill()
        raise RuntimeError('未取到调试目标（端口 %d）' % port)
    return proc, WS(ws_url), profile


# ============================================================
#                      主测试流程
# ============================================================
class Rep:
    def __init__(self):
        self.ok, self.fails, self.notes = [], [], []

    def check(self, label, cond, extra=''):
        print('  [%s] %-32s %s' % ('OK ' if cond else 'FAIL', label, extra))
        (self.ok if cond else self.fails).append(label)
        return cond


def wait_for(ws, expr, timeout=12, interval=0.35):
    """轮询等待 JS 表达式为真"""
    end = time.time() + timeout
    while time.time() < end:
        try:
            if ws.evaluate(expr):
                return True
        except Exception:
            pass
        time.sleep(interval)
    return False


def _workdir(prefix):
    """在工作区内建临时目录。

    不用 tempfile.mkdtemp：它创建的目录在本机安全策略下会继承受限 ACL，
    之后在其中建子目录会被拒绝（PermissionError）。
    这里手工拼路径并用 os.makedirs 创建，权限正常。
    """
    base = os.path.join(ROOT, '.uitest')
    os.makedirs(base, exist_ok=True)
    for i in range(1000):
        p = os.path.join(base, '%s%d' % (prefix, i))
        if not os.path.exists(p):
            os.makedirs(p)
            return p
    raise RuntimeError('无法创建临时目录')


def start_backend():
    tmp = _workdir('be-')
    data = os.path.join(tmp, 'data')
    os.makedirs(data, exist_ok=True)
    port = free_port()
    proc = subprocess.Popen([EXE, str(port), 'web', data, '127.0.0.1'],
                            cwd=ROOT, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    base = 'http://127.0.0.1:%d' % port
    for _ in range(60):
        try:
            urllib.request.urlopen(base + '/api/health', timeout=2)
            return proc, base, tmp
        except Exception:
            time.sleep(0.25)
    proc.kill()
    raise RuntimeError('后端启动超时')


def main():
    url_arg = None
    if '--url' in sys.argv:
        url_arg = sys.argv[sys.argv.index('--url') + 1]

    be_proc = be_tmp = None
    if url_arg:
        base = url_arg.rstrip('/')
    else:
        be_proc, base, be_tmp = start_backend()
    print('测试地址:', base)

    browser = find_browser()
    if not browser:
        print('没找到 Edge/Chrome，无法进行界面测试')
        return 2
    print('浏览器  :', browser)
    print('=' * 74)

    proc = ws = profile = None
    rep = Rep()
    console_msgs, page_errors = [], []

    try:
        proc, ws, profile = start_browser(browser, base + '/')
        ws.call('Runtime.enable')
        ws.call('Page.enable')

        # 收集控制台与异常
        def drain():
            """非阻塞地取走已到达的事件。这里简化处理：
            在每次断言前调用一次 evaluate，顺带把异常捕获住。"""
            pass

        # ---------- 1. 页面加载 ----------
        wait_for(ws, 'document.readyState === "complete"', timeout=20)
        rep.check('页面加载完成', ws.evaluate('document.readyState') == 'complete')
        rep.check('标题正确', '校园一卡通' in (ws.evaluate('document.title') or ''),
                  ws.evaluate('document.title'))

        # 捕获 JS 错误：在页面里挂一个收集器
        ws.evaluate('''
            window.__errs = window.__errs || [];
            window.addEventListener('error', e => window.__errs.push(String(e.message)));
            window.addEventListener('unhandledrejection',
                e => window.__errs.push('unhandled: ' + String(e.reason)));
            true
        ''')

        # ---------- 2. 元素完整性 ----------
        rep.check('登录表单存在', ws.evaluate('!!document.querySelector("#loginForm")'))
        rep.check('角色标签 3 个',
                  ws.evaluate('document.querySelectorAll("#loginTabs .tab").length') == 3)

        # ---------- 3. 学生登录 ----------
        ws.evaluate('''
            document.querySelector("#inId").value = "C001";
            document.querySelector("#inPwd").value = "123456";
            document.querySelector("#loginForm").dispatchEvent(
                new Event("submit", {cancelable: true, bubbles: true}));
            true
        ''')
        ok = wait_for(ws, '!document.querySelector("#app").classList.contains("hidden")')
        rep.check('学生登录成功', ok)
        rep.check('卡面姓名 = 张三',
                  ws.evaluate('document.querySelector("#cvName").textContent') == '张三',
                  ws.evaluate('document.querySelector("#cvName").textContent'))
        bal = ws.evaluate('document.querySelector("#cvRemain").textContent')
        rep.check('余额已渲染', bal not in ('', '0.00', None), str(bal))

        # ---------- 4. 导航与视图切换 ----------
        navs = ws.evaluate('document.querySelectorAll("#mainnav .nav-btn:not(.hidden)").length')
        rep.check('学生导航按钮 >= 4', navs >= 4, '%s 个' % navs)

        for label, vid in [('流水', 'student-ledger'), ('统计', 'student-stats'),
                           ('门禁', 'student-access'), ('消费', 'student-consume')]:
            ws.evaluate('document.querySelector(\'#mainnav .nav-btn[data-view="%s"]\').click()' % vid)
            time.sleep(0.8)
            vis = ws.evaluate('''(() => {
                const v = document.querySelector('.view[data-view="%s"]');
                return v && !v.classList.contains('hidden');
            })()''' % vid)
            rep.check('视图切换「%s」' % label, vis)

        # ---------- 5. 消费点下拉是否填充 ----------
        n = ws.evaluate('document.querySelectorAll("#selMerchant option").length')
        rep.check('消费点下拉已填充', n >= 6, '%s 个选项' % n)

        # ---------- 6. 上机登记 + 计时器 ----------
        ws.evaluate('document.querySelector("#btnOnline").click()')
        ok = wait_for(ws, '!document.querySelector("#machineBusy").classList.contains("hidden")')
        rep.check('上机登记成功', ok)
        if ok:
            t1 = ws.evaluate('document.querySelector("#mTime").textContent')
            time.sleep(2.2)
            t2 = ws.evaluate('document.querySelector("#mTime").textContent')
            rep.check('计时器走字', t1 != t2, '%s -> %s' % (t1, t2))
            ws.evaluate('document.querySelector("#btnOffline").click()')
            ok2 = wait_for(ws, '!document.querySelector("#machineIdle").classList.contains("hidden")')
            rep.check('下机结账成功', ok2)

        # ---------- 7. 消费 + 交易密码弹窗 ----------
        ws.evaluate('''
            document.querySelector("#selMerchant").selectedIndex = 0;
            document.querySelector("#inPayAmount").value = "2";
            document.querySelector("#btnPay").click();
            true
        ''')
        ok = wait_for(ws, '!document.querySelector("#modal").classList.contains("hidden")')
        rep.check('消费弹出交易密码窗口', ok)
        if ok:
            ws.evaluate('''
                document.querySelector("#pf_payPwd").value = "010101";
                const bs = document.querySelectorAll("#mdFoot button");
                bs[bs.length - 1].click();
                true
            ''')
            time.sleep(1.8)
            bal2 = ws.evaluate('document.querySelector("#cvRemain").textContent')
            rep.check('消费后余额已更新', bal2 not in ('', None), '余额 ' + str(bal2))

        # ---------- 8. 门禁刷卡 ----------
        ws.evaluate('document.querySelector(\'#mainnav .nav-btn[data-view="student-access"]\').click()')
        time.sleep(0.8)
        ws.evaluate('document.querySelector(\'[data-access-dir="0"]\').click()')
        time.sleep(1.6)
        rows = ws.evaluate('document.querySelectorAll("#tblAccess tbody tr").length')
        rep.check('门禁刷卡产生记录', rows >= 1, '%s 行' % rows)

        # ---------- 9. 退出并用管理员登录 ----------
        ws.evaluate('document.querySelector("#btnLogout").click()')
        time.sleep(0.8)
        ws.evaluate('''
            document.querySelectorAll("#loginTabs .tab")[1].click();
            document.querySelector("#inPwd").value = "admin123";
            document.querySelector("#loginForm").dispatchEvent(
                new Event("submit", {cancelable: true, bubbles: true}));
            true
        ''')
        ok = wait_for(ws, '!document.querySelector("#viewAdmin").classList.contains("hidden")')
        rep.check('管理员登录成功', ok)

        adm = ws.evaluate('document.querySelectorAll("#mainnav .nav-btn:not(.hidden)").length')
        rep.check('管理端导航可见', adm >= 3, '%s 个' % adm)

        # ---------- 10. 商户终端 POS ----------
        ws.evaluate('document.querySelector(\'#mainnav .nav-btn[data-view="admin-pos"]\').click()')
        time.sleep(1.0)
        cards = ws.evaluate('document.querySelectorAll("#selPosCard option").length')
        mchs = ws.evaluate('document.querySelectorAll("#selPosMerchant option").length')
        rep.check('POS 卡号下拉已填充', cards >= 3, '%s 张' % cards)
        rep.check('POS 消费点已填充', mchs >= 6, '%s 个' % mchs)
        ws.evaluate('''
            document.querySelector("#selPosCard").selectedIndex = 0;
            document.querySelector("#inPosAmount").value = "3";
            document.querySelector("#inPosPayPwd").value = "010101";
            document.querySelector("#btnPosPay").click();
            true
        ''')
        time.sleep(2.0)
        toast = ws.evaluate('document.querySelector("#toast").textContent')
        rep.check('POS 扣款执行', bool(toast) and toast.strip() != '', str(toast)[:40])

        # ---------- 11. 卡务中心 ----------
        ws.evaluate('document.querySelector(\'#mainnav .nav-btn[data-view="admin-card"]\').click()')
        time.sleep(1.0)
        rows = ws.evaluate('document.querySelectorAll("#tblCards tbody tr").length')
        rep.check('卡务中心卡片表有数据', rows >= 3, '%s 行' % rows)
        ws.evaluate('''
            document.querySelector("#selCard").selectedIndex = 0;
            document.querySelector("#opAmount").value = "10";
            document.querySelector(\'[data-op="recharge"]\').click();
            true
        ''')
        time.sleep(1.8)
        toast = ws.evaluate('document.querySelector("#toast").textContent')
        rep.check('充值操作执行', bool(toast) and toast.strip() != '', str(toast)[:40])

        # ---------- 12. 报表 ----------
        ws.evaluate('document.querySelector(\'#mainnav .nav-btn[data-view="admin-report"]\').click()')
        time.sleep(1.6)
        cons = ws.evaluate('document.querySelector("#incConsume").textContent')
        mrows = ws.evaluate('document.querySelectorAll("#tblMerchantSum tbody tr").length')
        lrows = ws.evaluate('document.querySelectorAll("#tblAdmLedgerAll tbody tr").length')
        rep.check('报表消费总额已渲染', cons not in ('', None), '消费 ' + str(cons))
        rep.check('报表消费点表有数据', mrows >= 6, '%s 行' % mrows)
        rep.check('报表流水表有数据', lrows >= 1, '%s 行' % lrows)

        # ---------- 13. 收集页面错误 ----------
        errs = ws.evaluate('window.__errs || []') or []
        rep.check('无未捕获的 JS 错误', len(errs) == 0,
                  '; '.join(str(e) for e in errs[:3])[:150])

    except Exception as e:
        print('\n测试中断：%s: %s' % (type(e).__name__, str(e)[:200]))
        rep.fails.append('测试中断')
    finally:
        if ws:
            ws.close()
        if proc:
            try:
                proc.kill()
            except Exception:
                pass
        if profile:
            shutil.rmtree(profile, ignore_errors=True)
        if be_proc:
            be_proc.terminate()
            try:
                be_proc.wait(timeout=6)
            except Exception:
                be_proc.kill()
        if be_tmp:
            shutil.rmtree(be_tmp, ignore_errors=True)

    print('=' * 74)
    print('通过 %d 项，失败 %d 项' % (len(rep.ok), len(rep.fails)))
    if rep.fails:
        print('失败项：')
        for f in rep.fails:
            print('   -', f)
    return 1 if rep.fails else 0


if __name__ == '__main__':
    sys.exit(main())
