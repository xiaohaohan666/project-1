/* ============================================================
 *  校园一卡通 —— Windows 桌面应用（纯 C + Win32 API）
 * ------------------------------------------------------------
 *  一个 exe，双击直接进入应用，不需要 Python、不需要浏览器地址栏：
 *    1. 在后台静默启动同目录下的 server.exe（不弹控制台窗口）
 *    2. 自动寻找 Edge / Chrome，用「应用模式」打开界面
 *       —— 独立窗口、无地址栏、无标签页，就是一个桌面程序
 *    3. 本窗口显示服务状态、卡片数、费率，以及本机/局域网地址
 *    4. 通过前端心跳判断应用界面是否关闭，并据此提示
 *    5. 关闭本窗口 = 退出应用，后台服务一并停止
 *
 *  编译（MinGW-w64）：
 *    gcc -std=c11 -O2 -mwindows -fexec-charset=UTF-8 src/launcher.c \
 *        -o 校园一卡通.exe -lcomctl32 -lgdi32 -luser32 -lws2_32 -lshell32
 *  其中 -mwindows 表示 GUI 子系统：双击不会弹出黑色控制台窗口。
 * ============================================================ */
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------------- 常量与控件编号 ---------------- */
#define APP_TITLE   L"校园一卡通 —— 桌面应用"
#define SRV_EXE     "server.exe"
#define DEFAULT_PORT 8090

#define ID_LBL_TITLE   1001
#define ID_LBL_STATUS  1002
#define ID_LBL_DETAIL  1003
#define ID_LBL_LOCAL   1004
#define ID_LBL_LAN     1005
#define ID_EDIT_LOCAL  1006
#define ID_EDIT_LAN    1007
#define ID_BTN_APP     1008
#define ID_BTN_TAB     1009
#define ID_BTN_COPY    1010
#define ID_BTN_EXIT    1011
#define ID_LBL_HINT    1012
#define ID_TIMER_REFRESH 1

/* ---------------- 全局状态 ---------------- */
static HINSTANCE g_inst;
static HWND  g_hwnd;
static HFONT g_font_ui, g_font_title;
static HBRUSH g_brush_bg, g_brush_head;
static PROCESS_INFORMATION g_srv;      /* 后端服务进程 */
static int   g_srv_running;
static int   g_port = DEFAULT_PORT;
static char  g_url_local[128];
static char  g_url_lan[128];
static char  g_lan_ip[64];

/* ============================================================
 *                    小工具
 * ============================================================ */

/* 取同目录下的完整路径。name 文件名；out 输出；cap 容量 */
static void exe_dir_path(const char *name, char *out, int cap) {
    char dir[MAX_PATH];
    char *p;
    GetModuleFileNameA(NULL, dir, sizeof(dir));
    p = strrchr(dir, '\\');
    if (p) *(p + 1) = '\0';
    snprintf(out, (size_t)cap, "%s%s", dir, name);
}

/* 取本机局域网 IPv4。out 输出；cap 容量。
 * 用一个不真正发包的 UDP connect 来选路由网卡 */
static void get_lan_ip(char *out, int cap) {
    SOCKET s;
    struct sockaddr_in name;
    int len = sizeof(name);
    snprintf(out, (size_t)cap, "127.0.0.1");

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return;
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(1);
    name.sin_addr.s_addr = inet_addr("10.255.255.255");
    if (connect(s, (struct sockaddr *)&name, sizeof(name)) == 0) {
        if (getsockname(s, (struct sockaddr *)&name, &len) == 0) {
            const char *ip = inet_ntoa(name.sin_addr);
            if (ip) snprintf(out, (size_t)cap, "%s", ip);
        }
    }
    closesocket(s);
}

/* 极简 HTTP GET：向本机服务取一个 /api/... 的响应体。
 * path 请求路径；out 输出；cap 容量。成功返回 1 */
static int http_get(const char *path, char *out, int cap) {
    SOCKET s;
    struct sockaddr_in addr;
    char req[256];
    char buf[2048];
    int n, total = 0;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)g_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return 0;
    }
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
             path);
    send(s, req, (int)strlen(req), 0);

    while (total < (int)sizeof(buf) - 1) {
        n = recv(s, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    closesocket(s);

    /* 跳过响应头，只留正文 */
    {
        char *body = strstr(buf, "\r\n\r\n");
        if (!body) return 0;
        body += 4;
        snprintf(out, (size_t)cap, "%s", body);
    }
    return 1;
}

/* 从 JSON 里取整数键值。json 文本；key 键名；dflt 默认值 */
static int json_int(const char *json, const char *key, int dflt) {
    char pat[48];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p < '0' || *p > '9') {
        if (*p == '-') return -1;             /* 例如 age:-1 */
        return dflt;
    }
    return atoi(p);
}

