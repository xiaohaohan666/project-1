/* ============================================================
 *  build.c —— 项目构建器（纯 C，替代原先的 Python 脚本）
 * ------------------------------------------------------------
 *  做三件事：
 *    1. 生成应用图标 campus.ico（手写 PNG 编码 + ICO 封装）
 *    2. 编译 server.exe / main.exe / 校园一卡通.exe
 *    3. 编译并运行单元测试 test_jstr.c，验证多字节处理
 *
 *  用法：
 *      build.exe              正常构建
 *      build.exe --selftest   只跑单元测试
 *      build.exe --icon-only  只生成图标
 *
 *  编译（自举）：
 *      gcc -std=c11 -O2 build.c -o build.exe
 *  下载的仓库里已附带 build.exe，一般无需自己编译。
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>          /* 图标 PNG 压缩用；MinGW 自带，链接加 -lz */

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define MKDIR(p) mkdir((p), 0755)
#endif

#define GCC_DEFAULT "gcc"
#define WINDRES_DEFAULT "windres"

/* ============================================================
 *                      图标生成
 * ============================================================ */
typedef struct { unsigned char r, g, b; } RGB3;

static const RGB3 C_DARK  = { 30,  64, 175 };
static const RGB3 C_MAIN  = { 37,  99, 235 };
static const RGB3 C_LIGHT = { 96, 165, 250 };
static const RGB3 C_GOLD  = { 250, 204, 21 };
static const RGB3 C_WHITE = { 255, 255, 255 };

static RGB3 lerp(RGB3 a, RGB3 b, double t) {
    RGB3 c;
    c.r = (unsigned char)(a.r + (b.r - a.r) * t + 0.5);
    c.g = (unsigned char)(a.g + (b.g - a.g) * t + 0.5);
    c.b = (unsigned char)(a.b + (b.b - a.b) * t + 0.5);
    return c;
}

/* 圆角矩形覆盖率，用于简单抗锯齿 */
static int round_alpha(double x, double y, double w, double h, double r) {
    double cx, cy, dx, dy, d;
    if (x < 0 || y < 0 || x > w || y > h) return 0;
    cx = x < r ? r : (x > w - r ? w - r : x);
    cy = y < r ? r : (y > h - r ? h - r : y);
    dx = x - cx; dy = y - cy;
    d = dx * dx + dy * dy;
    d = d <= 0 ? 0 : (d = (double)((int)(d * 1000) / 1000.0), d);
    /* 用整数近似开方，避免依赖 libm */
    {
        double g = d / 2 + 0.5;
        int i;
        for (i = 0; i < 24; i++) g = (g + d / (g <= 0 ? 1e-6 : g)) / 2;
        d = g;
    }
    if (d <= r - 1) return 255;
    if (d >= r + 1) return 0;
    return (int)(255.0 * (r + 1 - d) / 2.0);
}

