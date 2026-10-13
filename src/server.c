/* ============================================================
 *  模拟校园卡 —— HTTP/JSON 服务（可部署到云平台）
 *  纯 C + WinSock2，无第三方依赖；同时托管前端静态页面。
 * ------------------------------------------------------------
 *  用法: server [端口] [web目录] [数据目录] [监听地址]
 *        端口优先取环境变量 PORT（云平台会注入），其次命令行，默认 8090
 *        监听地址默认 0.0.0.0（容器内需对外可访问）
 * ------------------------------------------------------------
 *  无会话设计：每个请求带 X-Token（学生为卡号，管理员为 ADMIN）与
 *  X-Pwd（对应密码），服务端每次校验，因此天然支持多副本部署。
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>      /* intptr_t：统一套接字返回类型 */
#include <string.h>
#include <time.h>
#include "campus.h"

/* ---------------- 网络层跨平台适配（Windows WinSock / Linux BSD socket） ---------------- */
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;                      /* Windows 的套接字类型 */
#  define sock_close closesocket
/* 初始化 WinSock。成功返回 0 */
static int sock_init(void) {
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w);
}
static void sock_cleanup(void) { WSACleanup(); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
typedef int sock_t;                         /* Linux 的套接字类型   */
#  define sock_close close
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
static int  sock_init(void)    { return 0; }
static void sock_cleanup(void) { }
#endif

#define REQ_MAX (256 * 1024)        /* 请求缓冲上限 */
#define RES_MAX (512 * 1024)        /* 响应缓冲上限 */

static char g_web[256] = "web";     /* 静态页面目录 */

/* ============================================================
 *                    JSON 输出小工具
 * ============================================================ */
static char  *g_buf;                /* 输出缓冲 */
static size_t g_len, g_cap;         /* 已写长度 / 容量 */

/* 初始化输出缓冲 */
static void j_init(char *buf, size_t cap) { g_buf = buf; g_len = 0; g_cap = cap; g_buf[0] = '\0'; }
/* 追加原始字符串 */
static void j_put(const char *s) {
    size_t n = strlen(s);
    if (g_len + n + 1 > g_cap) n = g_cap - g_len - 1;
    memcpy(g_buf + g_len, s, n);
    g_len += n;
    g_buf[g_len] = '\0';
}
/* 追加格式化文本 */
static void j_fmt(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    j_put(tmp);
}
/* 追加 JSON 字符串（转义引号与控制字符） */
static void j_str(const char *s) {
    char out[1200];
    size_t o = 0;
    out[o++] = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p && o < sizeof(out) - 8; p++) {
        switch (*p) {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                if (*p < 0x20) o += (size_t)snprintf(out + o, 8, "\\u%04x", *p);
                else           out[o++] = (char)*p;
        }
    }
    out[o++] = '"';
    out[o] = '\0';
    j_put(out);
}
static void kv_s(const char *k, const char *v) { j_str(k); j_put(":"); j_str(v); }
static void kv_d(const char *k, double v)       { j_str(k); j_fmt(":%.2f", v); }
static void kv_i(const char *k, long v)         { j_str(k); j_fmt(":%ld", v); }
/* 统一的错误响应 */
static void res_err(const char *msg) {
    j_init(g_buf, g_cap);
    j_put("{\"ok\":false,\"error\":"); j_str(msg); j_put("}");
}

/* ============================================================
 *              极简 JSON / 查询串取值
 * ============================================================ */
