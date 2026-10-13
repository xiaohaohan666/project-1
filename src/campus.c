/* ============================================================
 *  模拟校园卡 —— 业务核心层实现
 * ------------------------------------------------------------
 *  设计要点：
 *    · 卡片一次载入内存（全局状态），改动后整体写回 cards.dat
 *    · 上机记录按卡分文件、追加写入，永不覆盖（<卡号>.txt）
 *    · 收入流水单独记录（income.dat），供统计当天收入
 *    · 计费：费率 RATE_PER_HOUR 元/小时，按实际分钟折算，保留两位小数
 *    · 无会话状态：Web 端每次请求携带卡号与密码做校验，便于多端共用
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "campus.h"

/* ---------------- 全局状态 ---------------- */
static Card g_cards[CARD_MAX];              /* 全部卡片               */
static int  g_n_card;                       /* 卡片数量               */
static char g_income_day[INCOME_MAX][12];   /* 收入流水：日期         */
static double g_income_amt[INCOME_MAX];     /* 收入流水：金额（元）   */
static int  g_n_income;                     /* 收入流水条数           */
static char g_dir[200];                     /* 数据目录               */

/* ---------------- 内部工具 ---------------- */

/* 拼出数据目录下的完整路径。name 文件名。返回静态缓冲区（下次调用被覆盖） */
static const char *path_of(const char *name) {
    static char buf[320];
    if (g_dir[0]) snprintf(buf, sizeof(buf), "%s/%s", g_dir, name);
    else          snprintf(buf, sizeof(buf), "%s", name);
    return buf;
}

/* 取当前时间并按 fmt 格式化。buf 输出缓冲；size 缓冲大小；fmt 格式串 */
static void now_fmt(char *buf, int size, const char *fmt) {
    time_t t = time(NULL);
    strftime(buf, (size_t)size, fmt, localtime(&t));
}

