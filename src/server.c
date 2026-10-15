/* ============================================================
 *  校园一卡通 —— HTTP/JSON 服务（可部署到云平台）
 *  纯 C，跨 Windows(WinSock) / Linux(BSD socket)，无第三方依赖；
 *  同时托管前端静态页面（含 PWA 的 sw.js 与 manifest）。
 * ------------------------------------------------------------
 *  用法: server [端口] [web目录] [数据目录] [监听地址]
 *        端口优先取环境变量 PORT（云平台注入），其次命令行，默认 8090
 *        监听地址默认 0.0.0.0（容器内需对外可访问）
 * ------------------------------------------------------------
 *  无会话设计：每个请求带 X-Token（学生为卡号，管理员为 ADMIN）与
 *  X-Pwd（对应密码），服务端每次校验，因此天然支持多副本部署。
 *  消费类请求额外带 X-PayPwd（交易密码）。
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>      /* intptr_t：统一套接字返回类型 */
#include <string.h>
#include <ctype.h>       /* isxdigit：解析 \uXXXX 转义时使用 */
#include <time.h>
#include "campus.h"

/* ---------------- 网络层跨平台适配 ---------------- */
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>       /* SetConsoleOutputCP：控制台按 UTF-8 显示中文 */
typedef SOCKET sock_t;
#  define sock_close closesocket
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
typedef int sock_t;
#  define sock_close close
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
static int  sock_init(void)    { return 0; }
static void sock_cleanup(void) { }
#endif

#define REQ_MAX (256 * 1024)
/* 单次返回的最大记录数：防止响应超过缓冲被截断 */
#define ACCESS_PAGE 300
#define RES_MAX (512 * 1024)

static char g_web[256] = "web";
/* 最近一次收到前端心跳的时间（time_t）。桌面启动器靠它判断
 * 应用窗口是否还开着：窗口关了就没人发心跳了。
 * 0 表示从未收到过心跳（例如只用命令行版时）。 */
static time_t g_last_beat;

/* ============================================================
 *                    JSON 输出小工具
 * ============================================================ */
static char  *g_buf;
static size_t g_len, g_cap;

static void j_init(char *buf, size_t cap) { g_buf = buf; g_len = 0; g_cap = cap; g_buf[0] = '\0'; }
static void j_put(const char *s) {
    size_t n = strlen(s);
    if (g_len + n + 1 > g_cap) n = g_cap - g_len - 1;
    memcpy(g_buf + g_len, s, n);
    g_len += n;
    g_buf[g_len] = '\0';
}
static void j_fmt(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    j_put(tmp);
}
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
static void res_err(const char *msg) {
    j_init(g_buf, g_cap);
    j_put("{\"ok\":false,\"error\":"); j_str(msg); j_put("}");
}