/* 取查询串参数。query 形如 "a=1&b=2"。未找到时 out 置空 */
static const char *q_get(const char *query, const char *key, char *out, int osz) {
    char pat[32];
    const char *p;
    int i = 0;
    snprintf(pat, sizeof(pat), "%s=", key);
    out[0] = '\0';
    p = query ? strstr(query, pat) : NULL;
    if (!p) return out;
    p += strlen(pat);
    while (*p && *p != '&' && i < osz - 1) out[i++] = *p++;
    out[i] = '\0';
    return out;
}
/* 取请求体 JSON 的字符串字段；找到返回 1 */
static int j_get_str(const char *body, const char *key, char *out, int osz) {
    char pat[48];
    const char *p;
    int i = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && i < osz - 1) {
        if (p[0] == '\\' && p[1] == 'u' && isxdigit((unsigned char)p[2])) {
            /* 兼容 \uXXXX 转义：转成 UTF-8 写入。前端 JSON.stringify 一般直接
             * 发原始 UTF-8，但其他客户端（如 Python json.dumps）可能发转义形式 */
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
        if (p[0] == '\\' && p[1]) p++;      /* 其他转义：取原字符 */
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}
/* 取请求体 JSON 的数字字段；找不到返回默认值 */
static double j_get_num(const char *body, const char *key, double dflt) {
    char pat[48];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(body, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return atof(p);
}

/* ============================================================
 *                      业务对象输出
 * ============================================================ */
static void put_card(const Card *k) {
    j_put("{");
    kv_s("id", k->id);             j_put(",");
    kv_s("name", k->name);         j_put(",");
    kv_s("number", k->number);     j_put(",");
    kv_d("recharge", k->recharge); j_put(",");
    kv_d("remain", k->remain);     j_put(",");
    kv_i("state", k->state);       j_put(",");
    kv_s("stateText", campus_state_text(k->state)); j_put(",");
    kv_i("online", k->online);     j_put(",");
    kv_s("startTime", k->start_time); j_put(",");
    kv_s("address", k->address);
    j_put("}");
}
static void put_record(const Record *r) {
    j_put("{");
    kv_s("id", r->id);                j_put(",");
    kv_s("date", r->date);            j_put(",");
    kv_s("startTime", r->start_time); j_put(",");
    kv_i("duration", r->duration);    j_put(",");
    kv_d("cost", r->cost);            j_put(",");
    kv_s("state", r->state);          j_put(",");
    kv_s("address", r->address);
    j_put("}");
}

/* ============================================================
 *                      认证与时间区间
 * ============================================================ */
/* 校验身份。token 卡号或 ADMIN；pwd 对应密码；as_admin 期望身份。
 * 通过返回 1；失败时已写入错误响应并返回 0 */
static int auth(const char *token, const char *pwd, int as_admin) {
    if (!*token) { res_err("缺少身份令牌 X-Token"); return 0; }
    if (!campus_login(token, pwd, as_admin)) {
        res_err(as_admin ? "管理员密码错误" : "卡号或密码错误");
        return 0;
    }
    return 1;
}
/* 按时间段名返回起始时间；"all" 返回 0（不限起始） */
static time_t period_start(const char *period) {
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    if (!strcmp(period, "all")) return 0;
    if (!strcmp(period, "week")) {
        int wday = tm.tm_wday;                      /* 0 为周日 */
        tm.tm_mday -= (wday == 0) ? 6 : (wday - 1); /* 退到本周一 */
    } else if (!strcmp(period, "month")) {
        tm.tm_mday = 1;
    }
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; tm.tm_isdst = -1;
    return mktime(&tm);
}

/* ============================================================
 *                        接口实现
 * ============================================================ */
/* 学生：卡内余额、状态、上机信息 */
static void h_card(const char *token, const char *pwd) {
    Card *k;
    if (!auth(token, pwd, 0)) return;
    k = campus_find(token);
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"card\":"); put_card(k);
    j_fmt(",\"rate\":%.2f}", RATE_PER_HOUR);
}
/* 学生：上机记录明细 */
static void h_records(const char *token, const char *pwd) {
    static Record rec[REC_MAX];
    int n;
    if (!auth(token, pwd, 0)) return;
    n = campus_load_records(token, rec, REC_MAX);
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"records\":[");
    for (int i = 0; i < n; i++) {
        if (i) j_put(",");
        put_record(&rec[i]);
    }
    j_fmt("],\"total\":%d}", n);
}
/* 学生：统计上机时间与费用，period=day|week|month|all */
static void h_stats(const char *token, const char *pwd, const char *period) {
    int count = 0, minutes = 0;
    double cost = 0.0;
    if (!auth(token, pwd, 0)) return;
    campus_stats(token, period_start(period), &count, &minutes, &cost);
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,");
    kv_s("period", period);   j_put(",");
    kv_i("count", count);     j_put(",");
    kv_i("minutes", minutes); j_put(",");
    kv_d("cost", cost);       j_put(",");
    j_fmt("\"hours\":%.2f", minutes / 60.0);
    j_put("}");
}
/* 上机登记 / 下机结账。action=online 时需要 place */
static void h_machine(const char *token, const char *pwd, const char *body) {
    Card *k;
    char action[12] = "", place[24] = "", err[64];
    double cost = 0.0;
    if (!auth(token, pwd, 0)) return;
    k = campus_find(token);
    j_get_str(body, "action", action, sizeof(action));
    j_get_str(body, "place", place, sizeof(place));

    if (!strcmp(action, "online")) {
        if (campus_online(k, place, err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(action, "offline")) {
        if (campus_offline(k, &cost, err, sizeof(err))) { res_err(err); return; }
    } else {
        res_err("action 只能是 online 或 offline");
        return;
    }
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"message\":");
    j_str(!strcmp(action, "online") ? "上机登记成功" : "下机结账完成");
    j_fmt(",\"cost\":%.2f,\"card\":", cost); put_card(k);
    j_put("}");
}
/* 学生：自助挂失 */
static void h_self_lost(const char *token, const char *pwd) {
    Card *k;
    char err[64];
    if (!auth(token, pwd, 0)) return;
    k = campus_find(token);
    if (campus_set_lost(k, err, sizeof(err))) { res_err(err); return; }
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"message\":"); j_str("挂失成功，该卡不能再上机");
    j_put(",\"card\":"); put_card(k); j_put("}");
}
/* 学生：修改登录密码 */
static void h_change_pwd(const char *token, const char *pwd, const char *body) {
    Card *k;
    char np[20] = "", err[64];
    if (!auth(token, pwd, 0)) return;
    k = campus_find(token);
    j_get_str(body, "newPwd", np, sizeof(np));
    if (campus_change_pwd(k, pwd, np, err, sizeof(err))) { res_err(err); return; }
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"message\":"); j_str("密码修改成功"); j_put("}");
}