/* 解析 "YYYY-MM-DD HH:MM:SS" 为 time_t；失败返回 (time_t)-1 */
static time_t parse_time(const char *s) {
    struct tm tm;
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
        return (time_t)-1;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = se;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* 统一错误返回：写原因并返回 -1。err 缓冲；esz 大小；msg 原因 */
static int fail(char *err, int esz, const char *msg) {
    if (err && esz > 0) snprintf(err, (size_t)esz, "%s", msg);
    return -1;
}

/* 追加一条收入流水并落盘。amount 金额（元） */
static void add_income(double amount) {
    FILE *fp;
    if (g_n_income >= INCOME_MAX) {         /* 满则丢弃最早一条 */
        memmove(g_income_day[0], g_income_day[1],
                sizeof(g_income_day[0]) * (INCOME_MAX - 1));
        memmove(&g_income_amt[0], &g_income_amt[1],
                sizeof(g_income_amt[0]) * (INCOME_MAX - 1));
        g_n_income = INCOME_MAX - 1;
    }
    now_fmt(g_income_day[g_n_income], 12, "%Y-%m-%d");
    g_income_amt[g_n_income] = amount;
    g_n_income++;

    fp = fopen(path_of("income.dat"), "a");     /* 追加，不覆盖历史 */
    if (fp) {
        fprintf(fp, "%s|%.2f\n", g_income_day[g_n_income - 1], amount);
        fclose(fp);
    }
}

/* ---------------- 载入与保存 ---------------- */

/* 保存全部卡片到 cards.dat。成功返回 1 */
int campus_save(void) {
    FILE *fp = fopen(path_of("cards.dat"), "w");
    if (!fp) return 0;
    for (int i = 0; i < g_n_card; i++) {
        Card *k = &g_cards[i];
        fprintf(fp, "%s|%s|%s|%s|%.2f|%.2f|%d|%d|%s|%s\n",
                k->id, k->name, k->number, k->pwd, k->recharge, k->remain,
                k->state, k->online, k->start_time, k->address);
    }
    fclose(fp);
    return 1;
}

/* 读取 cards.dat 与 income.dat。返回读到的卡片数 */
static int campus_load(void) {
    char line[256];
    FILE *fp = fopen(path_of("cards.dat"), "r");

    if (fp) {
        while (g_n_card < CARD_MAX && fgets(line, sizeof(line), fp)) {
            Card k;
            memset(&k, 0, sizeof(k));
            /* 前七个字段必填，上机状态/时间/地点允许为空 */
            if (sscanf(line, "%9[^|]|%11[^|]|%11[^|]|%19[^|]|%lf|%lf|%d|%d|%19[^|]|%23[^\n]",
                       k.id, k.name, k.number, k.pwd, &k.recharge, &k.remain,
                       &k.state, &k.online, k.start_time, k.address) < 7)
                continue;
            g_cards[g_n_card++] = k;
        }
        fclose(fp);
    }

    fp = fopen(path_of("income.dat"), "r");
    if (fp) {
        while (g_n_income < INCOME_MAX && fgets(line, sizeof(line), fp)) {
            char day[12];
            double amount;
            if (sscanf(line, "%11[^|]|%lf", day, &amount) != 2) continue;
            snprintf(g_income_day[g_n_income], sizeof(g_income_day[0]), "%s", day);
            g_income_amt[g_n_income] = amount;
            g_n_income++;
        }
        fclose(fp);
    }
    return g_n_card;
}

/* 建立三张演示卡片（首次运行） */
static void seed_cards(void) {
    static const Card demo[3] = {
        { "C001", "张三", "B17010101", DEFAULT_PWD, 50.0, 50.0, ST_NORMAL, ONLINE_NO, "", "" },
        { "C002", "李四", "B17010102", DEFAULT_PWD, 30.0, 30.0, ST_NORMAL, ONLINE_NO, "", "" },
        { "C003", "王五", "B17010103", DEFAULT_PWD,  0.0,  0.0, ST_NORMAL, ONLINE_NO, "", "" }
    };
    for (int i = 0; i < 3; i++) g_cards[i] = demo[i];
    g_n_card = 3;
    campus_save();
}

/* 初始化。data_dir 数据目录。成功返回 1 */
int campus_init(const char *data_dir) {
    size_t n;
    memset(g_cards, 0, sizeof(g_cards));
    memset(g_income_day, 0, sizeof(g_income_day));
    memset(g_income_amt, 0, sizeof(g_income_amt));
    g_n_card = g_n_income = 0;

    snprintf(g_dir, sizeof(g_dir), "%s", data_dir ? data_dir : "");
    n = strlen(g_dir);
    while (n > 1 && (g_dir[n - 1] == '/' || g_dir[n - 1] == '\\')) g_dir[--n] = '\0';

    if (campus_load() == 0) seed_cards();       /* 无数据则建立演示卡片 */
    return 1;
}

/* ---------------- 查询 ---------------- */

/* 卡片总数 */
int campus_count(void) { return g_n_card; }

/* 取第 i 张卡片；越界返回 NULL */
Card *campus_at(int i) {
    return (i >= 0 && i < g_n_card) ? &g_cards[i] : NULL;
}

/* 按卡号查找卡片；找不到返回 NULL */
Card *campus_find(const char *id) {
    for (int i = 0; i < g_n_card; i++)
        if (!strcmp(g_cards[i].id, id)) return &g_cards[i];
    return NULL;
}

/* 状态文字 */
const char *campus_state_text(int state) {
    static const char *text[3] = { "正常", "挂失", "冻结" };
    return (state >= 0 && state < 3) ? text[state] : "未知";
}

/* 校验学生密码；正确返回 1 */
int campus_check_pwd(const char *id, const char *pwd) {
    Card *k = campus_find(id);
    return k && !strcmp(k->pwd, pwd);
}

/* 读取某卡上机记录文件，返回条数 */
int campus_load_records(const char *id, Record *out, int max) {
    char name[32], line[192];
    FILE *fp;
    int n = 0;

    snprintf(name, sizeof(name), "%s.txt", id);
    fp = fopen(path_of(name), "r");
    if (!fp) return 0;

    while (n < max && fgets(line, sizeof(line), fp)) {
        Record r;
        memset(&r, 0, sizeof(r));
        if (sscanf(line, "%9[^|]|%11[^|]|%9[^|]|%d|%lf|%11[^|]|%23[^\n]",
                   r.id, r.date, r.start_time, &r.duration, &r.cost,
                   r.state, r.address) < 7)
            continue;
        out[n++] = r;
    }
    fclose(fp);
    return n;
}

/* 统计某卡 [from, 现在] 的上机情况；from 为 0 表示全部 */
int campus_stats(const char *id, time_t from,
                 int *out_count, int *out_minutes, double *out_cost) {
    static Record rec[REC_MAX];             /* 静态数组，避免占用栈空间 */
    int n = campus_load_records(id, rec, REC_MAX);
    int count = 0, minutes = 0;
    double cost = 0.0;

    for (int i = 0; i < n; i++) {
        char stamp[24];
        time_t t;
        snprintf(stamp, sizeof(stamp), "%s %s", rec[i].date, rec[i].start_time);
        t = parse_time(stamp);
        if (from && t != (time_t)-1 && t < from) continue;
        count++;
        minutes += rec[i].duration;
        cost += rec[i].cost;
    }
    if (out_count)   *out_count = count;
    if (out_minutes) *out_minutes = minutes;
    if (out_cost)    *out_cost = cost;
    return count;
}

/* ---------------- 业务动作 ---------------- */

/* 登录校验：管理员比对 ADMIN_PWD，学生比对该卡密码。成功返回 1 */
int campus_login(const char *id, const char *pwd, int as_admin) {
    if (as_admin) return !strcmp(pwd, ADMIN_PWD);
    return campus_check_pwd(id, pwd);
}

/* 充值：增加余额与累计充值数额，并记一条收入 */
int campus_recharge(Card *card, double amount, char *err, int esz) {
    if (amount <= 0) return fail(err, esz, "充值金额必须大于 0");
    card->remain   += amount;
    card->recharge += amount;
    add_income(amount);
    campus_save();
    return 0;
}

/* 开始上机：登记开始时间与地点 */
int campus_online(Card *card, const char *address, char *err, int esz) {
    if (card->state == ST_LOST)     return fail(err, esz, "该卡已挂失，不能上机");
    if (card->state == ST_FROZEN)   return fail(err, esz, "该卡已冻结，不能上机");
    if (card->online == ONLINE_YES) return fail(err, esz, "该卡已在上机中");
    if (card->remain <= 0)          return fail(err, esz, "卡内余额不足，请先充值");
    if (!address || !*address)      return fail(err, esz, "请选择上机地点");

    snprintf(card->address, sizeof(card->address), "%s", address);
    now_fmt(card->start_time, sizeof(card->start_time), "%Y-%m-%d %H:%M:%S");
    card->online = ONLINE_YES;
    campus_save();
    return 0;
}

/* 追加一条上机记录到 <卡号>.txt（永久保存） */
static void append_record(const Card *card, const Record *r) {
    char name[32];
    FILE *fp;
    snprintf(name, sizeof(name), "%s.txt", card->id);
    fp = fopen(path_of(name), "a");         /* 追加：历史记录不被覆盖 */
    if (!fp) return;
    fprintf(fp, "%s|%s|%s|%d|%.2f|%s|%s\n", r->id, r->date, r->start_time,
            r->duration, r->cost, r->state, r->address);
    fclose(fp);
}

/* 下机结账：按用时计费、扣余额、写记录并记收入 */
int campus_offline(Card *card, double *out_cost, char *err, int esz) {
    Record r;
    char end_time[20];
    time_t t1, t2;
    double minutes, cost;

    if (card->online != ONLINE_YES) return fail(err, esz, "该卡当前未上机");

    /* 1. 用时与费用 */
    now_fmt(end_time, sizeof(end_time), "%Y-%m-%d %H:%M:%S");
    t1 = parse_time(card->start_time);
    t2 = parse_time(end_time);
    minutes = (t1 == (time_t)-1) ? 0.0 : difftime(t2, t1) / 60.0;
    if (minutes < 0) minutes = 0;
    cost = minutes / 60.0 * RATE_PER_HOUR;
    cost = floor(cost * 100 + 0.5) / 100.0;

    /* 2. 扣费（余额不足则清零，视为欠费） */
    if (card->remain >= cost) card->remain -= cost;
    else                      card->remain = 0.0;

    /* 3. 组装并追加上机记录 */
    memset(&r, 0, sizeof(r));
    snprintf(r.id, sizeof(r.id), "%s", card->id);
    snprintf(r.date, sizeof(r.date), "%.10s", card->start_time);
    snprintf(r.start_time, sizeof(r.start_time), "%.8s", card->start_time + 11);
    r.duration = (int)minutes;
    r.cost = cost;
    snprintf(r.state, sizeof(r.state), "%s", "已下机");
    snprintf(r.address, sizeof(r.address), "%s", card->address);
    append_record(card, &r);

    /* 4. 记收入并清除上机状态 */
    add_income(cost);
    card->online = ONLINE_NO;
    card->start_time[0] = '\0';
    card->address[0] = '\0';
    campus_save();

    if (out_cost) *out_cost = cost;
    return 0;
}

/* 挂失：正在上机则先自动结账，避免费用无上限累积 */
int campus_set_lost(Card *card, char *err, int esz) {
    double cost = 0.0;
    if (card->state == ST_LOST)   return fail(err, esz, "该卡已处于挂失状态");
    if (card->state == ST_FROZEN) return fail(err, esz, "该卡已冻结，不能挂失");
    if (card->online == ONLINE_YES) campus_offline(card, &cost, NULL, 0);
    card->state = ST_LOST;
    campus_save();
    return 0;
}

/* 重置学生密码为默认值，并解除冻结（挂失状态保持不变） */
int campus_reset_pwd(Card *card, char *err, int esz) {
    (void)err; (void)esz;
    snprintf(card->pwd, sizeof(card->pwd), "%s", DEFAULT_PWD);
    if (card->state == ST_FROZEN) card->state = ST_NORMAL;
    campus_save();
    return 0;
}

/* 学生修改密码：校验原密码，新密码不少于 6 位 */
int campus_change_pwd(Card *card, const char *old_pwd, const char *new_pwd,
                      char *err, int esz) {
    if (strcmp(card->pwd, old_pwd)) return fail(err, esz, "原密码错误");
    if (strlen(new_pwd) < 6)        return fail(err, esz, "新密码不得少于 6 位");
    snprintf(card->pwd, sizeof(card->pwd), "%s", new_pwd);
    campus_save();
    return 0;
}

/* 当天收入：逐卡读上机记录，按日期等于今天累加 */
void campus_today_income(double *out_fee, int *out_times) {
    char today[12];
    int times = 0;
    double fee = 0.0;

    now_fmt(today, sizeof(today), "%Y-%m-%d");
    for (int i = 0; i < g_n_card; i++) {
        static Record rec[REC_MAX];
        int n = campus_load_records(g_cards[i].id, rec, REC_MAX);
        for (int j = 0; j < n; j++) {
            if (strcmp(rec[j].date, today)) continue;
            fee += rec[j].cost;
            times++;
        }
    }
    if (out_fee)   *out_fee = fee;
    if (out_times) *out_times = times;
}