/* 服务是否已就绪 */
static int server_ready(void) {
    char buf[512];
    if (!http_get("/api/health", buf, sizeof(buf))) return 0;
    return strstr(buf, "\"ok\":true") != NULL;
}

/* 前端心跳距今多少秒；-1 表示从未收到；-2 表示服务不可达 */
static int beat_age(void) {
    char buf[256];
    if (!http_get("/api/beat/age", buf, sizeof(buf))) return -2;
    return json_int(buf, "age", -1);
}

/* 启动后端服务（静默，无控制台窗口）。成功返回 1 */
static int start_server(void) {
    char exe[MAX_PATH];
    char cmd[MAX_PATH * 2];
    STARTUPINFOA si;
    char *argv0;

    exe_dir_path(SRV_EXE, exe, sizeof(exe));
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return 0;

    snprintf(cmd, sizeof(cmd), "\"%s\" %d web data 0.0.0.0", exe, g_port);

    memset(&g_srv, 0, sizeof(g_srv));
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    /* CREATE_NO_WINDOW：不弹出黑色控制台窗口 */
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &g_srv))
        return 0;
    g_srv_running = 1;
    /* 让新进程自己跑，我们不需要句柄 */
    CloseHandle(g_srv.hThread);
    (void)argv0;
    return 1;
}

/* 停止后端服务 */
static void stop_server(void) {
    if (!g_srv_running) return;
    if (g_srv.hProcess) {
        /* 优先温和结束；它自己会保存数据 */
        if (WaitForSingleObject(g_srv.hProcess, 0) == WAIT_TIMEOUT) {
            /* 该进程是 cmd 风格启动的，直接 TerminateProcess 不会丢数据，
             * 因为服务端每次改动都已即时写盘 */
            TerminateProcess(g_srv.hProcess, 0);
        }
        CloseHandle(g_srv.hProcess);
        g_srv.hProcess = NULL;
    }
    g_srv_running = 0;
}

/* 找可用的 Chromium 浏览器（支持 --app 应用模式）。
 * out 输出路径；cap 容量。找到返回 1 */
static int find_browser(char *out, int cap) {
    static const char *cands[] = {
        "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
        "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
        NULL
    };
    char local[MAX_PATH];
    const char *env;
    int i;

    for (i = 0; cands[i]; i++) {
        if (GetFileAttributesA(cands[i]) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, (size_t)cap, "%s", cands[i]);
            return 1;
        }
    }
    /* 再找用户目录下的安装位置 */
    env = getenv("LOCALAPPDATA");
    if (env) {
        snprintf(local, sizeof(local),
                 "%s\\Microsoft\\Edge\\Application\\msedge.exe", env);
        if (GetFileAttributesA(local) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, (size_t)cap, "%s", local);
            return 1;
        }
        snprintf(local, sizeof(local),
                 "%s\\Google\\Chrome\\Application\\chrome.exe", env);
        if (GetFileAttributesA(local) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, (size_t)cap, "%s", local);
            return 1;
        }
    }
    return 0;
}

/* 打开界面。app_mode 为 1 时用应用模式（独立窗口，无地址栏） */
static void open_ui(int app_mode) {
    char browser[MAX_PATH];
    char cmd[MAX_PATH * 3];
    char profile[MAX_PATH];
    const char *tmp;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    if (app_mode && find_browser(browser, sizeof(browser))) {
        tmp = getenv("TEMP");
        snprintf(profile, sizeof(profile), "%s\\campus-app-profile",
                 tmp ? tmp : ".");
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" --app=%s --window-size=1180,820 "
                 "--user-data-dir=\"%s\" --no-first-run "
                 "--no-default-browser-check",
                 browser, g_url_local, profile);
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }
    /* 退化为默认浏览器（普通标签页） */
    ShellExecuteA(NULL, "open", g_url_local, NULL, NULL, SW_SHOWNORMAL);
}

/* 复制文本到剪贴板 */
static void copy_to_clipboard(const char *text) {
    size_t len = strlen(text) + 1;
    HGLOBAL h;
    char *p;
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, len);
    if (h) {
        p = (char *)GlobalLock(h);
        if (p) {
            memcpy(p, text, len);
            GlobalUnlock(h);
            SetClipboardData(CF_TEXT, h);
        }
    }
    CloseClipboard();
}

