/* 单元测试：单独验证服务端 JSON 解析函数 j_get_str 的多字节处理。
 *
 * 注意：这里用 \uXXXX 转义而不是 \xNN。C 语言的十六进制转义在字符串里是
 * 贪婪的 —— "\xa6\"" 会被解析成 \xa6 加 " （吃掉了结尾引号），
 * 反而会制造出「字符串被截断」的假象，把自己坑进去。
 * \u 转义后面必须是非十六进制字符，不存在这个问题，且服务端本来也支持。
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ---- 从 server.c 原样复制过来的 j_get_str ---- */
static int j_get_str(const char *body, const char *key, char *out, int osz) {
    char pat[48];
    const char *p;
    int i = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(body, pat);
    if (!p) { out[0] = '\0'; return 0; }
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') { out[0] = '\0'; return 0; }
    p++;
    /* 与 server.c 保持一致：i + 1 < osz，保证 4 字节缓冲能放下一个汉字 */
    while (*p && *p != '"' && i + 1 < osz) {
        if (p[0] == '\\' && p[1] == 'u' && isxdigit((unsigned char)p[2])) {
            unsigned int cp = 0;
            int k;
            for (k = 2; k < 6 && isxdigit((unsigned char)p[k]); k++) {
                int d = (p[k] <= '9') ? p[k] - '0' : ((p[k] | 0x20) - 'a' + 10);
                cp = cp * 16 + (unsigned)d;
            }
            if (cp < 0x80) {
                out[i++] = (char)cp;
            } else if (cp < 0x800) {
                if (i + 1 < osz) {
                    out[i++] = (char)(0xC0 | (cp >> 6));
                    out[i++] = (char)(0x80 | (cp & 0x3F));
                }
            } else if (i + 2 < osz) {
                out[i++] = (char)(0xE0 | (cp >> 12));
                out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[i++] = (char)(0x80 | (cp & 0x3F));
            }
            p += 6;
            continue;
        }
        if (p[0] == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    /* 仅当确实被缓冲上限截断时才回退到字符边界。
     * 若不判断就无条件回退，会把最后一个完整汉字也删掉。 */
    if (*p && *p != '"') {
        while (i > 0) {
            unsigned char c = (unsigned char)out[i - 1];
            if (c < 0x80) break;
            if (c >= 0xC0) { i--; break; }
            i--;
        }
    }
    out[i] = '\0';
    return 1;
}

static int is_valid_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n;
        if (*p < 0x80) { p++; continue; }
        else if ((*p & 0xE0) == 0xC0) n = 1;
        else if ((*p & 0xF0) == 0xE0) n = 2;
        else if ((*p & 0xF8) == 0xF0) n = 3;
        else return 0;
        p++;
        for (int k = 0; k < n; k++, p++) {
            if ((*p & 0xC0) != 0x80) return 0;
        }
    }
    return 1;
}

static int fails = 0;

static void dump(const char *label, const char *s) {
    printf("  %-12s %2zu 字节  ", label, strlen(s));
    for (size_t i = 0; i < strlen(s); i++) printf("%02x", (unsigned char)s[i]);
    printf("  %s\n", s);
}

static void expect(const char *label, const char *got, size_t want_len) {
    int ok_utf8 = is_valid_utf8(got);
    int ok_len = strlen(got) == want_len;
    printf("  [%s] %-12s 长度=%zu(期望 %zu)  UTF-8合法=%s\n",
           (ok_utf8 && ok_len) ? "OK " : "FAIL", label,
           strlen(got), want_len, ok_utf8 ? "是" : "否");
    if (!ok_utf8 || !ok_len) fails++;
    dump("", got);
}

int main(void) {
    /* name = 验收同学
     * 逐字节写成十六进制，并用字符串拼接隔开：
     *   · 不用 \uXXXX：GCC 在窄字符串里会把 \u 转成执行字符集，可能丢字符
     *   · 用拼接而不是连续 \xNN：十六进制转义是贪婪的，"\xa6\"" 会吃掉结尾引号
     */
    const char *body;
    /* 用 snprintf 构造含 \uXXXX 转义的 JSON 文本：
     * 直接写字面量会踩到 GCC 对窄字符串里 \u 的处理（转成执行字符集，丢字符），
     * 用 "\\u" 在运行时拼出来最稳妥。 */
    static char real_body[256];
    snprintf(real_body, sizeof(real_body),
             "{\"id\":\"C9291\",\"name\":\"\\u9a8c\\u6536\\u540c\\u5b66\","
             "\"number\":\"B9900100\",\"pwd\":\"abcd12\"}");
    body = real_body;

    /* 同时准备一份"原始 UTF-8 字节"的请求体，模拟前端实际发送的内容 */
    static const char raw_name[] = "\xe9\xaa\x8c" "\xe6\x94\xb6"
                                   "\xe5\x90\x8c" "\xe5\xad\xa6";
    static char raw_body[256];
    snprintf(raw_body, sizeof(raw_body),
             "{\"id\":\"C9291\",\"name\":\"%s\","
             "\"number\":\"B9900100\",\"pwd\":\"abcd12\"}", raw_name);

    char big[32], mid[12], tiny[4], exactly[5];

    printf("=== j_get_str 多字节处理测试 ===\n");
    printf("name = 验收同学（4 个汉字，UTF-8 共 12 字节）\n");
    printf("原始字节: ");
    for (size_t i = 0; i < sizeof(raw_name) - 1; i++)
        printf("%02x", (unsigned char)raw_name[i]);
    printf("\n\n");

    printf("--- A. 请求体用原始 UTF-8 字节（前端实际行为）---\n");
    memset(big, 0, sizeof(big));
    j_get_str(raw_body, "name", big, (int)sizeof(big));
    printf("缓冲 32 字节：应完整保留 12 字节\n");
    expect("out[32]", big, 12);

    memset(mid, 0, sizeof(mid));
    j_get_str(raw_body, "name", mid, (int)sizeof(mid));
    printf("\n缓冲 12 字节：只能放 3 个汉字（9 字节），须回退到字符边界\n");
    expect("out[12]", mid, 9);

    memset(tiny, 0, sizeof(tiny));
    j_get_str(raw_body, "name", tiny, (int)sizeof(tiny));
    printf("\n缓冲 4 字节：只剩 2 字节可用空间，放不下 3 字节汉字，只能是空串\n");
    expect("out[4]", tiny, 0);

    memset(exactly, 0, sizeof(exactly));
    j_get_str(raw_body, "name", exactly, (int)sizeof(exactly));
    printf("\n缓冲 5 字节：刚好放 1 个汉字（3 字节）+ 终止符\n");
    expect("out[5]", exactly, 3);

    printf("\n--- B. 请求体用 \\uXXXX 转义（兼容路径）---\n");
    memset(big, 0, sizeof(big));
    j_get_str(body, "name", big, (int)sizeof(big));
    printf("缓冲 32 字节：应完整保留 12 字节\n");
    expect("out[32]", big, 12);

    printf("\n=== 结果：%s ===\n", fails ? "存在失败" : "全部通过");
    return fails ? 1 : 0;
}
