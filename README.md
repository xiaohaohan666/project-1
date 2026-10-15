# 校园一卡通

一个用**纯 C 语言**实现的校园一卡通系统，含消费、机房上机计费、门禁考勤、
卡务办理与统计报表。全部代码为 C（C11 + 少量 Win32 API），无第三方运行时依赖。

## 三种使用方式

| 方式 | 入口 | 说明 |
|---|---|---|
| **桌面应用** | 双击 `校园一卡通.exe` | 独立窗口、无地址栏、无标签页；含自定义图标 |
| **网页** | 双击 `start.bat`，或运行 `server.exe 8090 . data 0.0.0.0` | 浏览器打开 `http://127.0.0.1:8090/` |
| **命令行** | `main.exe` | 学生 / 管理员两套菜单，适合教学演示 |

PWA：网页在 HTTPS 或 localhost 下，可用浏览器「添加到主屏幕」当 App 使用。

## 目录与文件

```
校园一卡通/
├─ campus.h / campus.c     业务核心：数据模型、限额风控、上机计费、报表
├─ server.c                网页服务：手写 HTTP/1.1 + JSON，静态托管
├─ main.c                  命令行界面：学生菜单 + 管理员菜单
├─ launcher.c              桌面应用：Win32 窗口，启动后端并打开界面
├─ launcher.rc             桌面应用的图标资源
├─ build.c                 构建器：生成图标 → 编译 → 跑单元测试
├─ test_jstr.c             JSON 多字节处理单元测试
├─ build.exe               已编译的构建器（可直接用，无需自行编译）
├─ campus.ico              应用图标（7 种尺寸）
├─ start.bat               一键启动（缺什么编什么，然后启动桌面应用）
├─ index.html              前端页面
├─ css/ js/ icons/         前端样式、脚本、PWA 图标
├─ sw.js / manifest.webmanifest / 404.html    PWA 支持
├─ Dockerfile              容器镜像（Linux 下编译命令行版）
└─ 模拟校园卡-俞琼-物联网工程.pdf    需求文档
```

## 构建

```bash
build.exe                # 生成图标 + 编译三个程序 + 跑单元测试
```

`build.exe` 已随仓库提供。需要自己重新编译构建器时（要 MinGW-w64 的 gcc）：

```bash
gcc -std=c11 -O2 build.c -o build.exe -lz
```

手工编译各程序（注意 **必须带 `-fexec-charset=UTF-8`**，否则中文会乱码）：

```bash
# 网页服务
gcc -std=c11 -Wall -Wextra -O2 -s -fexec-charset=UTF-8 \
    server.c campus.c -o server.exe -lws2_32

# 命令行版
gcc -std=c11 -Wall -Wextra -O2 -s -fexec-charset=UTF-8 \
    main.c campus.c -o main.exe

# 桌面应用（先编译图标资源）
windres launcher.rc -o launcher_res.o --output-format=coff
gcc -std=c11 -Wall -Wextra -O2 -s -mwindows -fexec-charset=UTF-8 \
    launcher.c launcher_res.o -o 校园一卡通.exe \
    -lcomctl32 -lgdi32 -luser32 -lws2_32 -lshell32
```

`-mwindows` 让 PE 子系统为 GUI，双击不会弹出黑色控制台窗口。

## 自检

桌面应用内置两个自检模式，不需要图形界面即可运行：

```bash
windres launcher.rc -o launcher_res.o --output-format=coff
gcc -std=c11 -O2 -fexec-charset=UTF-8 launcher.c launcher_res.o \
    -o selftest.exe -lcomctl32 -lgdi32 -luser32 -lws2_32 -lshell32

selftest.exe --layoutcheck   # 窗口适配：6 档尺寸下控件是否越界/重叠
selftest.exe --selftest      # 端到端：取 IP、探浏览器、启后端、心跳、停止
```

`--layoutcheck` 会把不同窗口尺寸下每个控件的几何位置算出来并检查，
是「窗口适配」这一项的回归测试。

## 账号

| 角色 | 账号 | 登录密码 | 交易密码 |
|---|---|---|---|
| 学生 | `C001` / `C002` / `C003` | `123456` | `010101` / `010102` / `010103` |
| 管理员 | 登录页切「管理员」 | `admin123` | — |

新注册的卡，交易密码默认取**学号末 6 位**。
默认限额：单笔 20 元、日累计 50 元（管理员可调整）。

## 功能

**学生端**：消费（6 个消费点）、机房上机与计时、门禁考勤刷卡、
交易流水、消费统计、上机记录、修改登录密码、修改交易密码、自助挂失/解挂。

**管理端**：商户终端模拟 POS 扣款、卡务中心（充值 / 挂失 / 解挂 / 冻结 /
重置密码 / 重置交易密码 / 补办 / 调整限额 / 延期 / 发放补助）、
当天全站汇总报表、各消费点消费额、交易流水、门禁记录、全部卡片一览。

消费点：`食堂`、`超市`、`文印店`、`热水洗衣`、`宿舍水电`、`图书馆罚款`、`机房`。

## 数据与接口

数据以 `|` 分隔的文本保存在 `data/`：

| 文件 | 内容 | 写入方式 |
|---|---|---|
| `cards.dat` | 全部卡片 | 全量覆写 |
| `ledger.dat` | 交易流水 | 追加 |
| `access.dat` | 门禁记录 | 追加 |
| `<卡号>.txt` | 该卡上机记录 | 追加 |
| `income.dat` | 旧版收入流水（兼容） | 追加 |

无服务端会话：每个请求带 `X-Token`（卡号或 `ADMIN`）与 `X-Pwd`；
消费类请求另带 `X-PayPwd`。接口列表见 `server.c` 的路由分派。

列表类接口支持分页（`?offset=N`），并回传 `total` / `count` / `hasMore`，
避免流水过多时响应超出缓冲区。

## 三个必须记住的坑

### 1. 固定长度缓冲区存中文**不能按字节截断**

一个汉字占 3 字节。若按字节数硬砍，末尾会留下不完整的字节序列，
**整条 JSON 响应都会无法解析**（`invalid continuation byte`），
现象是「界面能打开，但所有数据都拿不到、功能全不正常」。
本项目因此踩过两次（`char name[12]` 只够 4 个汉字；边界回退写成了
无条件执行，又白丢一个完整汉字）。现在统一为：**仅当确实被截断时**
才回退到字符边界，并由 `test_jstr.c` 守着。

### 2. 批处理文件必须 GBK + CRLF

`cmd` 按系统 ANSI 代码页读取批处理文件：

- 存成 UTF-8 → 中文注释解析失败
- 行尾为 LF → `rem` 注释行被截断，残句被当成命令执行

所以 `start.bat` 以 **GBK 保存、CRLF 换行**，并在 `.gitattributes`
里为 `*.bat` 指定 `eol=crlf`，防止换行被转换。

### 3. 窗口布局必须响应式

桌面应用的控件位置全部由 `compute_layout(W, H)` 按窗口宽度等比算出，
不能写死像素坐标。早期版本硬编码坐标，窗口一缩放就出现标题叠字、
控件跑到窗口外，看起来像「界面没做适配」。缩放系数只按**宽度**算：
内容高度随宽度等比增长，纵向多余留白由吸底的按钮行吸收。

## 已知限制

- 卡片上限 `CARD_MAX` 是编译期常量（默认 200）。超出部分不载入，
  但会通过 `cardTruncated` 标记显式告警，不再静默丢弃。
- 服务端单线程、无 TLS。真实部署应在前面加反向代理。