/* 生成 size x size 的 RGBA 像素（行优先） */
static unsigned char *make_card(int size) {
    int S = size, x, y;
    unsigned char *px = (unsigned char *)calloc((size_t)S * S * 4, 1);
    double m, cw, ch, cy0, r;
    double chip_x, chip_y, chip_w, chip_h, bar_x, bar_w;

    if (!px) return NULL;
    m = S * 0.10;
    cw = S - 2 * m;
    ch = cw * 0.66;
    cy0 = (S - ch) / 2.0;
    r = S * 0.08;
    chip_x = m + cw * 0.12;
    chip_y = cy0 + ch * 0.44;
    chip_w = cw * 0.20;
    chip_h = ch * 0.26;
    bar_x = m + cw * 0.44;
    bar_w = cw * 0.42;

    for (y = 0; y < S; y++) {
        for (x = 0; x < S; x++) {
            int i = (y * S + x) * 4;
            int a = round_alpha(x - m, y - cy0, cw, ch, r);
            RGB3 col;
            double t;
            if (a == 0) continue;
            t = (y - cy0) / (ch <= 0 ? 1 : ch);
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            col = (t < 0.5) ? lerp(C_DARK, C_MAIN, t * 2)
                            : lerp(C_MAIN, C_LIGHT, (t - 0.5) * 2);
            /* 顶部高光带 */
            if (y >= cy0 + ch * 0.16 && y <= cy0 + ch * 0.30)
                col = lerp(col, C_WHITE, 0.14);
            /* 金色芯片 */
            if (x >= chip_x && x < chip_x + chip_w &&
                y >= chip_y && y < chip_y + chip_h) {
                int cw3 = (int)(chip_w / 3); if (cw3 < 2) cw3 = 2;
                int ch2 = (int)(chip_h / 2); if (ch2 < 2) ch2 = 2;
                col = C_GOLD;
                if ((int)(x - chip_x) % cw3 < 1 || (int)(y - chip_y) % ch2 < 1)
                    col = lerp(C_GOLD, (RGB3){ 180, 130, 10 }, 0.55);
            }
            /* 两条白色文字条 */
            {
                double bys[2] = { 0.36, 0.54 }, bhs[2] = { 0.09, 0.07 };
                int k;
                for (k = 0; k < 2; k++) {
                    double yy = cy0 + ch * bys[k];
                    double hh = ch * bhs[k];
                    if (hh < 1) hh = 1;
                    if (x >= bar_x && x < bar_x + bar_w &&
                        y >= yy && y < yy + hh)
                        col = lerp(col, C_WHITE, k == 0 ? 0.85 : 0.55);
                }
            }
            px[i] = col.r; px[i + 1] = col.g; px[i + 2] = col.b; px[i + 3] = (unsigned char)a;
        }
    }
    return px;
}

/* ---- 最小 zlib 存储块（不压缩）与 CRC32 ---- */
static uint32_t crc_table[256];
static void crc_init(void) {
    uint32_t c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (uint32_t)n;
        for (k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
}
static uint32_t crc32_of(const unsigned char *buf, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < len; i++) c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* 把 RGBA 像素写成 PNG 文件。
 * 只实现必要路径：IHDR + 存储型 deflate 块（不压缩）+ IEND，
 * 这样不需要引入 zlib，仍然产出完全合规的 PNG。 */
static int write_png_blocks(FILE *f, int size, const unsigned char *px) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    size_t stride = (size_t)size * 4;
    size_t raw_len = (size_t)size * (stride + 1);
    unsigned char *raw, *z;
    size_t zcap, zlen = 0, off = 0;
    uint32_t adler;
    int y;

    raw = (unsigned char *)malloc(raw_len);
    if (!raw) return 0;
    for (y = 0; y < size; y++) {
        raw[(size_t)y * (stride + 1)] = 0;
        memcpy(raw + (size_t)y * (stride + 1) + 1, px + (size_t)y * stride, stride);
    }
    /* 用 zlib 真正压缩。
     * 早先用"存储型 deflate 块"（完全不压缩）实现简单，但 512x512 的
     * 图标会膨胀到 1MB，仓库与传输都很浪费。 */
    zcap = compressBound((uLong)raw_len);
    z = (unsigned char *)malloc(zcap);
    if (!z) { free(raw); return 0; }
    {
        uLongf olen = (uLongf)zcap;
        if (compress2(z, &olen, raw, (uLong)raw_len, 9) != Z_OK) {
            free(raw); free(z); return 0;
        }
        zlen = (size_t)olen;
    }
    free(raw);
    (void)off; (void)adler;

    fwrite(sig, 1, 8, f);
    {
        unsigned char hdr[13], buf[4096];
        size_t n;
        hdr[0] = (unsigned char)(size >> 24); hdr[1] = (unsigned char)(size >> 16);
        hdr[2] = (unsigned char)((size >> 8) & 0xFF); hdr[3] = (unsigned char)(size & 0xFF);
        memcpy(hdr + 4, hdr, 4);
        hdr[8] = 8; hdr[9] = 6; hdr[10] = 0; hdr[11] = 0; hdr[12] = 0;
        /* IHDR */
        {
            uint32_t len = 13, crc;
            unsigned char lb[4] = { 0, 0, 0, 13 };
            memcpy(buf, "IHDR", 4); memcpy(buf + 4, hdr, 13);
            crc = crc32_of(buf, 17);
            fwrite(lb, 1, 4, f); fwrite(buf, 1, 17, f);
            fwrite(&(uint32_t){ 0 }, 1, 0, f);
            { unsigned char cb[4] = { (unsigned char)(crc >> 24), (unsigned char)(crc >> 16),
                                      (unsigned char)((crc >> 8) & 0xFF), (unsigned char)(crc & 0xFF) };
              fwrite(cb, 1, 4, f); }
            (void)len;
        }
        /* IDAT */
        n = zlen;
        {
            uint32_t crc;
            unsigned char lenb[4] = { (unsigned char)(n >> 24), (unsigned char)(n >> 16),
                                      (unsigned char)((n >> 8) & 0xFF), (unsigned char)(n & 0xFF) };
            unsigned char *tmp = (unsigned char *)malloc(n + 4);
            memcpy(tmp, "IDAT", 4); memcpy(tmp + 4, z, n);
            crc = crc32_of(tmp, n + 4);
            free(tmp);
            fwrite(lenb, 1, 4, f); fwrite("IDAT", 1, 4, f);
            fwrite(z, 1, n, f);
            { unsigned char cb[4] = { (unsigned char)(crc >> 24), (unsigned char)(crc >> 16),
                                      (unsigned char)((crc >> 8) & 0xFF), (unsigned char)(crc & 0xFF) };
              fwrite(cb, 1, 4, f); }
        }
        /* IEND */
        {
            uint32_t crc = crc32_of((unsigned char *)"IEND", 4);
            unsigned char cb[4] = { (unsigned char)(crc >> 24), (unsigned char)(crc >> 16),
                                    (unsigned char)((crc >> 8) & 0xFF), (unsigned char)(crc & 0xFF) };
            fwrite("\0\0\0\0", 1, 4, f); fwrite("IEND", 1, 4, f); fwrite(cb, 1, 4, f);
        }
    }
    free(z);
    return 1;
}

