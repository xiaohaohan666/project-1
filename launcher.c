/* ============================================================
 *  校园一卡通 —— Windows 桌面应用（纯 C + Win32 API）
 * ------------------------------------------------------------
 *  一个 exe，双击直接进入应用，不需要 Python、不需要浏览器地址栏：
 *    1. 在后台静默启动同目录下的 server.exe（不弹控制台窗口）
 *    2. 自动寻找 Edge / Chrome，用「应用模式」打开界面
 *       —— 独立窗口、无地址栏、无标签页，就是一个桌面程序
 *    3. 本窗口显示服务状态、卡片数、费率，以及本机/局域网地址
 *    4. 通过前端心跳判断应用界面是否关闭
 *    5. 关闭本窗口 = 退出应用，后台服务一并停止
 *
 *  界面适配说明（重要）
 *  所有控件位置都在 layout() 里按当前客户区尺寸**按比例**计算，
 *  并在 WM_SIZE 时重排。早期的版本用硬编码像素坐标，窗口一旦
 *  缩放或系统缩放比不同，文字就会互相重叠、控件跑到窗口外，
 *  看起来像"界面没做适配"。现已全部改为响应式。
 *
 *  编译（MinGW-w64）：
 *    windres launcher.rc -o launcher_res.o --output-format=coff
 *    gcc -std=c11 -O2 -mwindows -fexec-charset=UTF-8 launcher.c \
 *        launcher_res.o -o 校园一卡通.exe \
 *        -lcomctl32 -lgdi32 -luser32 -lws2_32 -lshell32
 *  其中 -mwindows 表示 GUI 子系统：双击不会弹出黑色控制台窗口。
 * ============================================================ */
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>          /* InitCommonControls：美化按钮外观 */
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------------- 常量与控件编号 ---------------- */
#define APP_TITLE    L"校园一卡通"
#define SRV_EXE      "server.exe"
#define DEFAULT_PORT 8090

/* 设计基准尺寸：layout() 以这套坐标为准，再按实际客户区等比缩放 */
#define BASE_W 640
#define BASE_H 560

#define ID_LBL_STATUS  1001
#define ID_LBL_DETAIL  1002
#define ID_LBL_LOCAL   1003
#define ID_LBL_LAN     1004
#define ID_EDIT_LOCAL  1005
#define ID_EDIT_LAN    1006
#define ID_LBL_HINT    1007
#define ID_BTN_APP     1008
#define ID_BTN_TAB     1009
#define ID_BTN_COPY    1010
#define ID_BTN_EXIT    1011
#define ID_TIMER_REFRESH 1

/* ---------------- 全局状态 ---------------- */
static HINSTANCE g_inst;
static HWND  g_hwnd;
static HFONT g_font_ui, g_font_title, g_font_mono;
static HBRUSH g_brush_bg, g_brush_head, g_brush_card;
static int   g_srv_running;
static int   g_port = DEFAULT_PORT;
static char  g_url_local[128];
static char  g_url_lan[128];
static char  g_lan_ip[64];
static int   g_app_window_opened;

/* ============================================================
 *                    小工具
 * ============================================================ */

/* 取同目录下的完整路径 */
static void exe_dir_path(const char *name, char *out, int cap) {
    char dir[MAX_PATH];
    char *p;
    GetModuleFileNameA(NULL, dir, sizeof(dir));
    p = strrchr(dir, '\\');
    if (p) *(p + 1) = '\0';
    snprintf(out, (size_t)cap, "%s%s", dir, name);
}

/* 取本机局域网 IPv4（用一个不真正发包的 UDP connect 选路由网卡） */
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

/* 极简 HTTP GET：向本机服务取一个 /api/... 的响应体。成功返回 1 */
static int http_get(const char *path, char *out, int cap) {
    SOCKET s;
    struct sockaddr_in addr;
    char req[256];
    char buf[4096];
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

    {
        char *body = strstr(buf, "\r\n\r\n");
        if (!body) return 0;
        body += 4;
        snprintf(out, (size_t)cap, "%s", body);
    }
    return 1;
}