/* ============================================================
 *                    窗口绘制
 * ============================================================ */
static HFONT make_font(int size, int bold) {
    return CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                       0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

static void create_controls(void) {
    /* 顶部标题条由 WM_PAINT 自绘，这里创建其余控件 */
    CreateWindowW(L"STATIC", L"正在启动服务 ...",
                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                  18, 84, 500, 24, g_hwnd, (HMENU)ID_LBL_STATUS, g_inst, NULL);
    CreateWindowW(L"STATIC", L"",
                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                  18, 110, 510, 20, g_hwnd, (HMENU)ID_LBL_DETAIL, g_inst, NULL);

    CreateWindowW(L"STATIC", L"本机打开",
                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                  18, 146, 70, 22, g_hwnd, (HMENU)ID_LBL_LOCAL, g_inst, NULL);
    CreateWindowW(L"EDIT", L"",
                  WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL,
                  92, 144, 434, 24, g_hwnd, (HMENU)ID_EDIT_LOCAL, g_inst, NULL);

    CreateWindowW(L"STATIC", L"手机访问",
                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                  18, 176, 70, 22, g_hwnd, (HMENU)ID_LBL_LAN, g_inst, NULL);
    CreateWindowW(L"EDIT", L"",
                  WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL,
                  92, 174, 434, 24, g_hwnd, (HMENU)ID_EDIT_LAN, g_inst, NULL);

    CreateWindowW(L"STATIC", L"手机与本电脑连同一个 WiFi 时可用",
                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                  92, 200, 400, 18, g_hwnd, (HMENU)ID_LBL_HINT, g_inst, NULL);

    CreateWindowW(L"BUTTON", L"打开界面",
                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  18, 232, 150, 38, g_hwnd, (HMENU)ID_BTN_APP, g_inst, NULL);
    CreateWindowW(L"BUTTON", L"浏览器标签页",
                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  176, 232, 130, 38, g_hwnd, (HMENU)ID_BTN_TAB, g_inst, NULL);
    CreateWindowW(L"BUTTON", L"复制手机地址",
                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  314, 232, 120, 38, g_hwnd, (HMENU)ID_BTN_COPY, g_inst, NULL);
    CreateWindowW(L"BUTTON", L"退出",
                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  442, 232, 84, 38, g_hwnd, (HMENU)ID_BTN_EXIT, g_inst, NULL);

    /* 统一下发字体 */
    {
        HWND kids[] = {
            GetDlgItem(g_hwnd, ID_LBL_STATUS), GetDlgItem(g_hwnd, ID_LBL_DETAIL),
            GetDlgItem(g_hwnd, ID_LBL_LOCAL),  GetDlgItem(g_hwnd, ID_LBL_LAN),
            GetDlgItem(g_hwnd, ID_EDIT_LOCAL), GetDlgItem(g_hwnd, ID_EDIT_LAN),
            GetDlgItem(g_hwnd, ID_LBL_HINT),   GetDlgItem(g_hwnd, ID_BTN_APP),
            GetDlgItem(g_hwnd, ID_BTN_TAB),    GetDlgItem(g_hwnd, ID_BTN_COPY),
            GetDlgItem(g_hwnd, ID_BTN_EXIT)
        };
        for (int i = 0; i < (int)(sizeof(kids) / sizeof(kids[0])); i++) {
            if (kids[i]) SendMessageW(kids[i], WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        }
    }
    SendMessageW(GetDlgItem(g_hwnd, ID_LBL_STATUS), WM_SETFONT,
                 (WPARAM)make_font(15, 1), TRUE);

    SetWindowTextA(GetDlgItem(g_hwnd, ID_EDIT_LOCAL), g_url_local);
    SetWindowTextA(GetDlgItem(g_hwnd, ID_EDIT_LAN), g_url_lan);
}

/* 刷新状态：服务是否在线、卡片数、界面是否打开 */
static void refresh_status(void) {
    char buf[1024];
    int cards, rate, age;
    wchar_t wtext[512];

    if (!server_ready()) {
        SetWindowTextW(GetDlgItem(g_hwnd, ID_LBL_STATUS), L"服务未响应");
        SetWindowTextW(GetDlgItem(g_hwnd, ID_LBL_DETAIL), L"");
        return;
    }
    if (!http_get("/api/health", buf, sizeof(buf))) return;
    cards = json_int(buf, "cards", 0);
    rate  = json_int(buf, "rate", 0);
    age   = beat_age();

    SetWindowTextW(GetDlgItem(g_hwnd, ID_LBL_STATUS), L"服务运行中");
    if (age <= 14 && age >= 0)
        swprintf(wtext, 512, L"卡片 %d 张 · 费率 %d.00 元/小时 · 应用界面在线",
                 cards, rate);
    else if (age == -1)
        swprintf(wtext, 512, L"卡片 %d 张 · 费率 %d.00 元/小时 · 界面尚未打开",
                 cards, rate);
    else
        swprintf(wtext, 512, L"卡片 %d 张 · 费率 %d.00 元/小时 · 界面已关闭",
                 cards, rate);
    SetWindowTextW(GetDlgItem(g_hwnd, ID_LBL_DETAIL), wtext);
}

/* ============================================================
 *                    窗口过程
 * ============================================================ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            create_controls();
            SetTimer(hwnd, ID_TIMER_REFRESH, 2000, NULL);
            return 0;

        case WM_TIMER:
            if (wp == ID_TIMER_REFRESH) refresh_status();
            return 0;

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            HWND ctl = (HWND)lp;
            int id = GetDlgCtrlID(ctl);
            SetBkMode(dc, TRANSPARENT);
            if (id == ID_LBL_STATUS) {
                SetTextColor(dc, RGB(5, 150, 105));      /* 绿色：服务正常 */
                return (LRESULT)g_brush_bg;
            }
            if (id == ID_LBL_HINT) {
                SetTextColor(dc, RGB(107, 114, 128));
                return (LRESULT)g_brush_bg;
            }
            SetTextColor(dc, RGB(55, 65, 81));
            return (LRESULT)g_brush_bg;
        }

        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, RGB(255, 255, 255));
            SetTextColor(dc, RGB(31, 41, 55));
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            /* 顶部蓝色标题条 */
            GetClientRect(hwnd, &rc);
            rc.bottom = 64;
            FillRect(dc, &rc, g_brush_head);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            SelectObject(dc, g_font_title);
            TextOutW(dc, 18, 12, L"校园一卡通", 5);

            SelectObject(dc, g_font_ui);
            SetTextColor(dc, RGB(219, 234, 254));
            TextOutW(dc, 122, 26, L"消费 · 机房上机 · 门禁考勤", 13);

            /* 底部账号提示 */
            SetTextColor(dc, RGB(107, 114, 128));
            TextOutW(dc, 18, 288,
                     L"学生 C001 / C002 / C003 · 登录密码 123456 · 交易密码 010101 · 管理员 admin123",
                     46);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_BTN_APP:  open_ui(1); break;
                case ID_BTN_TAB:  open_ui(0); break;
                case ID_BTN_COPY:
                    copy_to_clipboard(g_url_lan);
                    MessageBoxW(hwnd, L"手机访问地址已复制，可粘贴发送到手机。",
                                L"已复制", MB_OK | MB_ICONINFORMATION);
                    break;
                case ID_BTN_EXIT:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER_REFRESH);
            stop_server();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ============================================================
 *                          入口
 * ============================================================ */