/* 管理员：全部卡片与汇总 */
static void h_admin_cards(const char *token, const char *pwd) {
    double remain = 0.0, recharge = 0.0;
    int online = 0;
    if (!auth(token, pwd, 1)) return;
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"cards\":[");
    for (int i = 0; i < campus_count(); i++) {
        Card *k = campus_at(i);
        if (i) j_put(",");
        put_card(k);
        remain += k->remain;
        recharge += k->recharge;
        if (k->online == ONLINE_YES) online++;
    }
    j_put("],");
    kv_i("total", campus_count());   j_put(",");
    kv_d("remainTotal", remain);     j_put(",");
    kv_d("rechargeTotal", recharge); j_put(",");
    kv_i("onlineCount", online);
    j_put("}");
}
/* 管理员：当天收入统计 */
static void h_admin_income(const char *token, const char *pwd) {
    double fee = 0.0;
    int times = 0;
    char today[12];
    time_t t = time(NULL);
    if (!auth(token, pwd, 1)) return;
    campus_today_income(&fee, &times);
    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&t));
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,");
    kv_s("day", today);     j_put(",");
    kv_d("feeIncome", fee); j_put(",");
    kv_i("times", times);
    j_put("}");
}
/* 管理员：查看指定卡的上机记录（?id=C001） */
static void h_admin_records(const char *token, const char *pwd, const char *id) {
    static Record rec[REC_MAX];
    int n;
    if (!auth(token, pwd, 1)) return;
    if (!*id) { res_err("请提供卡号参数 id"); return; }
    if (!campus_find(id)) { res_err("找不到该卡号"); return; }
    n = campus_load_records(id, rec, REC_MAX);
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"records\":[");
    for (int i = 0; i < n; i++) {
        if (i) j_put(",");
        put_record(&rec[i]);
    }
    j_fmt("],\"total\":%d}", n);
}
/* 管理员：充值 / 挂失 / 重置密码（op 取自路径末段） */
static void h_admin_op(const char *token, const char *pwd, const char *body, const char *op) {
    char id[10] = "", err[64];
    Card *k;
    if (!auth(token, pwd, 1)) return;
    j_get_str(body, "id", id, sizeof(id));
    k = campus_find(id);
    if (!k) { res_err("找不到该卡号"); return; }

    if (!strcmp(op, "recharge")) {
        double amount = j_get_num(body, "amount", 0);
        if (campus_recharge(k, amount, err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(op, "lost")) {
        if (campus_set_lost(k, err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(op, "reset")) {
        if (campus_reset_pwd(k, err, sizeof(err))) { res_err(err); return; }
    } else {
        res_err("未知操作");
        return;
    }
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"card\":"); put_card(k); j_put("}");
}

/* ============================================================
 *                        请求路由
 * ============================================================ */
static void route(const char *method, const char *path, const char *query,
                  const char *body, char *res, size_t cap) {
    char token[16] = "", pwd[20] = "", period[12] = "", id[10] = "";
    int post = !strcmp(method, "POST");

    j_init(res, cap);
    q_get(query, "token", token, sizeof(token));
    q_get(query, "pwd", pwd, sizeof(pwd));

    if (!strcmp(path, "/api/health")) {
        j_put("{\"ok\":true,\"service\":\"campus-card\"");
        j_fmt(",\"cards\":%d,\"rate\":%.2f}", campus_count(), RATE_PER_HOUR);
        return;
    }
    /* 登录只做校验；令牌与密码由前端保存并在后续请求中携带 */
    if (!strcmp(path, "/api/login") && post) {
        char id2[10] = "", pw2[20] = "";
        int as_admin = strstr(body, "\"admin\":true") != NULL;
        j_get_str(body, "id", id2, sizeof(id2));
        j_get_str(body, "pwd", pw2, sizeof(pw2));
        if (!campus_login(id2, pw2, as_admin)) {
            res_err(as_admin ? "管理员密码错误" : "卡号或密码错误");
            return;
        }
        j_put("{\"ok\":true,\"token\":");
        j_str(as_admin ? "ADMIN" : id2);
        j_put(",\"admin\":"); j_put(as_admin ? "true" : "false");
        j_put(",\"card\":");
        if (as_admin) j_put("null"); else put_card(campus_find(id2));
        j_put("}");
        return;
    }
    if (!strcmp(path, "/api/card"))    { h_card(token, pwd);    return; }
    if (!strcmp(path, "/api/records")) { h_records(token, pwd); return; }
    if (!strcmp(path, "/api/stats")) {
        q_get(query, "period", period, sizeof(period));
        h_stats(token, pwd, *period ? period : "day");
        return;
    }
    if (!strcmp(path, "/api/machine") && post) { h_machine(token, pwd, body);    return; }
    if (!strcmp(path, "/api/lost")    && post) { h_self_lost(token, pwd);        return; }
    if (!strcmp(path, "/api/pwd")     && post) { h_change_pwd(token, pwd, body); return; }
    if (!strcmp(path, "/api/admin/cards"))     { h_admin_cards(token, pwd);      return; }
    if (!strcmp(path, "/api/admin/income"))    { h_admin_income(token, pwd);     return; }
    if (!strcmp(path, "/api/admin/records")) {
        q_get(query, "id", id, sizeof(id));
        h_admin_records(token, pwd, id);
        return;
    }
    if (!strncmp(path, "/api/admin/", 11) && post) { h_admin_op(token, pwd, body, path + 11); return; }
    res_err("接口不存在");
}

/* ============================================================
 *                       静态文件托管
 * ============================================================ */
static const char *mime_of(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript; charset=utf-8";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    return "text/plain; charset=utf-8";
}
static int send_all(sock_t fd, const char *buf, size_t len) {
    while (len > 0) {
        intptr_t n = send(fd, buf, (int)len, 0);
        if (n <= 0) return 0;
        buf += n;
        len -= (size_t)n;
    }
    return 1;
}
static void send_head(sock_t fd, int code, const char *status,
                      const char *mime, size_t len) {
    char head[320];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
             "Connection: close\r\n\r\n",
             code, status, mime, len);
    (void)send_all(fd, head, strlen(head));
}
static void serve_static(sock_t fd, const char *path) {
    char rel[256], full[512];
    FILE *fp;
    char *buf;
    long size;
    size_t got;

    snprintf(rel, sizeof(rel), "%s", path);
    if (!strcmp(rel, "/") || !*rel) snprintf(rel, sizeof(rel), "/index.html");
    if (strstr(rel, "..")) { send_head(fd, 400, "Bad Request", "text/plain", 0); return; }

    snprintf(full, sizeof(full), "%s%s", g_web, rel);
    fp = fopen(full, "rb");
    if (!fp && !strchr(rel + 1, '.')) {             /* 前端路由回退首页 */
        snprintf(full, sizeof(full), "%s/index.html", g_web);
        fp = fopen(full, "rb");
    }
    if (!fp) { send_head(fd, 404, "Not Found", "text/plain; charset=utf-8", 0); return; }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return; }
    got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    send_head(fd, 200, "OK", mime_of(rel), got);
    send_all(fd, buf, got);
    free(buf);
}

/* 取请求头字段值（头名不区分大小写）。
 * req 完整请求报文；name 头名；out 输出；osz 缓冲大小。找到返回 1 */
static int header_value(const char *req, const char *name, char *out, int osz) {
    size_t nlen = strlen(name);
    const char *p = req;
    out[0] = '\0';
    /* 逐行扫描请求头，遇到空行（头部结束）即停止 */
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len == 0) break;                        /* 空行：请求头结束 */
        if (len > nlen && !_strnicmp(p, name, (int)nlen) && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            int i = 0;
            while (v < p + len && i < osz - 1) out[i++] = *v++;
            out[i] = '\0';
            return 1;
        }
        if (!eol) break;
        p = eol + 2;
    }
    return 0;
}