/* ============================================================
 *              极简 JSON / 查询串 / 请求头取值
 * ============================================================ */
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
    while (*p && *p != '"' && i < osz - 1) {
        if (p[0] == '\\' && p[1] == 'u' && isxdigit((unsigned char)p[2])) {
            /* 兼容 \uXXXX 转义：转成 UTF-8 写入 */
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
    out[i] = '\0';
    return 1;
}
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
/* 取请求头字段值（头名不区分大小写） */
static int ci_ncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) break;
    }
    return 0;
}
static int header_value(const char *req, const char *name, char *out, int osz) {
    size_t nlen = strlen(name);
    const char *p = req;
    out[0] = '\0';
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len == 0) break;
        if (len > nlen && !ci_ncmp(p, name, nlen) && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            int i = 0;
            while (*v == ' ' || *v == '\t') v++;
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
    kv_s("address", k->address);   j_put(",");
    kv_d("limitOnce", k->limit_once); j_put(",");
    kv_d("limitDay", k->limit_day);   j_put(",");
    kv_d("usedDay", k->used_day);     j_put(",");
    kv_s("expire", k->expire);
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
static void put_tx(const Transaction *t) {
    j_put("{");
    kv_i("seq", t->seq);              j_put(",");
    kv_s("time", t->time);            j_put(",");
    kv_s("id", t->id);                j_put(",");
    kv_i("type", t->type);            j_put(",");
    kv_s("typeText", campus_type_text(t->type)); j_put(",");
    kv_s("merchant", t->merchant);    j_put(",");
    kv_d("amount", t->amount);        j_put(",");
    kv_d("balance", t->balance);      j_put(",");
    kv_s("device", t->device);        j_put(",");
    kv_s("memo", t->memo);
    j_put("}");
}
static void put_access(const AccessLog *a) {
    j_put("{");
    kv_s("id", a->id);      j_put(",");
    kv_s("time", a->time);  j_put(",");
    kv_i("dir", a->dir);    j_put(",");
    kv_s("dirText", a->dir == ACC_IN ? "进入" : "离开"); j_put(",");
    kv_s("place", a->place); j_put(",");
    kv_i("kind", a->kind);  j_put(",");
    kv_s("kindText", a->kind == ACCESS_DOOR ? "门禁" : "考勤");
    j_put("}");
}

/* ============================================================
 *                      认证与时间区间
 * ============================================================ */
static int auth(const char *token, const char *pwd, int as_admin) {
    if (!*token) { res_err("缺少身份令牌 X-Token"); return 0; }
    if (!campus_login(token, pwd, as_admin)) {
        res_err(as_admin ? "管理员密码错误" : "卡号或密码错误");
        return 0;
    }
    return 1;
}
static time_t period_start(const char *period) {
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    if (!strcmp(period, "all")) return 0;
    if (!strcmp(period, "week")) {
        int wday = tm.tm_wday;
        tm.tm_mday -= (wday == 0) ? 6 : (wday - 1);
    } else if (!strcmp(period, "month")) {
        tm.tm_mday = 1;
    }
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; tm.tm_isdst = -1;
    return mktime(&tm);
}

/* ============================================================
 *                      学生端接口
 * ============================================================ */
static void h_card(const char *token, const char *pwd) {
    Card *k;
    if (!auth(token, pwd, 0)) return;
    k = campus_find(token);
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"card\":"); put_card(k);
    j_fmt(",\"rate\":%.2f}", RATE_PER_HOUR);
}
static void h_records(const char *token, const char *pwd) {
    static Record rec[REC_MAX];
    int n;
    if (!auth(token, pwd, 0)) return;
    n = campus_load_records(token, rec, REC_MAX);
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"records\":[");
    for (int i = 0; i < n; i++) { if (i) j_put(","); put_record(&rec[i]); }
    j_fmt("],\"total\":%d}", n);
}
/* 机房上机统计 */
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
/* 交易流水（学生查自己的；管理员可查全部）。
 * 必须分页：全部流水可能上万条，一次性序列化会超出响应缓冲，
 * 导致客户端收到被截断的畸形 JSON */
