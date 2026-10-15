/* ============================================================
 *  build.c —— 构建器（纯 C，无第三方依赖）
 * ------------------------------------------------------------
 *  本项目是终端（命令行）程序：没有图形界面、没有网页、没有网络，
 *  因此构建只做两件事：
 *    1. 编译主程序：main.c + campus.c -> main.exe
 *    2. 编译并运行单元测试：test_jstr.c，验证 UTF-8 字段处理
 *
 *  用法：
 *      build            构建 + 跑测试
 *      build --test     只跑测试
 *      build --clean    删除构建产物
 *
 *  自举（需要 MinGW-w64 的 gcc）：
 *      gcc -std=c11 -O2 build.c -o build.exe
 *  仓库里已附带 build.exe，一般无需自己编译。
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#define GCC_DEFAULT "gcc"

/* 编译选项。
 *   -fexec-charset=UTF-8  必须带：源码是 UTF-8，不带这个选项程序输出的
 *                         中文会乱码。
 *   -s                    去除符号表，减小体积。
 *   -Werror=implicit-function-declaration  防止漏原型带来的隐患。 */
#define CFLAGS "-std=c11 -Wall -Wextra -O2 -s -fexec-charset=UTF-8 " \
               "-Werror=implicit-function-declaration"

/* UTF-8 文本输出。
 *
 * 这里刻意只用 printf，和 main.c 保持一致。理由：
 *   main() 里调了 SetConsoleOutputCP(65001)，CRT 会把 UTF-8 字节按
 *   控制台代码页写出，中文正常；重定向到文件时也是 UTF-8 字节，同样正常。
 *
 * 早先尝试用 WriteConsoleW 直写控制台，结果在重定向场景下写出了 UTF-16
 * （开头 FF FE BOM、字符间夹 0x00），文件不可读。原因是用 GetConsoleMode
 * 或 GetFileType 判断「是不是控制台」都不可靠 —— 同一程序在终端、
 * 重定向、管道下三个 API 的返回值并不一致，且部分环境里
 * WriteConsoleW 会静默失败（返回 0 并丢弃内容）。
 * 既然 CRT 这条路已经被 main.exe 验证过，就没必要自己绕。 */
static void out_u8(const char *s) {
    fputs(s, stdout);
}

/* 启动子进程执行命令行。
 * 用 CreateProcessW 而不是 system()：system() 走 ANSI 代码页，
 * 命令行里的 UTF-8 中文会被误解（曾把输出文件名搞坏）。 */
static int run_cmd(const wchar_t *cmdline) {
#ifdef _WIN32
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD rc = 0;
    wchar_t *buf;
    size_t n = wcslen(cmdline) + 1;

    buf = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (!buf) return 0;
    wcscpy(buf, cmdline);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL,
                        &si, &pi)) {
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
#else
    char narrow[2048];
    size_t i;
    for (i = 0; i < sizeof(narrow) - 1 && cmdline[i]; i++)
        narrow[i] = (char)cmdline[i];
    narrow[i] = '\0';
    return system(narrow) == 0;
#endif
}

static int run_utf8(const char *cmdline) {
    int ok;
    char line[1200];
    snprintf(line, sizeof(line), "      %s\n", cmdline);
    out_u8(line);
#ifdef _WIN32
    {
        wchar_t w[2048];
        if (MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, w, 2048) <= 0)
            return 0;
        ok = run_cmd(w);
    }
#else
    ok = run_cmd(NULL);
#endif
    return ok;
}

static int have_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* 编译主程序 */
static int build_main(void) {
    char cmd[512];
    out_u8("\n[1/2] 编译主程序（终端版）\n");
    snprintf(cmd, sizeof(cmd),
             GCC_DEFAULT " " CFLAGS " main.c campus.c -o main.exe");
    if (!run_utf8(cmd)) {
        out_u8("      main.exe 编译失败\n");
        return 0;
    }
    return 1;
}

/* 编译并运行单元测试 */
static int run_tests(void) {
    char cmd[512];
    out_u8("\n[2/2] 单元测试：UTF-8 字段处理\n");
    if (!have_file("test_jstr.c")) {
        out_u8("      跳过：找不到 test_jstr.c\n");
        return 1;
    }
    snprintf(cmd, sizeof(cmd),
             GCC_DEFAULT " " CFLAGS " test_jstr.c -o test_jstr.exe");
    if (!run_utf8(cmd)) {
        out_u8("      测试程序编译失败\n");
        return 0;
    }
    if (!run_utf8("test_jstr.exe")) {
        out_u8("      测试未通过\n");
        return 0;
    }
    remove("test_jstr.exe");
    return 1;
}

static void clean(void) {
    out_u8("\n清理构建产物\n");
    remove("main.exe");
    remove("test_jstr.exe");
    remove("campus.o");
    remove("main.o");
    out_u8("      已删除 main.exe / test_jstr.exe / *.o\n");
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);      /* 控制台按 UTF-8 输出中文 */
#endif

    if (argc > 1 && strcmp(argv[1], "--clean") == 0) {
        clean();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        return run_tests() ? 0 : 1;
    }

    out_u8("============================================================\n");
    out_u8("  校园一卡通（终端版）—— 构建\n");
    out_u8("============================================================\n");

    if (!build_main()) return 1;
    if (!run_tests()) return 1;

    out_u8("\n============================================================\n");
    out_u8("  构建成功\n");
    if (have_file("main.exe")) {
        FILE *f = fopen("main.exe", "rb");
        long n = -1;
        char line[128];
        if (f) {
            fseek(f, 0, SEEK_END);
            n = ftell(f);
            fclose(f);
        }
        snprintf(line, sizeof(line), "    %-16s %8.1f KB\n",
                 "main.exe", n / 1024.0);
        out_u8(line);
    } else {
        out_u8("    main.exe 缺失\n");
    }
    out_u8("\n  直接运行：main.exe\n");
    return 0;
}
