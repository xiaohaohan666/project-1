#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校园一卡通 —— 桌面应用启动器（原生窗口）

双击即可运行：
  1. 首次或源码更新时自动编译 server.exe
  2. 在后台静默启动 C 语言后端（不弹控制台）
  3. 自动打开界面；若系统支持，用「应用模式」窗口（无地址栏、无标签页）
  4. 本窗口保持运行，显示服务状态、局域网地址与实时心跳
  5. 关闭本窗口即停止后端

为什么用 tkinter 做外壳：它是 Python 标准库，一定有原生窗口，
不依赖任何浏览器能否启动。界面本身仍由 C 服务托管的网页提供，
因此「应用窗口」与「网页」是同一套界面，不会有两份实现。

用法：
    校园一卡通.pyw            默认端口 8090
    校园一卡通.pyw 9000       指定端口
"""
import os
import sys
import time
import json
import socket
import threading
import subprocess
import urllib.request

try:
    import tkinter as tk
    from tkinter import ttk, messagebox
except Exception:
    tk = None

ROOT = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(ROOT, 'server.exe')
APP_NAME = '校园一卡通'
DEFAULT_PORT = 8090
LOGIN_INFO = '学生 C001 / C002 / C003 · 登录密码 123456 · 交易密码 010101 · 管理员 admin123'


# ============================================================
#                      环境准备
# ============================================================
def find_gcc():
    for c in ('gcc',
              r'C:\Program Files\mingw64\bin\gcc.exe',
              r'C:\mingw64\bin\gcc.exe',
              r'E:\mingw64\bin\gcc.exe'):
        if os.path.sep in c:
            if os.path.exists(c):
                return c
        else:
            for p in os.environ.get('PATH', '').split(os.pathsep):
                full = os.path.join(p, c + '.exe')
                if os.path.exists(full):
                    return full
    return None


def needs_build():
    if not os.path.exists(EXE):
        return True
    exe_t = os.path.getmtime(EXE)
    for src in ('src/campus.c', 'src/campus.h', 'src/server.c'):
        p = os.path.join(ROOT, *src.split('/'))
        if os.path.exists(p) and os.path.getmtime(p) > exe_t:
            return True
    return False


def build(log):
    gcc = find_gcc()
    if not gcc:
        return False, ('没有找到 gcc 编译器，且 server.exe 不存在。\n\n'
                       '请安装 MinGW-w64，或把编译好的 server.exe 放到程序目录。')
    log('正在编译（源码为 UTF-8，需 -fexec-charset=UTF-8）...')
    cmd = [gcc, '-std=c11', '-Wall', '-Wextra', '-O2',
           '-fexec-charset=UTF-8', '-Isrc',
           'src/server.c', 'src/campus.c', '-o', 'server.exe', '-lws2_32']
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    if r.returncode != 0:
        return False, '编译失败：\n\n' + (r.stderr or '')[-1200:]
    log('编译完成。')
    return True, ''


def lan_ip():
    """取本机局域网 IP（用 UDP 连一下外网地址，不会真的发包）"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('10.255.255.255', 1))
        return s.getsockname()[0]
    except Exception:
        try:
            return socket.gethostbyname(socket.gethostname())
        except Exception:
            return '127.0.0.1'
    finally:
        s.close()


def find_browser():
    """找一个支持应用模式的 Chromium 内核浏览器"""
    for c in (r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
              r'C:\Program Files\Microsoft\Edge\Application\msedge.exe',
              r'C:\Program Files\Google\Chrome\Application\chrome.exe',
              r'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe',
              os.path.expandvars(r'%LOCALAPPDATA%\Microsoft\Edge\Application\msedge.exe'),
              os.path.expandvars(r'%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe')):
        if os.path.exists(c):
            return c
    return None