static int write_png_file(const char *path, int size) {
    unsigned char *px = make_card(size);
    FILE *f;
    int ok;
    if (!px) return 0;
    f = fopen(path, "wb");
    if (!f) { free(px); return 0; }
    ok = write_png_blocks(f, size, px);
    fclose(f);
    free(px);
    return ok;
}

/* 读回 PNG 文件内容 */
static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)n);
    if (b) { if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; } }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

static int make_ico(const char *ico_path, int *sizes, int n_sizes) {
    FILE *f;
    unsigned char **datas = (unsigned char **)calloc((size_t)n_sizes, sizeof(void *));
    size_t *lens = (size_t *)calloc((size_t)n_sizes, sizeof(size_t));
    int i, ok = 0;
    uint32_t offset;

    if (!datas || !lens) { free(datas); free(lens); return 0; }
    for (i = 0; i < n_sizes; i++) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "_ico_%d.png", sizes[i]);
        if (!write_png_file(tmp, sizes[i])) break;
        datas[i] = read_file(tmp, &lens[i]);
        remove(tmp);
        if (!datas[i]) break;
    }
    if (i == n_sizes) {
        f = fopen(ico_path, "wb");
        if (f) {
            unsigned char hdr[6] = { 0, 0, 1, 0, (unsigned char)n_sizes, 0 };
            fwrite(hdr, 1, 6, f);
            offset = 6 + 16 * (uint32_t)n_sizes;
            for (i = 0; i < n_sizes; i++) {
                unsigned char e[16];
                int s = sizes[i];
                e[0] = (unsigned char)(s >= 256 ? 0 : s);
                e[1] = (unsigned char)(s >= 256 ? 0 : s);
                e[2] = 0; e[3] = 0;
                e[4] = 1; e[5] = 0;
                e[6] = 32; e[7] = 0;
                e[8]  = (unsigned char)(lens[i] & 0xFF);
                e[9]  = (unsigned char)((lens[i] >> 8) & 0xFF);
                e[10] = (unsigned char)((lens[i] >> 16) & 0xFF);
                e[11] = (unsigned char)((lens[i] >> 24) & 0xFF);
                e[12] = (unsigned char)(offset & 0xFF);
                e[13] = (unsigned char)((offset >> 8) & 0xFF);
                e[14] = (unsigned char)((offset >> 16) & 0xFF);
                e[15] = (unsigned char)((offset >> 24) & 0xFF);
                fwrite(e, 1, 16, f);
                offset += (uint32_t)lens[i];
            }
            for (i = 0; i < n_sizes; i++) fwrite(datas[i], 1, lens[i], f);
            fclose(f);
            ok = 1;
        }
    }
    for (i = 0; i < n_sizes; i++) free(datas[i]);
    free(datas); free(lens);
    return ok;
}