/* ============================================================
 *                   连接处理与主函数
 * ============================================================ */
static void handle_conn(sock_t fd) {
    char *req = (char *)malloc(REQ_MAX);
    char *res = (char *)malloc(RES_MAX);
    size_t total = 0;
    int ready = 0;
    char method[8] = "", path[256] = "", query[320] = "";
    char *qmark, *body;

    if (!req || !res) { free(req); free(res); sock_close(fd); return; }

    /* 1. 收请求直到报文头完整（POST 还要收满 Content-Length） */
    while (total < REQ_MAX - 1) {
        intptr_t n = recv(fd, req + total, (int)(REQ_MAX - 1 - total), 0);
        char *he, *cl;
        size_t need;
        if (n <= 0) break;
        total += (size_t)n;
        req[total] = '\0';
        he = strstr(req, "\r\n\r\n");
        if (!he) continue;
        need = (size_t)(he - req) + 4;
        cl = strstr(req, "Content-Length:");
        if (cl) need += (size_t)strtoul(cl + 15, NULL, 10);
        if (total >= need) { ready = 1; break; }
    }
    if (!ready) { free(req); free(res); sock_close(fd); return; }

    /* 2. 拆方法、路径、查询串与请求体 */
    sscanf(req, "%7s %255s", method, path);
    qmark = strchr(path, '?');
    if (qmark) { *qmark = '\0'; snprintf(query, sizeof(query), "%s", qmark + 1); }
    body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : req + strlen(req);

    /* 3. 令牌与密码优先取请求头（HTTP 头名大小写不敏感），其次查询串 */
    {
        char v[24];
        if (header_value(req, "X-Token", v, sizeof(v)))
            snprintf(query + strlen(query), sizeof(query) - strlen(query),
                     "%stoken=%s", *query ? "&" : "", v);
        if (header_value(req, "X-Pwd", v, sizeof(v)))
            snprintf(query + strlen(query), sizeof(query) - strlen(query),
                     "%spwd=%s", *query ? "&" : "", v);
    }

    /* 4. 接口或静态文件 */
    if (!strncmp(path, "/api/", 5)) {
        size_t len;
        route(method, path, query, body, res, RES_MAX);
        len = strlen(res);
        send_head(fd, 200, "OK", "application/json; charset=utf-8", len);
        send_all(fd, res, len);
    } else if (!strcmp(method, "OPTIONS")) {
        send_head(fd, 204, "No Content", "text/plain", 0);
    } else {
        serve_static(fd, path);
    }
    free(req);
    free(res);
    sock_close(fd);
}