def make_shortcut():
    """首次运行创建桌面快捷方式"""
    if os.name != 'nt':
        return None
    try:
        desktop = os.path.join(os.path.expanduser('~'), 'Desktop')
        if not os.path.isdir(desktop):
            desktop = os.path.join(os.path.expanduser('~'), '桌面')
        if not os.path.isdir(desktop):
            return None
        link = os.path.join(desktop, APP_NAME + '.lnk')
        if os.path.exists(link):
            return link
        pyw = sys.executable.replace('python.exe', 'pythonw.exe')
        if not os.path.exists(pyw):
            pyw = sys.executable
        vbs = ('Set s = CreateObject("WScript.Shell")\n'
               'Set l = s.CreateShortcut("%s")\n'
               'l.TargetPath = "%s"\n'
               'l.Arguments = """%s"""\n'
               'l.WorkingDirectory = "%s"\n'
               'l.Description = "%s"\n'
               'l.Save\n' % (link, pyw, os.path.abspath(__file__), ROOT, APP_NAME))
        tmp = os.path.join(os.environ.get('TEMP', '.'), '_mklnk_campus.vbs')
        with open(tmp, 'w', encoding='gbk', errors='replace') as f:
            f.write(vbs)
        subprocess.run(['cscript', '//nologo', tmp],
                       capture_output=True, creationflags=0x08000000)
        os.remove(tmp)
        return link
    except Exception:
        return None


# ============================================================
#                    后端进程管理
# ============================================================
class Backend:
    def __init__(self, port):
        self.port = port
        self.proc = None

    @property
    def base(self):
        return 'http://127.0.0.1:%d' % self.port

    def start(self):
        flags = 0x08000000 if os.name == 'nt' else 0   # CREATE_NO_WINDOW
        self.proc = subprocess.Popen(
            [EXE, str(self.port), 'web', 'data', '0.0.0.0'],
            cwd=ROOT, creationflags=flags,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def ready(self, timeout=25):
        """等服务就绪；返回 True/False"""
        end = time.time() + timeout
        while time.time() < end:
            if self.proc and self.proc.poll() is not None:
                return False
            try:
                with urllib.request.urlopen(self.base + '/api/health', timeout=1) as r:
                    if r.status == 200:
                        return True
            except Exception:
                time.sleep(0.3)
        return False

    def health(self):
        try:
            with urllib.request.urlopen(self.base + '/api/health', timeout=2) as r:
                return json.loads(r.read())
        except Exception:
            return None

    def beat_age(self):
        try:
            with urllib.request.urlopen(self.base + '/api/beat/age', timeout=2) as r:
                return json.loads(r.read()).get('age')
        except Exception:
            return None

    def stop(self):
        if not self.proc:
            return
        try:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=5)
                except Exception:
                    self.proc.kill()
        except Exception:
            pass
        self.proc = None