/* 从 JSON 里取整数键值 */
static int json_int(const char *json, const char *key, int dflt) {
    char pat[48];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p == '-') return -1;
    if (*p < '0' || *p > '9') return dflt;
    return atoi(p);
}

static int server_ready(void) {
    char buf[512];
    if (!http_get("/api/health", buf, sizeof(buf))) return 0;
    return strstr(buf, "\"ok\":true") != NULL;
}

/* 前端心跳距今多少秒；-1 从未收到；-2 服务不可达 */
static int beat_age(void) {
    char buf[256];
    if (!http_get("/api/beat/age", buf, sizeof(buf))) return -2;
    return json_int(buf, "age", -1);
}

/* 启动后端服务（静默，无控制台窗口） */
static PROCESS_INFORMATION g_srv;

static int start_server(void) {
    char exe[MAX_PATH];
    char cmd[MAX_PATH * 2];
    STARTUPINFOA si;

    exe_dir_path(SRV_EXE, exe, sizeof(exe));
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return 0;

    snprintf(cmd, sizeof(cmd), "\"%s\" %d web data 0.0.0.0", exe, g_port);
    memset(&g_srv, 0, sizeof(g_srv));
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &g_srv))
        return 0;
    g_srv_running = 1;
    CloseHandle(g_srv.hThread);
    return 1;
}

static void stop_server(void) {
    if (!g_srv_running) return;
    if (g_srv.hProcess) {
        if (WaitForSingleObject(g_srv.hProcess, 0) == WAIT_TIMEOUT)
            TerminateProcess(g_srv.hProcess, 0);
        CloseHandle(g_srv.hProcess);
        g_srv.hProcess = NULL;
    }
    g_srv_running = 0;
}

/* 找可用的 Chromium 浏览器（支持 --app 应用模式） */
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
                 "\"%s\" --app=%s --window-size=1240,860 "
                 "--user-data-dir=\"%s\" --no-first-run "
                 "--no-default-browser-check",
                 browser, g_url_local, profile);
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                           NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            g_app_window_opened = 1;
            return;
        }
    }
    ShellExecuteA(NULL, "open", g_url_local, NULL, NULL, SW_SHOWNORMAL);
}

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
 *                    界面构建与响应式布局
 * ============================================================ */
static HFONT make_font(int size, int bold, const wchar_t *face) {
    return CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                       0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style,
               int id) {
    return CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style,
                         0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id,
                         g_inst, NULL);
}

static void create_controls(void) {
    mk(L"STATIC", L"服务状态",       SS_LEFT, ID_LBL_STATUS);
    mk(L"STATIC", L"",              SS_LEFT, ID_LBL_DETAIL);
    mk(L"STATIC", L"本机打开",       SS_LEFT, ID_LBL_LOCAL);
    mk(L"EDIT",   L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, ID_EDIT_LOCAL);
    mk(L"STATIC", L"手机访问",       SS_LEFT, ID_LBL_LAN);
    mk(L"EDIT",   L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, ID_EDIT_LAN);
    mk(L"STATIC", L"手机与本电脑连同一个 WiFi 时可用", SS_LEFT, ID_LBL_HINT);
    mk(L"BUTTON", L"打开界面",       BS_PUSHBUTTON, ID_BTN_APP);
    mk(L"BUTTON", L"浏览器标签页",    BS_PUSHBUTTON, ID_BTN_TAB);
    mk(L"BUTTON", L"复制手机地址",    BS_PUSHBUTTON, ID_BTN_COPY);
    mk(L"BUTTON", L"退出",           BS_PUSHBUTTON, ID_BTN_EXIT);

    /* 下发字体 */
    for (int id = ID_LBL_STATUS; id <= ID_BTN_EXIT; id++) {
        HWND c = GetDlgItem(g_hwnd, id);
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    }
    SendMessageW(GetDlgItem(g_hwnd, ID_LBL_STATUS), WM_SETFONT,
                 (WPARAM)g_font_title, TRUE);
    SendMessageW(GetDlgItem(g_hwnd, ID_LBL_HINT), WM_SETFONT,
                 (WPARAM)make_font(12, 0, L"Microsoft YaHei UI"), TRUE);
    SendMessageW(GetDlgItem(g_hwnd, ID_EDIT_LOCAL), WM_SETFONT,
                 (WPARAM)g_font_mono, TRUE);
    SendMessageW(GetDlgItem(g_hwnd, ID_EDIT_LAN), WM_SETFONT,
                 (WPARAM)g_font_mono, TRUE);

    SetWindowTextA(GetDlgItem(g_hwnd, ID_EDIT_LOCAL), g_url_local);
    SetWindowTextA(GetDlgItem(g_hwnd, ID_EDIT_LAN), g_url_lan);
}

