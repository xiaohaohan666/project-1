# 模拟校园卡 —— 机房上机计费与消费记录查询系统

按《模拟校园卡》（出题人：俞琼，面向专业：物联网工程）要求实现的 C 语言程序，
提供**命令行版**与**网页版**两种界面，两版共用同一套业务规则与数据文件。

```
main.c              命令行版：登录、学生端、管理员端、主函数
src/campus.h/.c     业务核心：卡片读写、上机计费、挂失、统计
src/server.c        HTTP/JSON 服务（纯 C，跨 Windows / Linux，无第三方依赖）
web/                网页前端（原生 HTML/CSS/JS，零构建）
Dockerfile          容器镜像定义
.github/workflows/  CI：Linux + Windows 双平台编译与接口冒烟测试
data/               数据目录（卡片、上机记录、收入流水）
```

## 一、题目要求与实现对应

| 题目要求 | 实现 |
|---|---|
| 登录模块：同学根据自己设定的密码登录，**三次错误则冻结账户** | `student_login()` / `campus_check_pwd()` |
| 管理员：**充值、挂失、重置学生密码** | 管理员菜单 1 / 2 / 3 |
| 学生：查询**卡中余额、上机时间和地点** | 学生菜单 1，上机中还显示已用时长与预计费用 |
| 学生：**统计自己上机时间、上机费用** | 学生菜单 4，可选今天 / 本周 / 本月 / 全部 |
| 参考数据结构：卡号、姓名、学号、充值数额、余额、状态（正常、挂失、冻结） | `Card` 结构体 |
| 每个学生对应一个文本文件存放上机记录：卡号、上机日期、开始时间、上机用时、状态、上机地点 | `Record` 结构体 + `<卡号>.txt` |
| 上机记录表**永久保存**，用于事后查询和统计 | 记录**追加**写入，永不覆盖 |
| 菜单选择项：角色选择、时间选择、功能选择 | 主菜单 / 统计时间菜单 / 各级功能菜单 |
| 选做：对用户输入进行正确性检查 | 数字、金额、菜单编号、卡号、密码全部校验 |
| 选做：**管理员统计全部当天收入情况** | 管理员菜单 4 |
| 其他：注释详细、层次清晰、命名规范 | 源码逐项注释，分六层组织 |

## 二、功能清单

**主菜单（角色选择）**
```
1. 学生登录      2. 管理员登录      0. 返回（退出系统）
```

**学生菜单**
```
1. 查询卡中余额、上机时间和地点
2. 上机登记
3. 下机结账
4. 统计上机时间与费用      （今天 / 本周 / 本月 / 全部）
5. 修改登录密码            （题目要求"同学根据自己设定的密码登录"）
```

**管理员菜单**
```
1. 学生充值
2. 卡片挂失                （正在上机则先自动结账）
3. 重置学生密码（同时解除冻结）
4. 统计当天收入情况
```

## 三、计费规则

- 费率 **2.00 元 / 小时**（`RATE_PER_HOUR`），按实际分钟折算，保留两位小数；
- 上机登记记录开始时间与地点；下机结账计算用时、扣余额、写记录；
- 余额不足不能上机；结账时余额不够则清零并视为欠费；
- **挂失时若正在上机，先自动按下机结账**，避免费用无上限累积。

## 四、编译与运行

### 命令行版

```bash
gcc -std=c11 -Wall -Wextra -O2 -fexec-charset=UTF-8 -Isrc main.c src/campus.c -o main
./main
```

### 网页版（本地）

```bash
gcc -std=c11 -Wall -Wextra -O2 -fexec-charset=UTF-8 -Isrc src/server.c src/campus.c -o campus-server
./campus-server 8090 web data
```

浏览器打开 **http://127.0.0.1:8090/**；若要让同一局域网的同学访问，用本机内网 IP 替换
`127.0.0.1`（服务默认监听 `0.0.0.0`）。
Windows 下编译服务需额外链接 WinSock：加 `-lws2_32`。

