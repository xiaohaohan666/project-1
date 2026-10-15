"""一键构建全部产物（Windows + MinGW-w64）。

依次编译：
  1. server.exe        网页服务（静态托管 + REST 接口）
  2. main.exe          命令行版
  3. 校园一卡通.exe     桌面应用（含图标资源）
  4. 自检版（可选）     用于 --selftest，检查完即删

用法：
    python tools/build.py           全部编译
    python tools/build.py --release 同上，但为零告警严格要求（-Werror）
"""
import io
import os
import sys
import glob
import shutil
import subprocess

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GCC_CANDIDATES = [
    r'C:\Program Files\mingw64\bin\gcc.exe',
    r'C:\mingw64\bin\gcc.exe',
    r'E:\mingw64\bin\gcc.exe',
    'gcc',
]
WINDRES_CANDIDATES = [
    r'C:\Program Files\mingw64\bin\windres.exe',
    r'C:\mingw64\bin\windres.exe',
    r'E:\mingw64\bin\windres.exe',
    'windres',
]

COMMON = ['-std=c11', '-Wall', '-Wextra', '-O2', '-fexec-charset=UTF-8']


def find(cands):
    for c in cands:
        if os.path.sep in c:
            if os.path.exists(c):
                return c
        else:
            for p in os.environ.get('PATH', '').split(os.pathsep):
                if os.path.exists(os.path.join(p, c + '.exe')):
                    return c
    return None


def run(cmd, label):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    warn = (r.stderr or '').strip()
    if r.returncode != 0:
        print('  [失败] %s' % label)
        print('    ' + '\n    '.join(warn.splitlines()[-12:]))
        return False
    if warn:
        print('  [告警] %s' % label)
        for line in warn.splitlines()[:6]:
            print('    ' + line)
    else:
        print('  [通过] %s' % label)
    return True


def main():
    strict = '--release' in sys.argv
    gcc = find(GCC_CANDIDATES)
    windres = find(WINDRES_CANDIDATES)
    if not gcc:
        print('没有找到 gcc，请安装 MinGW-w64')
        return 1

    extra = ['-Werror=implicit-function-declaration']
    if strict:
        extra.append('-Werror')

    print('编译器 :', gcc)
    print('windres:', windres or '未找到（桌面应用将没有自定义图标）')
    print('=' * 70)

    ok = True
    base = [gcc] + COMMON + extra

    print('[1/5] server.exe —— 网页服务')
    ok &= run(base + ['-Isrc', 'src/server.c', 'src/campus.c',
                      '-o', 'server.exe', '-lws2_32'], 'server.exe')

    print('[2/5] main.exe —— 命令行版')
    ok &= run(base + ['-Isrc', 'main.c', 'src/campus.c',
                      '-o', 'main.exe'], 'main.exe')

    print('[3/5] 校园一卡通.exe —— 桌面应用')
    res_o = None
    if windres:
        if not os.path.exists(os.path.join(ROOT, 'campus.ico')):
            print('  未找到 campus.ico，先运行 tools/make_icon.py')
        else:
            if run([windres, '--input', 'src/launcher.rc', '--output',
                    'launcher_res.o', '--output-format=coff'], '编译图标资源'):
                res_o = ['launcher_res.o']
            else:
                ok = False
    app = [gcc] + COMMON + extra + ['-mwindows', 'src/launcher.c'] + \
          (res_o or []) + ['-o', '校园一卡通.exe',
                           '-lcomctl32', '-lgdi32', '-luser32', '-lws2_32',
                           '-lshell32']
    ok &= run(app, '校园一卡通.exe')

    print('[4/5] 单元测试')
    test_c = 'tests/test_jstr.c'
    if os.path.exists(os.path.join(ROOT, test_c)):
        exe = os.path.join(ROOT, 'test_jstr.exe')
        if run(base + [test_c, '-o', 'test_jstr.exe'], '编译单元测试'):
            r = subprocess.run([exe], cwd=ROOT, capture_output=True, text=True,
                               encoding='utf-8', errors='replace')
            tail = (r.stdout or '').strip().splitlines()[-1:] or ['']
            if r.returncode == 0:
                print('  [通过] ' + tail[0])
            else:
                ok = False
                print('  [失败] ' + tail[0])
                print('    ' + '\n    '.join((r.stdout or '').splitlines()[-8:]))
            try:
                os.remove(exe)
            except Exception:
                pass
    else:
        print('  跳过（没有 tests/test_jstr.c）')

    print('[5/5] 清理中间文件')
    for f in glob.glob(os.path.join(ROOT, '*.o')):
        try:
            os.remove(f)
        except Exception:
            pass
    for d in ('run/data',):
        pass
    print('  已清理 .o 文件')

    print('=' * 70)
    print('构建结果:', '全部成功' if ok else '有失败项')
    for f in ('server.exe', 'main.exe', '校园一卡通.exe'):
        p = os.path.join(ROOT, f)
        if os.path.exists(p):
            print('  %-18s %8.1f KB' % (f, os.path.getsize(p) / 1024.0))
        else:
            print('  %-18s 缺失' % f)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