/* 控件几何：由客户区尺寸算出每个控件的位置与大小。
 *
 * 单独抽成纯函数（不碰任何窗口状态），这样 --layoutcheck 能在没有
 * 图形界面的环境下验证「各种窗口尺寸下控件是否都在可见区域内、
 * 是否互相重叠」。早期版本用硬编码像素坐标，窗口一变就叠字、
 * 控件跑到窗口外，正是靠这个检查抓出来的。 */
typedef struct { int x, y, w, h; } Box;

typedef struct {
    Box status, detail, lbl_local, edit_local, lbl_lan, edit_lan;
    Box hint, btn_app, btn_tab, btn_copy, btn_exit;
    Box header;                    /* 顶部标题带（自绘） */
    int pad;
} Layout;

static Layout compute_layout(int W, int H) {
    Layout L;
    double s;
    int pad, head_h, row_h, gap, btn_h, y;
    int content_w, label_w, edit_x, edit_w;

    memset(&L, 0, sizeof(L));
    if (W <= 0 || H <= 0) return L;

    /* 缩放系数只按宽度算。
     *
     * 早先用 min(宽比, 高比)：窗口一拉宽，宽比变大而高比不变，系数被
     * 高比限制住，可控件是按宽度排的，内容就会溢出到窗口底部之外——
     * 表现正是「界面没做窗口适配」。按宽度缩放后，内容高度随宽度等比
     * 增长，纵向多余的留白由吸底的按钮行自然吸收。 */
    s = (double)W / BASE_W;
    if (s > 2.0) s = 2.0;                 /* 限制过大放大 */
    if (s < 0.62) s = 0.62;               /* 再小字号就没法看了 */

    pad    = (int)(24 * s);
    head_h = (int)(56 * s);
    row_h  = (int)(30 * s);
    gap    = (int)(10 * s);
    btn_h  = (int)(42 * s);

    content_w = W - pad * 2;
    label_w   = (int)(78 * s);
    edit_x    = pad + label_w + gap;
    edit_w    = W - edit_x - pad;
    if (edit_w < 60) edit_w = 60;

    L.pad = pad;
    L.header.x = 0; L.header.y = 0; L.header.w = W; L.header.h = head_h;

    y = head_h + (int)(18 * s);
    L.status.x = pad; L.status.y = y;
    L.status.w = content_w; L.status.h = (int)(26 * s);

    y += (int)(30 * s);
    L.detail.x = pad; L.detail.y = y;
    L.detail.w = content_w; L.detail.h = (int)(22 * s);

    y += (int)(36 * s);
    L.lbl_local.x = pad;  L.lbl_local.y = y + (int)(5 * s);
    L.lbl_local.w = label_w; L.lbl_local.h = row_h;
    L.edit_local.x = edit_x; L.edit_local.y = y;
    L.edit_local.w = edit_w; L.edit_local.h = row_h;

    y += row_h + gap;
    L.lbl_lan.x = pad;  L.lbl_lan.y = y + (int)(5 * s);
    L.lbl_lan.w = label_w; L.lbl_lan.h = row_h;
    L.edit_lan.x = edit_x; L.edit_lan.y = y;
    L.edit_lan.w = edit_w; L.edit_lan.h = row_h;

    y += row_h + (int)(4 * s);
    L.hint.x = edit_x; L.hint.y = y;
    L.hint.w = edit_w; L.hint.h = (int)(18 * s);

    /* 按钮行吸底，宽度按 4 等分自适应 */
    y = H - pad - btn_h;
    {
        int bw = (content_w - gap * 3) / 4;
        if (bw < 40) bw = 40;
        L.btn_app.x  = pad;                  L.btn_app.y  = y;
        L.btn_app.w  = bw;                   L.btn_app.h  = btn_h;
        L.btn_tab.x  = pad + (bw + gap);     L.btn_tab.y  = y;
        L.btn_tab.w  = bw;                   L.btn_tab.h  = btn_h;
        L.btn_copy.x = pad + (bw + gap) * 2; L.btn_copy.y = y;
        L.btn_copy.w = bw;                   L.btn_copy.h = btn_h;
        L.btn_exit.x = pad + (bw + gap) * 3; L.btn_exit.y = y;
        L.btn_exit.w = bw;                   L.btn_exit.h = btn_h;
    }
    return L;
}