演示卡号 `C001` / `C002` / `C003`，密码 `123456`；管理员密码 `admin123`。

> 源码为 UTF-8。Windows 下编译命令行版必须带 `-fexec-charset=UTF-8`，否则中文乱码
> （程序内已用 `SetConsoleOutputCP(65001)` 让控制台按 UTF-8 显示）。

## 五、REST API

身份通过请求头 `X-Token`（学生为卡号，管理员为 `ADMIN`）与 `X-Pwd`（对应密码）携带，
服务端每次校验，因此无服务端会话、天然支持多副本部署。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET  | `/api/health` | 服务与费率自检 |
| POST | `/api/login` | `{id,pwd,admin?}` 登录校验 |
| GET  | `/api/card` | 当前卡片：余额、累计充值、状态、上机信息 |
| POST | `/api/machine` | `{action:"online"\|"offline", place?}` 上机 / 下机 |
| GET  | `/api/records` | 当前卡的上机记录 |
| GET  | `/api/stats?period=day\|week\|month\|all` | 上机次数 / 时长 / 费用 |
| POST | `/api/lost` | 学生自助挂失 |
| POST | `/api/pwd` | `{newPwd}` 学生修改密码 |
| GET  | `/api/admin/cards` | 全部卡片与汇总 |
| GET  | `/api/admin/income` | 当天收入统计 |
| GET  | `/api/admin/records?id=C001` | 指定卡的上机记录 |
| POST | `/api/admin/recharge` | `{id,amount}` 充值 |
| POST | `/api/admin/lost` | `{id}` 挂失 |
| POST | `/api/admin/reset` | `{id}` 重置学生密码 |

```bash
# 登录
curl -s -X POST http://127.0.0.1:8090/api/login \
     -H "Content-Type: application/json" -d '{"id":"C001","pwd":"123456"}'

# 上机登记
curl -s -X POST http://127.0.0.1:8090/api/machine \
     -H "Content-Type: application/json" \
     -H "X-Token: C001" -H "X-Pwd: 123456" \
     -d '{"action":"online","place":"A101 机房"}'

# 管理员当天收入
curl -s http://127.0.0.1:8090/api/admin/income \
     -H "X-Token: ADMIN" -H "X-Pwd: admin123"
```

## 六、容器部署

```bash
docker build -t campus-card .
docker run -p 8090:8090 -v campus-data:/app/data campus-card
```

镜像内服务监听 `0.0.0.0`，端口可用环境变量 `PORT` 覆盖，数据卷挂在 `/app/data`。

## 七、数据文件（data/）

| 文件 | 字段（以 `|` 分隔） |
|---|---|
| `cards.dat` | 卡号 \| 姓名 \| 学号 \| 登录密码 \| 累计充值 \| 余额 \| 状态 \| 是否上机 \| 开始时间 \| 上机地点 |
| `<卡号>.txt` | 卡号 \| 上机日期 \| 开始时间 \| 用时(分钟) \| 费用(元) \| 状态 \| 上机地点 |
| `income.dat` | 日期 \| 金额(元)（充值与结账各记一条，用于统计当天收入） |

状态编号：`0` 正常、`1` 挂失、`2` 冻结；是否上机：`0` 未上机、`1` 上机中。

## 八、持续集成

`.github/workflows/build.yml` 在每次推送时：

1. **Linux**：编译命令行版与服务，跑一轮"学生上机 → 下机结账"和接口冒烟测试；
2. **Windows**：用 MinGW 编译两个目标（验证 `-lws2_32` 链路）。

## 九、跨平台说明

- `main.c` 用 `#ifdef _WIN32` 区分密码输入与清屏实现（`_getch`/`cls` 与 `termios`/`clear`）；
- `src/server.c` 用一层薄适配区分 WinSock 与 BSD socket（`sock_t` / `sock_init` / `sock_close`），
  业务层与前端完全跨平台。