#define LEDGER_PAGE 300
static void h_ledger(const char *token, const char *pwd, const char *period,
                     int as_admin, const char *query) {
    static Transaction tx[LEDGER_PAGE];
    char off[16] = "";
    int total, skip, n;
    const char *id;
    if (!auth(token, pwd, as_admin)) return;
    id = as_admin ? "" : token;

    q_get(query, "offset", off, sizeof(off));
    skip = *off ? atoi(off) : 0;
    if (skip < 0) skip = 0;

    total = campus_count_ledger(id, -1, period_start(period));
    n = campus_load_ledger_page(id, -1, period_start(period), skip, tx, LEDGER_PAGE);

    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,");
    kv_i("total", total);                 j_put(",");
    kv_i("offset", skip);                 j_put(",");
    kv_i("limit", LEDGER_PAGE);           j_put(",");
    kv_i("count", n);                     j_put(",");
    /* 是否还有下一页，便于前端做「加载更多」 */
    j_put("\"hasMore\":"); j_put((skip + n < total) ? "true" : "false");
    j_put(",\"records\":[");
    for (int i = 0; i < n; i++) { if (i) j_put(","); put_tx(&tx[i]); }
    j_put("]}");
}
/* 消费统计 */
static void h_consume(const char *token, const char *pwd, const char *period) {
    double total = 0.0;
    int n;
    if (!auth(token, pwd, 0)) return;
    n = campus_sum_consume(token, period_start(period), &total);
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,");
    kv_s("period", period); j_put(",");
    kv_i("count", n);       j_put(",");
    kv_d("amount", total);
    j_put("}");
}
/* 商户列表 */
static void h_merchants(void) {
    const char *names[MERCHANT_MAX];
    int n = campus_merchants(names, MERCHANT_MAX);
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"merchants\":[");
    for (int i = 0; i < n; i++) { if (i) j_put(","); j_str(names[i]); }
    j_put("]}");
}
/* 机房上机 / 下机 */
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
/* 消费扣款：需要交易密码（X-PayPwd 或 body.payPwd） */
static void h_pay(const char *token, const char *pwd, const char *pay_pwd,
                  const char *body) {
    Card *k;
    char merchant[24] = "", err[64];
    double amount;
    if (!auth(token, pwd, 0)) return;
    k = campus_find(token);
    j_get_str(body, "merchant", merchant, sizeof(merchant));
    amount = j_get_num(body, "amount", 0);
    /* 交易密码优先取请求体 payPwd；头部的 X-PayPwd 已在路由层取好。
     * 注意用局部缓冲，不能写回 const 形参指向的内存 */
    {
        const char *use_pwd = pay_pwd;
        char body_pwd[20] = "";
        if (!*use_pwd && j_get_str(body, "payPwd", body_pwd, sizeof(body_pwd)))
            use_pwd = body_pwd;

        if (campus_pay(k, merchant, amount, use_pwd, "WEB-POS", err, sizeof(err))) {
            res_err(err);
            return;
        }
    }
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"message\":"); j_str("消费成功");
    j_fmt(",\"amount\":%.2f,\"card\":", amount); put_card(k);
    j_put("}");
}
/* 学生自助挂失 / 解除挂失 */
static void h_self_lost(const char *token, const char *pwd, int unlost) {
    Card *k;
    char err[64];
    if (!auth(token, pwd, 0)) return;
    k = campus_find(token);
    if (unlost) {
        if (campus_unlost(k, err, sizeof(err))) { res_err(err); return; }
    } else {
        if (campus_set_lost(k, err, sizeof(err))) { res_err(err); return; }
    }
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"message\":");
    j_str(unlost ? "已解除挂失" : "挂失成功，该卡不能再消费与上机");
    j_put(",\"card\":"); put_card(k); j_put("}");
}
/* 学生修改登录密码 / 交易密码 */
static void h_change_pwd(const char *token, const char *pwd, const char *body,
                         int pay) {
    Card *k;
    char np[20] = "", err[64];
    if (!auth(token, pwd, 0)) return;
    k = campus_find(token);
    j_get_str(body, "newPwd", np, sizeof(np));
    if ((pay ? campus_change_pay_pwd : campus_change_pwd)(k, pwd, np, err, sizeof(err))) {
        res_err(err);
        return;
    }
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"message\":");
    j_str(pay ? "交易密码修改成功" : "登录密码修改成功");
    j_put("}");
}
/* 门禁 / 考勤刷卡（管理员代刷，或学生签到自己） */
static void h_access(const char *token, const char *pwd, const char *body,
                     int as_admin) {
    Card *k;
    char id[10] = "", place[24] = "", err[64];
    int dir, kind;
    if (!auth(token, pwd, as_admin)) return;
    if (as_admin) {
        j_get_str(body, "id", id, sizeof(id));
        k = campus_find(id);
        if (!k) { res_err("找不到该卡号"); return; }
    } else {
        k = campus_find(token);
    }
    j_get_str(body, "place", place, sizeof(place));
    dir  = (int)j_get_num(body, "dir", ACC_IN);
    kind = (int)j_get_num(body, "kind", ACCESS_DOOR);
    if (campus_access(k, dir, kind, place, err, sizeof(err))) { res_err(err); return; }
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"message\":");
    j_str(kind == ACCESS_DOOR ? "门禁刷卡成功" : "考勤签到成功");
    j_put("}");
}
/* 门禁记录 */
static void h_access_log(const char *token, const char *pwd, const char *id,
                         int as_admin) {
    /* 同样限制条数：门禁记录也可能上千条，全量序列化会超出响应缓冲 */
    static AccessLog logs[ACCESS_PAGE];
    int n, truncated = 0, total;
    if (!auth(token, pwd, as_admin)) return;
    total = campus_count_access(as_admin ? id : token);
    n = campus_load_access(as_admin ? id : token, logs, ACCESS_PAGE, &truncated);
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,");
    kv_i("total", total);     j_put(",");
    kv_i("count", n);         j_put(",");
    j_put("\"truncated\":"); j_put(truncated ? "true" : "false");
    j_put(",\"records\":[");
    for (int i = 0; i < n; i++) { if (i) j_put(","); put_access(&logs[i]); }
    j_put("]}");
}