/* 响应式布局：把计算结果应用到真实控件上 */
static void layout(void) {
    RECT rc;
    int W, H;
    Layout L;

    if (!g_hwnd) return;
    GetClientRect(g_hwnd, &rc);
    W = rc.right - rc.left;
    H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    L = compute_layout(W, H);
    if (L.header.h == 0) return;

#define MOVE(id, box) MoveWindow(GetDlgItem(g_hwnd, id), \
    (box).x, (box).y, (box).w, (box).h, TRUE)
    MOVE(ID_LBL_STATUS, L.status);
    MOVE(ID_LBL_DETAIL, L.detail);
    MOVE(ID_LBL_LOCAL,  L.lbl_local);
    MOVE(ID_EDIT_LOCAL, L.edit_local);
    MOVE(ID_LBL_LAN,    L.lbl_lan);
    MOVE(ID_EDIT_LAN,   L.edit_lan);
    MOVE(ID_LBL_HINT,   L.hint);
    MOVE(ID_BTN_APP,    L.btn_app);
    MOVE(ID_BTN_TAB,    L.btn_tab);
    MOVE(ID_BTN_COPY,   L.btn_copy);
    MOVE(ID_BTN_EXIT,   L.btn_exit);
#undef MOVE

    /* 布局变化后重绘标题带与提示文字 */
    InvalidateRect(g_hwnd, NULL, TRUE);
}


/* 刷新状态 */
static void refresh_status(void) {
    char buf[1024], name[64];
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
    if (age >= 0 && age <= 14)
        swprintf(wtext, 512, L"%d 张卡 · %d.00 元/小时 · 应用界面在线",
                 cards, rate);
    else if (age < -1 || age == -1)
        swprintf(wtext, 512, L"%d 张卡 · %d.00 元/小时 · 界面尚未打开",
                 cards, rate);
    else
        swprintf(wtext, 512, L"%d 张卡 · %d.00 元/小时 · 界面已关闭",
                 cards, rate);
    SetWindowTextW(GetDlgItem(g_hwnd, ID_LBL_DETAIL), wtext);

    /* 同时刷新局域网地址（网络切换后 IP 会变） */
    {
        char ip[64], url[128];
        get_lan_ip(ip, sizeof(ip));
        snprintf(url, sizeof(url), "http://%s:%d/", ip, g_port);
        if (strcmp(url, g_url_lan)) {
            snprintf(g_lan_ip, sizeof(g_lan_ip), "%s", ip);
            snprintf(g_url_lan, sizeof(g_url_lan), "%s", url);
            SetWindowTextA(GetDlgItem(g_hwnd, ID_EDIT_LAN), g_url_lan);
        }
    }
    (void)name;
}