static int generate_icons(void) {
    int sizes[] = { 16, 24, 32, 48, 64, 128, 256 };
    int web_sizes[] = { 192, 512, 180 };
    const char *web_names[] = { "icons/icon-192.png", "icons/icon-512.png",
                                "icons/apple-touch-icon.png" };
    int i, ok = 1;

    printf("[1/4] 生成图标\n");
    crc_init();
    MKDIR("icons");
    if (make_ico("campus.ico", sizes, 7))
        printf("      campus.ico（7 种尺寸）\n");
    else { printf("      campus.ico 失败\n"); ok = 0; }

    for (i = 0; i < 3; i++) {
        if (write_png_file(web_names[i], web_sizes[i]))
            printf("      %s (%dpx)\n", web_names[i], web_sizes[i]);
        else ok = 0;
    }
    /* maskable 与 icon-512 内容一致，直接复制 */
    {
        size_t n;
        unsigned char *b = read_file("icons/icon-512.png", &n);
        if (b) {
            FILE *f = fopen("icons/maskable-512.png", "wb");
            if (f) { fwrite(b, 1, n, f); fclose(f);
                     printf("      icons/maskable-512.png (512px)\n"); }
            free(b);
        }
    }
    return ok;
}

/* UTF-8 文本输出。
 *
 * MSVCRT 的 printf 用 ANSI 代码页转换 %s，中文文件名会直接变成空串
 * （构建摘要里"校园一卡通.exe"消失就是这个原因）。
 * 控制台下改用 WriteConsoleW 输出宽字符，重定向到文件时退回 fwrite。 */
static void out_u8(const char *s) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        wchar_t w[1024];
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 1024);
        if (n > 1) {
            DWORD written = 0;
            WriteConsoleW(h, w, (DWORD)(n - 1), &written, NULL);
            return;
        }
    }
#endif
    fputs(s, stdout);
}

/* ============================================================
 *                      编译
 * ============================================================ */
/* 用 CreateProcessW 而不是 system()。
 *
 * system() 走 ANSI 代码页（本机是 GBK），命令行里的 UTF-8 中文字节
 * 会被误解，导致输出文件名损坏（曾出现 校园一卡?exe.exe）。
 * CreateProcessW 全程 Unicode，中文文件名安全。 */
static int run_cmd(const wchar_t *cmdline) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD rc = 0;
    wchar_t *buf;
    size_t n = wcslen(cmdline) + 1;

    buf = (wchar_t *)malloc(n * sizeof(wchar_t));    if (!buf) return 0;
    wcscpy(buf, cmdline);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("      [错误] 无法启动进程（错误码 %lu）\n", GetLastError());
        free(buf);
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    free(buf);
    return rc == 0;
}

/* 把 UTF-8 命令拼成宽字符命令行 */
static int run_utf8(const char *cmdline) {
    wchar_t w[2048];
    int n = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, w, 2048);
    if (n <= 0) return 0;
    /* 打印用 UTF-8 原串：宽字符打印在控制台会被截断，看不出完整命令 */
    printf("      %s\n", cmdline);
    return run_cmd(w);
}

static int run(const char *cmdline) { return run_utf8(cmdline); }