# ============================================================
#                       主窗口
# ============================================================
class App:
    def __init__(self, port):
        self.port = port
        self.backend = Backend(port)
        self.url = 'http://127.0.0.1:%d/' % port
        self.lan = lan_ip()
        self.browser = find_browser()
        self.app_window = False     # 是否已用应用模式打开
        self.stopping = False

        self.root = tk.Tk()
        self.root.title('%s —— 桌面应用' % APP_NAME)
        self.root.geometry('560x430')
        self.root.minsize(520, 400)
        self._center()
        self.root.protocol('WM_DELETE_WINDOW', self.on_close)
        self._build_ui()
        threading.Thread(target=self._boot, daemon=True).start()
        self.root.after(1000, self._tick)

    # ---------- 界面 ----------
    def _center(self):
        self.root.update_idletasks()
        w, h = 560, 430
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        self.root.geometry('%dx%d+%d+%d' % (w, h, (sw - w) // 2, (sh - h) // 3))

    def _build_ui(self):
        pad = {'padx': 16}
        head = tk.Frame(self.root, bg='#2563eb', height=64)
        head.pack(fill='x')
        head.pack_propagate(False)
        tk.Label(head, text='校园一卡通', bg='#2563eb', fg='white',
                 font=('Microsoft YaHei UI', 16, 'bold')).pack(side='left', padx=16)
        tk.Label(head, text='消费 · 机房上机 · 门禁考勤', bg='#2563eb', fg='#dbeafe',
                 font=('Microsoft YaHei UI', 9)).pack(side='left', pady=(8, 0))

        body = tk.Frame(self.root)
        body.pack(fill='both', expand=True, **pad)

        # 状态
        self.lbl_status = tk.Label(body, text='正在启动服务 ...', anchor='w',
                                   font=('Microsoft YaHei UI', 11, 'bold'),
                                   fg='#b45309')
        self.lbl_status.pack(fill='x', pady=(12, 6))

        self.lbl_detail = tk.Label(body, text='', anchor='w', justify='left',
                                   font=('Microsoft YaHei UI', 9), fg='#4b5563')
        self.lbl_detail.pack(fill='x')

        # 地址框（方便复制到手机）
        box = tk.Frame(body)
        box.pack(fill='x', pady=(12, 4))
        tk.Label(box, text='本机打开', width=10, anchor='w',
                 font=('Microsoft YaHei UI', 9)).pack(side='left')
        self.e_local = tk.Entry(box, font=('Consolas', 10))
        self.e_local.insert(0, self.url)
        self.e_local.pack(side='left', fill='x', expand=True)

        box2 = tk.Frame(body)
        box2.pack(fill='x', pady=4)
        tk.Label(box2, text='手机访问', width=10, anchor='w',
                 font=('Microsoft YaHei UI', 9)).pack(side='left')
        self.e_lan = tk.Entry(box2, font=('Consolas', 10))
        self.e_lan.insert(0, 'http://%s:%d/' % (self.lan, self.port))
        self.e_lan.pack(side='left', fill='x', expand=True)
        tk.Label(body, text='手机与本电脑连同一个 WiFi 时可用；点「复制」发到手机',
                 anchor='w', font=('Microsoft YaHei UI', 8), fg='#6b7280').pack(fill='x')

        # 按钮
        btns = tk.Frame(body)
        btns.pack(fill='x', pady=(14, 8))
        self.btn_open = tk.Button(btns, text='打开界面', width=12, height=2,
                                  font=('Microsoft YaHei UI', 10),
                                  command=self.open_ui, state='disabled')
        self.btn_open.pack(side='left')
        self.btn_app = tk.Button(btns, text='应用模式窗口', width=14, height=2,
                                 font=('Microsoft YaHei UI', 10),
                                 command=lambda: self.open_ui(app_mode=True),
                                 state='disabled')
        self.btn_app.pack(side='left', padx=8)
        tk.Button(btns, text='复制手机地址', width=12, height=2,
                  font=('Microsoft YaHei UI', 10),
                  command=self.copy_lan).pack(side='left')
        tk.Button(btns, text='退出', width=8, height=2,
                  font=('Microsoft YaHei UI', 10),
                  command=self.on_close).pack(side='right')

        # 日志
        tk.Label(body, text='运行日志', anchor='w',
                 font=('Microsoft YaHei UI', 9, 'bold')).pack(fill='x', pady=(8, 2))
        self.txt = tk.Text(body, height=6, font=('Consolas', 9),
                           bg='#f8fafc', relief='solid', borderwidth=1)
        self.txt.pack(fill='both', expand=True)
        self.txt.configure(state='disabled')

        tk.Label(self.root, text=LOGIN_INFO, anchor='w', fg='#374151',
                 font=('Microsoft YaHei UI', 8)).pack(fill='x', padx=16, pady=(0, 10))

    # ---------- 工具 ----------
    def log(self, msg):
        def _do():
            self.txt.configure(state='normal')
            self.txt.insert('end', time.strftime('[%H:%M:%S] ') + msg + '\n')
            self.txt.see('end')
            self.txt.configure(state='disabled')
        try:
            self.root.after(0, _do)
        except Exception:
            pass

    def set_status(self, text, color='#b45309'):
        try:
            self.root.after(0, lambda: self.lbl_status.configure(text=text, fg=color))
        except Exception:
            pass

    def set_detail(self, text):
        try:
            self.root.after(0, lambda: self.lbl_detail.configure(text=text))
        except Exception:
            pass

    # ---------- 启动流程 ----------
    def _boot(self):
        if needs_build():
            self.set_status('正在编译后端 ...')
            ok, err = build(self.log)
            if not ok:
                self.set_status('启动失败', '#dc2626')
                self.log(err)
                self.root.after(0, lambda: messagebox.showerror('启动失败', err))
                return
        else:
            self.log('已存在最新的 server.exe，跳过编译。')

        self.set_status('正在启动服务 ...')
        self.backend.start()
        if not self.backend.ready():
            self.set_status('服务启动失败', '#dc2626')
            self.log('后端在 25 秒内未就绪，请检查是否有其他程序占用了端口 %d。'
                     % self.port)
            return

        h = self.backend.health() or {}
        self.set_status('服务运行中', '#059669')
        self.log('服务已就绪：%d 张卡，机房费率 %.2f 元/小时'
                 % (h.get('cards', 0), h.get('rate', 0)))
        link = make_shortcut()
        if link:
            self.log('桌面快捷方式：%s' % link)
        self.root.after(0, lambda: (self.btn_open.configure(state='normal'),
                                    self.btn_app.configure(state='normal')))
        self.open_ui(app_mode=True)      # 自动打开界面

    # ---------- 打开界面 ----------
    def open_ui(self, app_mode=False):
        """app_mode=True 时优先用应用模式窗口（无地址栏、无标签页）"""
        if app_mode and self.browser:
            prof = os.path.join(os.environ.get('TEMP', '.'), 'campus-app-profile')
            try:
                subprocess.Popen(
                    [self.browser, '--app=' + self.url, '--window-size=1180,820',
                     '--user-data-dir=' + prof, '--no-first-run',
                     '--no-default-browser-check'],
                    creationflags=0x08000000 if os.name == 'nt' else 0)
                self.app_window = True
                self.log('已用应用模式打开界面（独立窗口，无地址栏）')
                return
            except Exception as e:
                self.log('应用模式启动失败，改用默认浏览器：%s' % e)
        try:
            os.startfile(self.url)
            self.log('已在默认浏览器中打开界面')
        except Exception as e:
            self.log('打开浏览器失败：%s' % e)

    def copy_lan(self):
        text = 'http://%s:%d/' % (self.lan, self.port)
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self.log('已复制到剪贴板：%s' % text)
        except Exception as e:
            self.log('复制失败：%s' % e)

    # ---------- 定时刷新状态 ----------
    def _tick(self):
        if self.stopping:
            return
        h = self.backend.health()
        if h:
            age = self.backend.beat_age()
            if age is None or age < 0:
                page = '界面尚未打开'
            elif age <= 14:
                page = '界面在线'
            else:
                page = '界面已关闭（%d 秒无心跳）' % age
            self.set_detail('卡片 %d 张 · 费率 %.2f 元/小时 · %s'
                            % (h.get('cards', 0), h.get('rate', 0), page))
        else:
            if self.backend.proc and self.backend.proc.poll() is not None:
                self.set_status('服务已退出', '#dc2626')
        self.root.after(2000, self._tick)

    # ---------- 退出 ----------
    def on_close(self):
        if self.stopping:
            return
        self.stopping = True
        # 若用应用模式开的窗口，顺手把它关掉
        if self.app_window and self.browser and os.name == 'nt':
            try:
                subprocess.run(['taskkill', '/F', '/IM',
                                os.path.basename(self.browser)],
                               capture_output=True, creationflags=0x08000000)
            except Exception:
                pass
        self.backend.stop()
        try:
            self.root.destroy()
        except Exception:
            pass


# ============================================================
def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    port = int(args[0]) if args and args[0].isdigit() else DEFAULT_PORT

    if tk is None:
        # 没有图形环境（极罕见）：退化为直接启动服务并打开浏览器
        if needs_build():
            ok, err = build(print)
            if not ok:
                print(err)
                return 1
        b = Backend(port)
        b.start()
        if b.ready():
            print('校园一卡通已启动：http://127.0.0.1:%d/' % port)
            try:
                os.startfile('http://127.0.0.1:%d/' % port)
            except Exception:
                pass
            try:
                while True:
                    time.sleep(5)
                    age = b.beat_age()
                    if age is not None and age > 14:
                        break
            except KeyboardInterrupt:
                pass
        b.stop()
        return 0

    App(port).root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