/* ============================================================
 *                    窗口过程
 * ============================================================ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            create_controls();
            layout();
            SetTimer(hwnd, ID_TIMER_REFRESH, 2000, NULL);
            return 0;

        case WM_SIZE:
            layout();
            return 0;

        case WM_GETMINMAXINFO: {
            /* 设最小尺寸：太小会让比例缩放失去意义 */
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            mmi->ptMinTrackSize.x = 520;
            mmi->ptMinTrackSize.y = 480;
            return 0;
        }

        case WM_TIMER:
            if (wp == ID_TIMER_REFRESH) refresh_status();
            return 0;

        case WM_ERASEBKGND: {
            /* 用背景色填满，避免窗口缩放时闪白 */
            HDC dc = (HDC)wp;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, g_brush_bg);
            return 1;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            int id = GetDlgCtrlID((HWND)lp);
            SetBkMode(dc, TRANSPARENT);
            if (id == ID_LBL_STATUS) {
                SetTextColor(dc, RGB(5, 150, 105));       /* 绿色：运行中 */
                return (LRESULT)g_brush_bg;
            }
            if (id == ID_LBL_HINT) {
                SetTextColor(dc, RGB(107, 114, 128));
                return (LRESULT)g_brush_bg;
            }
            if (id == ID_LBL_DETAIL) {
                SetTextColor(dc, RGB(55, 65, 81));
                return (LRESULT)g_brush_bg;
            }
            SetTextColor(dc, RGB(31, 41, 55));
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
            double s;
            int W, H, pad, head_h, base;
            wchar_t line[256];

            GetClientRect(hwnd, &rc);
            W = rc.right - rc.left;
            H = rc.bottom - rc.top;
            s = (double)W / BASE_W;
            if (s > 2.0) s = 2.0;
            if (s < 0.62) s = 0.62;
            pad = (int)(24 * s);
            head_h = (int)(56 * s);
            base = (int)(16 * s);          /* 基准字号 */

            /* 顶部标题带 */
            {
                RECT head = rc;
                head.bottom = head_h;
                FillRect(dc, &head, g_brush_head);
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            {
                HFONT old = (HFONT)SelectObject(dc, g_font_title);
                TextOutW(dc, pad, (int)(10 * s), APP_TITLE, 5);
                SelectObject(dc, old);
            }
            /* 副标题换到标题右侧，避免与主标题上下重叠 */
            SetTextColor(dc, RGB(191, 219, 254));
            {
                HFONT sub = make_font((int)(13 * s), 0, L"Microsoft YaHei UI");
                HFONT old = (HFONT)SelectObject(dc, sub);
                SIZE sz;
                const wchar_t *sub_txt = L"消费 · 机房上机 · 门禁考勤";
                GetTextExtentPoint32W(dc, APP_TITLE, 5, &sz);
                /* 主标题实际宽度 + 间距；若右侧放不下就不画 */
                if (pad + sz.cx + (int)(16 * s) + 200 * s < W)
                    TextOutW(dc, pad + sz.cx + (int)(16 * s),
                             (int)(18 * s), sub_txt, (int)wcslen(sub_txt));
                SelectObject(dc, old);
                DeleteObject(sub);
            }

            /* 底部账号提示（放在按钮行上方，避免与按钮重叠） */
            {
                HFONT f = make_font((int)(12 * s), 0, L"Microsoft YaHei UI");
                HFONT old = (HFONT)SelectObject(dc, f);
                int btn_h = (int)(42 * s);
                int ty = H - pad - btn_h - (int)(26 * s);
                SetTextColor(dc, RGB(107, 114, 128));
                swprintf(line, 256,
                         L"学生 C001 / C002 / C003 · 登录密码 123456 · "
                         L"交易密码 010101 · 管理员 admin123");
                if (ty > head_h + base)
                    TextOutW(dc, pad, ty, line, (int)wcslen(line));
                SelectObject(dc, old);
                DeleteObject(f);
            }
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
 *                          自检
 * ============================================================ */
static int self_test(void) {
    int i, ok;
    char buf[1024];
    char br[MAX_PATH];

    printf("=== 校园一卡通桌面应用 自检 ===\n\n");

    printf("[1] 取本机局域网 IP\n");
    get_lan_ip(g_lan_ip, sizeof(g_lan_ip));
    printf("    -> %s\n", g_lan_ip);

    printf("\n[2] 查找 Chromium 浏览器（用于应用模式）\n");
    ok = find_browser(br, sizeof(br));
    printf("    -> %s\n", ok ? br : "未找到（将退化为默认浏览器）");

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
        if (s != INVALID_SOCKET &&
            connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
            snprintf(req, sizeof(req),
                     "GET /api/beat HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n");
            send(s, req, (int)strlen(req), 0);
            while (recv(s, buf, sizeof(buf), 0) > 0) ;
        }
        if (s != INVALID_SOCKET) closesocket(s);
    }
    printf("    模拟一次心跳后 age = %d\n", beat_age());
    Sleep(2000);
    printf("    等 2 秒后 age = %d  (递增说明可据此判断界面是否关闭)\n",
           beat_age());

    printf("\n[7] 停止服务并确认无残留\n");
    stop_server();
    Sleep(800);
    printf("    停止后 server_ready() = %d  (0 表示已停止)\n", server_ready());

    printf("\n=== 自检完成 ===\n");
    return 0;
}