static int have_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int build_all(void) {
    const char *common = "-std=c11 -Wall -Wextra -O2 -s -fexec-charset=UTF-8 "
                         "-Werror=implicit-function-declaration";
    char cmd[1024];
    int ok = 1;

    printf("[2/4] 编译程序\n");

    snprintf(cmd, sizeof(cmd),
             "%s %s server.c campus.c -o server.exe -lws2_32", GCC_DEFAULT, common);
    if (!run(cmd)) { printf("      server.exe 编译失败\n"); ok = 0; }

    snprintf(cmd, sizeof(cmd),
             "%s %s main.c campus.c -o main.exe", GCC_DEFAULT, common);
    if (!run(cmd)) { printf("      main.exe 编译失败\n"); ok = 0; }

    /* 桌面应用：先编译图标资源，再链接 */
    {
        int res_ok = 0;
        if (have_file("campus.ico")) {
            res_ok = run(WINDRES_DEFAULT " launcher.rc -o launcher_res.o "
                         "--output-format=coff");
        }
        snprintf(cmd, sizeof(cmd),
                 "%s %s -mwindows launcher.c%s -o \"校园一卡通.exe\" "
                 "-lcomctl32 -lgdi32 -luser32 -lws2_32 -lshell32",
                 GCC_DEFAULT, common, res_ok ? " launcher_res.o" : "");
        if (!run(cmd)) { printf("      校园一卡通.exe 编译失败\n"); ok = 0; }
    }
    return ok;
}

static int build_and_run_tests(void) {
    char cmd[512];
    printf("[3/4] 单元测试（多字节 JSON 处理）\n");
    if (!have_file("test_jstr.c")) { printf("      跳过：没有 test_jstr.c\n"); return 1; }
    snprintf(cmd, sizeof(cmd),
             "%s -std=c11 -Wall -Wextra -O2 -s -fexec-charset=UTF-8 "
             "test_jstr.c -o test_jstr.exe", GCC_DEFAULT);
    if (!run(cmd)) { printf("      编译失败\n"); return 0; }
    if (!run("test_jstr.exe")) { printf("      测试未通过\n"); return 0; }
    remove("test_jstr.exe");
    return 1;
}

static void cleanup(void) {
    printf("[4/4] 清理中间文件\n");
    remove("launcher_res.o");
    remove("campus.o");
    printf("      已清理 .o 文件\n");
}

/* ============================================================
 *                          入口
 * ============================================================ */
int main(int argc, char **argv) {
    int i;
#ifdef _WIN32
    SetConsoleOutputCP(65001);      /* 控制台按 UTF-8 输出，中文不乱码 */
#endif

    if (argc > 1 && strcmp(argv[1], "--icon-only") == 0) {
        crc_init(); MKDIR("icons");
        return generate_icons() ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        crc_init();
        return build_and_run_tests() ? 0 : 1;
    }

    printf("============================================================\n");
    printf("  校园一卡通 —— 构建\n");
    printf("============================================================\n");

    if (!generate_icons()) return 1;
    if (!build_all()) { cleanup(); return 1; }
    if (!build_and_run_tests()) { cleanup(); return 1; }
    cleanup();

    printf("============================================================\n");
    printf("  构建成功\n");
    {
        /* 注意：这里必须用 out_u8。
         * MSVCRT 的 printf 把 %s 按 ANSI 代码页转换，写不出中文文件名
         * （会直接变成空字符串），必须走 WriteConsoleW。 */
        const char *outs[3];
        outs[0] = "server.exe";
        outs[1] = "main.exe";
        outs[2] = "\xe6\xa0\xa1\xe5\x9b\xad\xe4\xb8\x80\xe5\x8d\xa1\xe9\x80\x9a.exe";
        for (i = 0; i < 3; i++) {
            FILE *f;
            char line[160];
            /* fopen 走 ANSI 代码页，对中文文件名不可靠：先用
             * CreateFileW 探一次存在性，再决定用哪种方式打开 */
            if (i == 2) {
                wchar_t wname[64];
                MultiByteToWideChar(CP_UTF8, 0, outs[i], -1, wname, 64);
                f = _wfopen(wname, L"rb");
            } else {
                f = fopen(outs[i], "rb");
            }
            if (f) {
                long n;
                fseek(f, 0, SEEK_END);
                n = ftell(f);
                fclose(f);
                snprintf(line, sizeof(line), "    %-24s %8.1f KB\n",
                         outs[i], n / 1024.0);
            } else {
                snprintf(line, sizeof(line), "    %-24s 缺失\n", outs[i]);
            }
            out_u8(line);
        }
    }
    out_u8("\n  桌面应用：双击 校园一卡通.exe 即可\n");
    return 0;
}