/* 自检：不创建窗口，只验证各功能函数是否正常工作。
 * 用控制台版编译后运行 launcher.exe --selftest 即可看到结果。 */
static int self_test(void) {
    int i, ok;
    char buf[1024];

    printf("=== 校园一卡通桌面应用 自检 ===\n\n");

    printf("[1] 取本机局域网 IP\n");
    get_lan_ip(g_lan_ip, sizeof(g_lan_ip));
    printf("    -> %s\n", g_lan_ip);

    printf("\n[2] 查找 Chromium 浏览器（用于应用模式）\n");
    {
        char br[MAX_PATH];
        ok = find_browser(br, sizeof(br));
        printf("    -> %s\n", ok ? br : "未找到（将退化为默认浏览器）");
    }

    printf("\n[3] 启动后端服务 %s\n", SRV_EXE);
    if (!start_server()) {
        printf("    失败：同目录下没有 %s\n", SRV_EXE);
        return 1;
    }
    printf("    已创建进程，PID=%lu\n", (unsigned long)g_srv.dwProcessId);

    printf("\n[4] 等待服务就绪\n");
    for (i = 0; i < 80; i++) {
        if (server_ready()) break;
        Sleep(250);
    }
    if (i >= 80) {
        printf("    失败：20 秒内未就绪\n");
        stop_server();
        return 1;
    }
    printf("    就绪，耗时约 %.1f 秒\n", i * 0.25);

    printf("\n[5] 读取 /api/health\n");
    if (http_get("/api/health", buf, sizeof(buf))) {
        printf("    卡片数=%d  机房费率=%d  发卡上限=%d\n",
               json_int(buf, "cards", -1), json_int(buf, "rate", -1),
               json_int(buf, "cardMax", -1));
    } else {
        printf("    失败\n");
    }

    printf("\n[6] 心跳检测（模拟界面打开 / 关闭）\n");
    printf("    初始 age = %d  (-1 表示界面尚未打开)\n", beat_age());
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        char req[128];
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)g_port);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (s != INVALID_SOCKET && connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
            snprintf(req, sizeof(req),
                     "GET /api/beat HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n");
            send(s, req, (int)strlen(req), 0);
            while (recv(s, buf, sizeof(buf), 0) > 0) ;
        }
        if (s != INVALID_SOCKET) closesocket(s);
    }
    printf("    模拟一次心跳后 age = %d\n", beat_age());
    Sleep(2000);
    printf("    等 2 秒后 age = %d  (递增说明可据此判断界面是否关闭)\n", beat_age());

    printf("\n[7] 停止服务并确认无残留\n");
    stop_server();
    Sleep(800);
    printf("    停止后 server_ready() = %d  (0 表示已停止)\n", server_ready());

    printf("\n=== 自检完成 ===\n");
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
    WNDCLASSEXW wc;
    MSG msg;
    int waited;

    (void)prev; (void)show;
    g_inst = inst;

    /* 允许用命令行参数指定端口：校园一卡通.exe 9000 */
    if (cmdline && *cmdline) {
        int p = atoi(cmdline);
        if (p > 0 && p < 65536) g_port = p;
    }
    snprintf(g_url_local, sizeof(g_url_local), "http://127.0.0.1:%d/", g_port);    get_lan_ip(g_lan_ip, sizeof(g_lan_ip));
    snprintf(g_url_lan, sizeof(g_url_lan), "http://%s:%d/", g_lan_ip, g_port);

    /* 网络库（取本机 IP 与访问本机服务用） */
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    /* 通用控件（美化按钮） */
    InitCommonControls();

    g_brush_bg   = CreateSolidBrush(RGB(244, 246, 250));
    g_brush_head = CreateSolidBrush(RGB(37, 99, 235));
    g_font_ui    = make_font(14, 0);
    g_font_title = make_font(22, 1);

    /* 自检模式（用控制台版编译后可运行：launcher.exe --selftest） */
    if (cmdline && strstr(cmdline, "--selftest")) {
        int rc = self_test();
        DeleteObject(g_font_ui);
        DeleteObject(g_font_title);
        DeleteObject(g_brush_bg);
        DeleteObject(g_brush_head);
        WSACleanup();
        return rc;
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_brush_bg;
    wc.lpszClassName = L"CampusCardLauncher";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"窗口类注册失败", L"错误", MB_ICONERROR);
        return 1;
    }

    /* 居中显示 */
    {
        int w = 560, h = 330;
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE,
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 (sw - w) / 2, (sh - h) / 3, w, h,
                                 NULL, NULL, inst, NULL);
    }
    if (!g_hwnd) {
        MessageBoxW(NULL, L"创建窗口失败", L"错误", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    /* 启动后端并等待就绪（最多约 20 秒；server.exe 缺失时给出提示） */
    SetWindowTextW(GetDlgItem(g_hwnd, ID_LBL_STATUS), L"正在启动服务 ...");
    if (!start_server()) {
        MessageBoxW(g_hwnd,
                    L"同目录下没有找到 server.exe。\n\n"
                    L"请先用 gcc 编译后端：\n"
                    L"  gcc -std=c11 -O2 -fexec-charset=UTF-8 -Isrc "
                    L"src/server.c src/campus.c -o server.exe -lws2_32",
                    L"缺少后端程序", MB_ICONERROR);
        DestroyWindow(g_hwnd);
        return 1;
    }
    for (waited = 0; waited < 80; waited++) {
        if (server_ready()) break;
        Sleep(250);
    }
    if (waited >= 80) {
        SetWindowTextW(GetDlgItem(g_hwnd, ID_LBL_STATUS), L"服务启动失败");
        MessageBoxW(g_hwnd,
                    L"后端在 20 秒内没有就绪。\n"
                    L"可能端口被占用，或安全软件拦截了进程启动。",
                    L"启动失败", MB_ICONWARNING);
    } else {
        refresh_status();
        open_ui(1);                     /* 自动以应用模式打开界面 */
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* 收尾：确保后端不会残留 */
    stop_server();
    DeleteObject(g_font_ui);
    DeleteObject(g_font_title);
    DeleteObject(g_brush_bg);
    DeleteObject(g_brush_head);
    WSACleanup();
    return 0;
}