/* 布局自检：把若干典型窗口尺寸下的控件几何算出来并检查。
 * 检查项：所有控件都在客户区内；四个按钮互不重叠。
 * 这样在没有图形界面的环境下也能确认「窗口适配」确实成立。 */
static int layout_check(void) {
    static const struct { int w, h; const char *note; } cases[] = {
        { 520, 480,  "最小窗口" },
        { 660, 600,  "默认窗口" },
        { 900, 620,  "拉宽" },
        { 1200, 900, "大窗口" },
        { 1600, 1000, "全屏级" },
        { 420, 420,  "过小（受最小尺寸限制）" },
    };
    int i, bad = 0;

    printf("=== 窗口适配自检 ===\n\n");
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        int W = cases[i].w, H = cases[i].h;
        Layout L = compute_layout(W, H);
        Box boxes[11];
        static const char *names[11] = {
            "状态", "详情", "本机标签", "本机输入框", "手机标签", "手机输入框",
            "提示", "按钮1", "按钮2", "按钮3", "按钮4"
        };
        int j, overflow = 0, overlap = 0;

        boxes[0]  = L.status;      boxes[1]  = L.detail;
        boxes[2]  = L.lbl_local;   boxes[3]  = L.edit_local;
        boxes[4]  = L.lbl_lan;     boxes[5]  = L.edit_lan;
        boxes[6]  = L.hint;
        boxes[7]  = L.btn_app;     boxes[8]  = L.btn_tab;
        boxes[9]  = L.btn_copy;    boxes[10] = L.btn_exit;

        for (j = 0; j < 11; j++) {
            if (boxes[j].x < 0 || boxes[j].y < 0 ||
                boxes[j].x + boxes[j].w > W ||
                boxes[j].y + boxes[j].h > H) {
                overflow = 1;
                printf("  [越界] %s: (%d,%d) %dx%d  客户区 %dx%d\n",
                       names[j], boxes[j].x, boxes[j].y,
                       boxes[j].w, boxes[j].h, W, H);
            }
        }
        for (j = 7; j < 11 && !overlap; j++) {
            int k2;
            for (k2 = j + 1; k2 < 11; k2++) {
                Box a = boxes[j], b = boxes[k2];
                if (a.x < b.x + b.w && b.x < a.x + a.w &&
                    a.y < b.y + b.h && b.y < a.y + a.h) {
                    overlap = 1;
                    printf("  [重叠] %s 与 %s\n", names[j], names[k2]);
                    break;
                }
            }
        }
        printf("  [%s] %-22s %4dx%-4d  按钮宽 %3d  状态行 y=%3d  按钮行 y=%3d\n",
               (overflow || overlap) ? "FAIL" : "OK ",
               cases[i].note, W, H, L.btn_app.w, L.status.y, L.btn_app.y);
        if (overflow || overlap) bad++;
    }
    printf("\n=== %s ===\n", bad ? "存在不适配的尺寸" : "所有尺寸都适配");
    return bad;
}