/* 入口：端口 / web 目录 / 数据目录 / 监听地址 */
int main(int argc, char **argv) {
    int port = 8090;
    const char *env_port = getenv("PORT");      /* 云平台注入的端口 */
    const char *host = "0.0.0.0";               /* 容器内需对外可访问 */
    const char *data_dir = "data";
    sock_t srv;
    int yes = 1;
    struct sockaddr_in addr;

    if (env_port && atoi(env_port) > 0) port = atoi(env_port);
    if (argc > 1 && atoi(argv[1]) > 0)  port = atoi(argv[1]);
    if (argc > 2) snprintf(g_web, sizeof(g_web), "%s", argv[2]);
    if (argc > 3) data_dir = argv[3];
    if (argc > 4) host = argv[4];

    if (sock_init()) { printf("网络初始化失败\n"); return 1; }
    campus_init(data_dir);

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { printf("创建套接字失败\n"); return 1; }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("端口 %d 绑定失败\n", port);
        return 1;
    }
    listen(srv, 32);

    printf("模拟校园卡服务已启动 http://%s:%d/  卡片 %d 张  费率 %.2f 元/小时\n",
           host, port, campus_count(), RATE_PER_HOUR);
    fflush(stdout);

    while (1) {
        sock_t cli = accept(srv, NULL, NULL);
        if (cli == INVALID_SOCKET) continue;
        handle_conn(cli);
    }
    sock_cleanup();
    return 0;
}