/* ============================================================
 *                      管理员接口
 * ============================================================ */
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
    kv_i("cardMax", campus_card_max()); j_put(",");
    /* 载入时是否有卡片因超过上限被丢弃，前端据此提示用户 */
    j_put("\"cardTruncated\":"); j_put(campus_card_truncated() ? "true" : "false");
    j_put(",");
    kv_d("remainTotal", remain);     j_put(",");
    kv_d("rechargeTotal", recharge); j_put(",");
    kv_i("onlineCount", online);
    j_put("}");
}
/* 当天报表：机房收入 + 全站汇总 + 各消费点 */
static void h_admin_report(const char *token, const char *pwd) {
    double fee = 0.0, consume = 0.0, recharge = 0.0;
    double msum[MERCHANT_MAX];
    const char *names[MERCHANT_MAX];
    int times = 0, ct = 0, mn;
    char today[12];
    time_t t = time(NULL);
    if (!auth(token, pwd, 1)) return;

    campus_today_income(&fee, &times);
    campus_today_report(&consume, &ct, &recharge, msum);
    mn = campus_merchants(names, MERCHANT_MAX);
    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&t));

    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,");
    kv_s("day", today);            j_put(",");
    kv_d("feeIncome", fee);        j_put(",");
    kv_i("machineTimes", times);   j_put(",");
    kv_d("consumeTotal", consume); j_put(",");
    kv_i("consumeTimes", ct);      j_put(",");
    kv_d("rechargeTotal", recharge); j_put(",");
    j_put("\"merchants\":[");
    for (int i = 0; i < mn; i++) {
        if (i) j_put(",");
        j_put("{\"name\":"); j_str(names[i]);
        j_fmt(",\"amount\":%.2f}", msum[i]);
    }
    j_put("]}");
}
/* 兼容旧接口：仅当天机房收入 */
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
static void h_admin_records(const char *token, const char *pwd, const char *id) {
    static Record rec[REC_MAX];
    int n;
    if (!auth(token, pwd, 1)) return;
    if (!*id) { res_err("请提供卡号参数 id"); return; }
    if (!campus_find(id)) { res_err("找不到该卡号"); return; }
    n = campus_load_records(id, rec, REC_MAX);
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"records\":[");
    for (int i = 0; i < n; i++) { if (i) j_put(","); put_record(&rec[i]); }
    j_fmt("],\"total\":%d}", n);
}
/* 新卡注册（管理员或自助注册） */
static void h_register(const char *body) {
    char id[10] = "", name[12] = "", number[12] = "", pwd[20] = "", err[64];
    j_get_str(body, "id", id, sizeof(id));
    j_get_str(body, "name", name, sizeof(name));
    j_get_str(body, "number", number, sizeof(number));
    j_get_str(body, "pwd", pwd, sizeof(pwd));
    if (campus_register(id, name, number, pwd, err, sizeof(err))) {
        res_err(err);
        return;
    }
    j_init(g_buf, g_cap);
    j_put("{\"ok\":true,\"message\":"); j_str("注册成功");
    j_put(",\"card\":"); put_card(campus_find(id)); j_put("}");
}
/* 管理员卡务操作：op 取自路径末段 */
static void h_admin_op(const char *token, const char *pwd, const char *body,
                       const char *op) {
    char id[10] = "", err[64], date[12] = "";
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
    } else if (!strcmp(op, "unlost")) {
        if (campus_unlost(k, err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(op, "freeze")) {
        if (campus_freeze(k, (int)j_get_num(body, "on", 1), err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(op, "reset")) {
        if (campus_reset_pwd(k, err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(op, "resetpay")) {
        if (campus_reset_pay_pwd(k, err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(op, "reissue")) {
        double fee = j_get_num(body, "fee", 0);
        if (campus_reissue(k, fee, err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(op, "limits")) {
        double once = j_get_num(body, "limitOnce", 0);
        double day  = j_get_num(body, "limitDay", 0);
        if (campus_set_limits(k, once, day, err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(op, "expire")) {
        j_get_str(body, "date", date, sizeof(date));
        if (campus_set_expire(k, date, err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(op, "grant")) {
        double amount = j_get_num(body, "amount", 0);
        char memo[32] = "";
        int type = (int)j_get_num(body, "type", TX_GRANT);
        j_get_str(body, "memo", memo, sizeof(memo));
        if (campus_grant(k, type, amount, memo, err, sizeof(err))) { res_err(err); return; }
    } else if (!strcmp(op, "pay")) {
        /* 管理端代扣：模拟商户 POS。金额与交易密码由持卡人在终端输入，
         * 服务端仍按一卡通规则校验密码、限额与余额 */
        char merchant[24] = "", pay[20] = "", dev[16] = "";
        double amount = j_get_num(body, "amount", 0);
        j_get_str(body, "merchant", merchant, sizeof(merchant));
        j_get_str(body, "payPwd", pay, sizeof(pay));
        j_get_str(body, "device", dev, sizeof(dev));
        if (campus_pay(k, merchant, amount, pay, *dev ? dev : "ADMIN-POS",
                       err, sizeof(err))) {
            res_err(err);
            return;
        }
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
                  const char *body, const char *pay_hdr,
                  char *res, size_t cap) {
    char token[16] = "", pwd[20] = "", period[12] = "", id[10] = "", pay[20] = "";
    int post = !strcmp(method, "POST");

    j_init(res, cap);
    q_get(query, "token", token, sizeof(token));
    q_get(query, "pwd", pwd, sizeof(pwd));
    q_get(query, "payPwd", pay, sizeof(pay));
    if (!*pay) snprintf(pay, sizeof(pay), "%s", pay_hdr);

    /* 前端定时调用，让桌面启动器感知应用窗口是否仍然打开 */
    if (!strcmp(path, "/api/beat")) {
        g_last_beat = time(NULL);
        j_put("{\"ok\":true}");
        return;
    }
    /* 启动器查询：距上次心跳过去多少秒；从未收到过则为 -1 */
    if (!strcmp(path, "/api/beat/age")) {
        j_put("{\"ok\":true,");
        kv_i("age", g_last_beat ? (long)(time(NULL) - g_last_beat) : -1L);
        j_put("}");
        return;
    }
    if (!strcmp(path, "/api/health")) {
        j_put("{\"ok\":true,\"service\":\"campus-card\"");
        j_fmt(",\"cards\":%d,\"rate\":%.2f", campus_count(), RATE_PER_HOUR);
        j_fmt(",\"cardMax\":%d", campus_card_max());
        j_put(",\"cardTruncated\":");
        j_put(campus_card_truncated() ? "true" : "false");
        j_put("}");
        return;
    }
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
    if (!strcmp(path, "/api/register") && post) { h_register(body); return; }
    if (!strcmp(path, "/api/merchants"))         { h_merchants();   return; }

    /* 学生端 */
    if (!strcmp(path, "/api/card"))    { h_card(token, pwd);    return; }
    if (!strcmp(path, "/api/records")) { h_records(token, pwd); return; }
    if (!strcmp(path, "/api/stats")) {
        q_get(query, "period", period, sizeof(period));
        h_stats(token, pwd, *period ? period : "day");
        return;
    }
    if (!strcmp(path, "/api/ledger")) {
        q_get(query, "period", period, sizeof(period));
        h_ledger(token, pwd, *period ? period : "all", 0, query);
        return;
    }
    if (!strcmp(path, "/api/consume")) {
        q_get(query, "period", period, sizeof(period));
        h_consume(token, pwd, *period ? period : "day");
        return;
    }
    if (!strcmp(path, "/api/access")) {
        if (post) h_access(token, pwd, body, 0);
        else      h_access_log(token, pwd, "", 0);
        return;
    }
    if (!strcmp(path, "/api/machine") && post) { h_machine(token, pwd, body); return; }
    if (!strcmp(path, "/api/pay")     && post) { h_pay(token, pwd, pay, body); return; }
    if (!strcmp(path, "/api/lost")    && post) { h_self_lost(token, pwd, 0);   return; }
    if (!strcmp(path, "/api/unlost")  && post) { h_self_lost(token, pwd, 1);   return; }
    if (!strcmp(path, "/api/pwd")     && post) { h_change_pwd(token, pwd, body, 0); return; }
    if (!strcmp(path, "/api/paypwd")  && post) { h_change_pwd(token, pwd, body, 1); return; }

    /* 管理员端 */
    if (!strcmp(path, "/api/admin/cards"))  { h_admin_cards(token, pwd);  return; }
    if (!strcmp(path, "/api/admin/income")) { h_admin_income(token, pwd); return; }
    if (!strcmp(path, "/api/admin/report")) { h_admin_report(token, pwd); return; }
    if (!strcmp(path, "/api/admin/ledger")) {
        q_get(query, "period", period, sizeof(period));
        h_ledger(token, pwd, *period ? period : "day", 1, query);
        return;
    }
    if (!strcmp(path, "/api/admin/access")) {
        q_get(query, "id", id, sizeof(id));
        if (post) {
            /* 管理员代刷卡时复用统一处理，需要从 body 取卡号 */
            h_access(token, pwd, body, 1);
        } else {
            h_access_log(token, pwd, id, 1);
        }
        return;
    }
    if (!strcmp(path, "/api/admin/records")) {
        q_get(query, "id", id, sizeof(id));
        h_admin_records(token, pwd, id);
        return;
    }
    if (!strncmp(path, "/api/admin/", 11) && post) {
        h_admin_op(token, pwd, body, path + 11);
        return;
    }
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
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(dot, ".webmanifest"))
        return "application/manifest+json; charset=utf-8";
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
    char head[420];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
             /* PWA：允许 Service Worker 从站点根路径注册并控制整个站点范围 */
             "Service-Worker-Allowed: /\r\n"
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
    if (!fp && !strchr(rel + 1, '.')) {
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

/* ============================================================
 *                   连接处理与主函数
 * ============================================================ */
static void handle_conn(sock_t fd) {
    char *req = (char *)malloc(REQ_MAX);
    char *res = (char *)malloc(RES_MAX);
    size_t total = 0;
    int ready = 0;
    char method[8] = "", path[256] = "", query[320] = "", pay_hdr[20] = "";
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

    /* 3. 令牌、密码与交易密码优先取请求头（头名大小写不敏感） */
    {
        char v[32];
        if (header_value(req, "X-Token", v, sizeof(v)))
            snprintf(query + strlen(query), sizeof(query) - strlen(query),
                     "%stoken=%s", *query ? "&" : "", v);
        if (header_value(req, "X-Pwd", v, sizeof(v)))
            snprintf(query + strlen(query), sizeof(query) - strlen(query),
                     "%spwd=%s", *query ? "&" : "", v);
        header_value(req, "X-PayPwd", pay_hdr, sizeof(pay_hdr));
    }

    /* 4. 接口或静态文件 */
    if (!strncmp(path, "/api/", 5)) {
        size_t len;
        route(method, path, query, body, pay_hdr, res, RES_MAX);
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

int main(int argc, char **argv) {
    int port = 8090;
#ifdef _WIN32
    SetConsoleOutputCP(65001);      /* 保证启动信息里的中文不乱码 */
#endif
    const char *env_port = getenv("PORT");
    const char *host = "0.0.0.0";
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
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("端口 %d 绑定失败\n", port);
        return 1;
    }
    listen(srv, 32);

    printf("校园一卡通服务已启动 http://%s:%d/  卡片 %d 张  机房费率 %.2f 元/小时\n",
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