/* ============================================================
 *                          入口
 * ============================================================ */
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
    snprintf(g_url_local, sizeof(g_url_local), "http://127.0.0.1:%d/", g_port);
    get_lan_ip(g_lan_ip, sizeof(g_lan_ip));
    snprintf(g_url_lan, sizeof(g_url_lan), "http://%s:%d/", g_lan_ip, g_port);

    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    InitCommonControls();

    g_brush_bg   = CreateSolidBrush(RGB(244, 246, 250));
    g_brush_head = CreateSolidBrush(RGB(37, 99, 235));
    g_brush_card = CreateSolidBrush(RGB(255, 255, 255));
    g_font_ui    = make_font(15, 0, L"Microsoft YaHei UI");
    g_font_title = make_font(20, 1, L"Microsoft YaHei UI");
    g_font_mono  = make_font(13, 0, L"Consolas");

    /* 自检模式（用控制台版编译后可运行：launcher.exe --selftest） */
    if (cmdline && strstr(cmdline, "--layoutcheck")) {
        int rc = layout_check();
        DeleteObject(g_font_ui);
        DeleteObject(g_font_title);
        DeleteObject(g_font_mono);
        DeleteObject(g_brush_bg);
        DeleteObject(g_brush_head);
        DeleteObject(g_brush_card);
        WSACleanup();
        return rc;
    }
    if (cmdline && strstr(cmdline, "--selftest")) {
        int rc = self_test();
        DeleteObject(g_font_ui);
        DeleteObject(g_font_title);
        DeleteObject(g_font_mono);
        DeleteObject(g_brush_bg);
        DeleteObject(g_brush_head);
        DeleteObject(g_brush_card);
        WSACleanup();
        return rc;
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;              /* 背景由 WM_ERASEBKGND 处理 */
    wc.lpszClassName = L"CampusCardLauncher";
    /* 优先用编译进 exe 的自定义图标（资源 id 1） */
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"窗口类注册失败", L"错误", MB_ICONERROR);
        return 1;
    }

    /* 默认尺寸与最小尺寸都留足空间，避免文字挤压重叠 */
    {
        int w = 660, h = 600;
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        g_hwnd = CreateWindowExW(
            0, wc.lpszClassName, APP_TITLE,
            WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,   /* 可缩放，不最大化 */
            (sw - w) / 2, (sh - h) / 3, w, h,
            NULL, NULL, inst, NULL);
    }
    if (!g_hwnd) {
        MessageBoxW(NULL, L"创建窗口失败", L"错误", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    /* 启动后端并等待就绪 */
    SetWindowTextW(GetDlgItem(g_hwnd, ID_LBL_STATUS), L"正在启动服务 ...");
    if (!start_server()) {
        MessageBoxW(g_hwnd,
                    L"同目录下没有找到 server.exe。\n\n"
                    L"请先编译后端（在项目目录执行 build.bat），"
                    L"或用下面的命令手工编译：\n"
                    L"  gcc -std=c11 -O2 -fexec-charset=UTF-8 server.c "
                    L"campus.c -o server.exe -lws2_32",
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

    stop_server();
    DeleteObject(g_font_ui);
    DeleteObject(g_font_title);
    DeleteObject(g_font_mono);
    DeleteObject(g_brush_bg);
    DeleteObject(g_brush_head);
    DeleteObject(g_brush_card);
    WSACleanup();
    return 0;
}
