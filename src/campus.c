/* ============================================================
 *  校园一卡通 —— 业务核心层实现
 * ------------------------------------------------------------
 *  设计要点：
 *    · 卡片一次载入内存（全局状态），改动后整体写回 cards.dat
 *    · 交易总账 ledger.dat 采用「追加写」，历史流水永不覆盖
 *    · 门禁记录 access.dat 同样追加写
 *    · 机房上机记录仍按卡分文件（<卡号>.txt），保留原题目要求
 *    · 消费统一走 campus_pay()：集中处理状态、交易密码、单笔与日累计限额，
 *      并把扣款与记账放在一起，便于审计
 *    · 计费：机房费率 RATE_PER_HOUR 元/小时，按实际分钟折算，保留两位小数
 *    · 无会话状态：Web 端每次请求携带卡号与密码做校验，便于多端共用
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#ifdef _WIN32
#  include <direct.h>            /* _mkdir：Windows 下创建目录 */
#else
#  include <sys/stat.h>          /* mkdir：Linux 下创建目录    */
#  include <sys/types.h>
#endif
#include "campus.h"

/* ---------------- 商户 / 消费点表 ---------------- */
/* 前 6 个为消费场景，最后一个为机房（保留原题目要求的上机计费） */
static const char *g_merchants[MERCHANT_MAX] = {
    "食堂", "超市", "文印店", "热水洗衣", "宿舍水电", "图书馆罚款", "机房"
};
static const int g_n_merchant = 7;

/* ---------------- 全局状态 ---------------- */
static Card g_cards[CARD_MAX];              /* 全部卡片               */
static int  g_n_card;                       /* 卡片数量               */
static char g_income_day[INCOME_MAX][12];   /* 收入流水：日期（兼容旧数据） */
static double g_income_amt[INCOME_MAX];     /* 收入流水：金额（元）   */
static int  g_n_income;                     /* 收入流水条数           */
static long g_seq;                          /* 交易流水号自增计数     */
static int  g_card_truncated;               /* 载入时是否因超上限而丢卡 */
static char g_dir[200];                     /* 数据目录               */

/* ---------------- 内部工具 ---------------- */

/* 拼出数据目录下的完整路径。name 文件名。返回静态缓冲区（下次调用被覆盖） */
static const char *path_of(const char *name) {
    static char buf[320];
    if (g_dir[0]) snprintf(buf, sizeof(buf), "%s/%s", g_dir, name);
    else          snprintf(buf, sizeof(buf), "%s", name);
    return buf;
}

/* 取当前时间并按 fmt 格式化 */
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

/* 统一错误返回：写原因并返回 -1 */
static int fail(char *err, int esz, const char *msg) {
    if (err && esz > 0) snprintf(err, (size_t)esz, "%s", msg);
    return -1;
}

/* 确保数据目录存在，不存在则创建一层（跨平台）。成功返回 1 */
static int ensure_dir(const char *dir) {
    if (!dir || !*dir) return 1;
#ifdef _WIN32
    if (_mkdir(dir) == 0) return 1;
#else
    if (mkdir(dir, 0755) == 0) return 1;
#endif
    return 0;
}

/* 金额取两位小数（四舍五入） */
static double round2(double v) { return floor(v * 100 + 0.5) / 100.0; }

/* 判断卡片是否已过有效期。空有效期视为长期有效。
 * 已过期时把状态置为 ST_EXPIRED 并返回 1 */
static int check_expire(Card *card) {
    char today[12];
    if (!card->expire[0]) return 0;
    now_fmt(today, sizeof(today), "%Y-%m-%d");
    if (strcmp(today, card->expire) > 0) {      /* 字典序即日期序 */
        card->state = ST_EXPIRED;
        return 1;
    }
    return 0;
}

/* 把今日已消费金额按日期滚动清零（跨天自动重置） */
static void roll_day(Card *card) {
    char today[12];
    now_fmt(today, sizeof(today), "%Y-%m-%d");
    if (strcmp(card->used_date, today)) {
        snprintf(card->used_date, sizeof(card->used_date), "%s", today);
        card->used_day = 0.0;
    }
}

/* 追加一条收入流水（兼容旧的 income.dat，供当天收入统计） */
static void add_income(double amount) {
    FILE *fp;
    if (g_n_income >= INCOME_MAX) {
        memmove(g_income_day[0], g_income_day[1],
                sizeof(g_income_day[0]) * (INCOME_MAX - 1));
        memmove(&g_income_amt[0], &g_income_amt[1],
                sizeof(g_income_amt[0]) * (INCOME_MAX - 1));
        g_n_income = INCOME_MAX - 1;
    }
    now_fmt(g_income_day[g_n_income], 12, "%Y-%m-%d");
    g_income_amt[g_n_income] = amount;
    g_n_income++;

    fp = fopen(path_of("income.dat"), "a");
    if (fp) {
        fprintf(fp, "%s|%.2f\n", g_income_day[g_n_income - 1], amount);
        fclose(fp);
    }
}

/* ---------------- 交易总账：追加写 ---------------- */
/* 追加一条交易流水。card 所属卡；type 类型；merchant 商户；
 * amount 金额（正数）；device 终端号（可空）；memo 备注（可空） */
static void add_tx(Card *card, int type, const char *merchant,
                   double amount, const char *device, const char *memo) {
    FILE *fp;
    Transaction tx;

    memset(&tx, 0, sizeof(tx));
    tx.seq = ++g_seq;
    now_fmt(tx.time, sizeof(tx.time), "%Y-%m-%d %H:%M:%S");
    snprintf(tx.id, sizeof(tx.id), "%s", card->id);
    tx.type = type;
    snprintf(tx.merchant, sizeof(tx.merchant), "%s", merchant ? merchant : "");
    tx.amount = round2(amount);
    tx.balance = round2(card->remain);
    snprintf(tx.device, sizeof(tx.device), "%s", device ? device : "");
    snprintf(tx.memo, sizeof(tx.memo), "%s", memo ? memo : "");

    fp = fopen(path_of("ledger.dat"), "a");     /* 追加：历史流水不覆盖 */
    if (!fp) return;
    fprintf(fp, "%ld|%s|%s|%d|%s|%.2f|%.2f|%s|%s\n",
            tx.seq, tx.time, tx.id, tx.type, tx.merchant,
            tx.amount, tx.balance, tx.device, tx.memo);
    fclose(fp);
}

/* 载入交易总账，仅用于恢复流水号自增计数 */
static void load_ledger_seq(void) {
    char line[256];
    FILE *fp = fopen(path_of("ledger.dat"), "r");
    long max_seq = 0;
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        long s = strtol(line, NULL, 10);
        if (s > max_seq) max_seq = s;
    }
    fclose(fp);
    g_seq = max_seq;
}

/* ---------------- 门禁记录：追加写 ---------------- */
static void add_access(Card *card, int dir, int kind, const char *place) {
    FILE *fp = fopen(path_of("access.dat"), "a");
    char t[20];
    if (!fp) return;
    now_fmt(t, sizeof(t), "%Y-%m-%d %H:%M:%S");
    fprintf(fp, "%s|%s|%d|%s|%d\n", card->id, t, dir, place ? place : "", kind);
    fclose(fp);
}

/* ---------------- 卡片读写 ---------------- */
/* 保存全部卡片到 cards.dat。成功返回 1 */
int campus_save(void) {
    FILE *fp = fopen(path_of("cards.dat"), "w");
    if (!fp) return 0;
    for (int i = 0; i < g_n_card; i++) {
        Card *k = &g_cards[i];
        fprintf(fp, "%s|%s|%s|%s|%s|%.2f|%.2f|%d|%d|%s|%s|%.2f|%.2f|%s|%.2f|%s\n",
                k->id, k->name, k->number, k->pwd, k->pay_pwd,
                k->recharge, k->remain, k->state, k->online,
                k->start_time, k->address, k->limit_day, k->used_day,
                k->used_date, k->limit_once, k->expire);
    }
    fclose(fp);
    return 1;
}

/* 按 | 切分一行，取第 idx 个字段（从 0 开始）写入 out。
 * 空字段会得到空串。
 * 注意：不能直接用 sscanf 的 %[^|]——它遇到空字段（两个连续 |）会失败并
 * 中止整个解析，导致后续字段全部读不到（曾造成卡号被错读为交易密码）。 */
static void field_at(const char *line, int idx, char *out, int osz) {
    const char *p = line;
    int i = 0;
    out[0] = '\0';
    while (p) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (i == idx) {
            /* 末字段去掉行尾换行 */
            while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) len--;
            if (len >= (size_t)osz) len = (size_t)osz - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return;
        }
        if (!bar) break;
        p = bar + 1;
        i++;
    }
}

/* 解析整数的辅助：空字段返回默认值 */
static int field_int(const char *line, int idx, int dflt) {
    char buf[24];
    field_at(line, idx, buf, sizeof(buf));
    return buf[0] ? atoi(buf) : dflt;
}
static double field_num(const char *line, int idx, double dflt) {
    char buf[24];
    field_at(line, idx, buf, sizeof(buf));
    return buf[0] ? atof(buf) : dflt;
}

/* 读取 cards.dat 与 income.dat。返回读到的卡片数。
 * 字段下标与写入格式严格对应（0 基）：
 *   0 卡号  1 姓名  2 学号  3 登录密码  4 交易密码  5 累计充值  6 余额
 *   7 状态  8 是否上机  9 开始时间  10 地点  11 日限额  12 今日已用
 *   13 今日日期  14 单笔限额  15 有效期
 * 采用逐字段取值而非 sscanf，因此空字段（如上机中的开始时间与地点）
 * 不会影响后续字段；不足 12 个字段的旧版数据自动补默认值 */
static int campus_load(void) {
    char line[320];
    FILE *fp = fopen(path_of("cards.dat"), "r");

    if (fp) {
        while (g_n_card < CARD_MAX && fgets(line, sizeof(line), fp)) {
            Card k;
            char ext[24];
            memset(&k, 0, sizeof(k));

            /* 先取卡号：为空说明是空行，跳过 */
            field_at(line, 0, k.id, sizeof(k.id));
            if (!k.id[0]) continue;

            field_at(line, 1, k.name, sizeof(k.name));
            field_at(line, 2, k.number, sizeof(k.number));
            field_at(line, 3, k.pwd, sizeof(k.pwd));
            field_at(line, 4, k.pay_pwd, sizeof(k.pay_pwd));
            k.recharge = field_num(line, 5, 0);
            k.remain   = field_num(line, 6, 0);
            k.state    = field_int(line, 7, ST_NORMAL);
            k.online   = field_int(line, 8, ONLINE_NO);
            field_at(line, 9, k.start_time, sizeof(k.start_time));
            field_at(line, 10, k.address, sizeof(k.address));

            /* 第 11 字段起为扩展字段；旧版数据到第 10 字段为止 */
            field_at(line, 11, ext, sizeof(ext));
            if (ext[0]) {
                k.limit_day  = atof(ext);
                k.used_day   = field_num(line, 12, 0);
                field_at(line, 13, k.used_date, sizeof(k.used_date));
                k.limit_once = field_num(line, 14, 0);
                field_at(line, 15, k.expire, sizeof(k.expire));
            } else {
                /* 旧版数据：补默认交易密码与限额 */
                k.limit_day  = 50.0;        /* 合理的默认日限额 */
                k.limit_once = 20.0;
            }

            if (!k.pay_pwd[0])
                snprintf(k.pay_pwd, sizeof(k.pay_pwd), "%s", DEFAULT_PWD);
            if (!k.used_date[0])
                now_fmt(k.used_date, sizeof(k.used_date), "%Y-%m-%d");
            if (k.state < 0 || k.state > ST_EXPIRED) k.state = ST_NORMAL;
            g_cards[g_n_card++] = k;
        }
        /* 若读满上限后文件仍有内容，说明有卡片被丢弃，置位以便上层告警 */
        if (g_n_card >= CARD_MAX && fgets(line, sizeof(line), fp) && line[0])
            g_card_truncated = 1;
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
    load_ledger_seq();
    return g_n_card;
}

/* 建立演示卡片（首次运行）：三张卡，含限额与不同的交易密码 */
static void seed_cards(void) {
    static const Card demo[3] = {
        { "C001", "张三", "B17010101", DEFAULT_PWD, "010101", 50.0, 50.0,
          ST_NORMAL, ONLINE_NO, "", "", 50.0, 0.0, "", 20.0, "2029-12-31" },
        { "C002", "李四", "B17010102", DEFAULT_PWD, "010102", 30.0, 30.0,
          ST_NORMAL, ONLINE_NO, "", "", 30.0, 0.0, "", 15.0, "2029-12-31" },
        { "C003", "王五", "B17010103", DEFAULT_PWD, "010103",  0.0,  0.0,
          ST_NORMAL, ONLINE_NO, "", "", 20.0, 0.0, "", 10.0, "2029-12-31" }
    };
    char today[12];
    now_fmt(today, sizeof(today), "%Y-%m-%d");
    for (int i = 0; i < 3; i++) {
        g_cards[i] = demo[i];
        snprintf(g_cards[i].used_date, sizeof(g_cards[i].used_date), "%s", today);
    }
    g_n_card = 3;
    campus_save();
}

/* 初始化。data_dir 数据目录（不存在会自动创建）。成功返回 1 */
int campus_init(const char *data_dir) {
    size_t n;
    memset(g_cards, 0, sizeof(g_cards));
    memset(g_income_day, 0, sizeof(g_income_day));
    memset(g_income_amt, 0, sizeof(g_income_amt));
    g_n_card = g_n_income = 0;
    g_seq = 0;
    g_card_truncated = 0;

    snprintf(g_dir, sizeof(g_dir), "%s", data_dir ? data_dir : "");
    n = strlen(g_dir);
    while (n > 1 && (g_dir[n - 1] == '/' || g_dir[n - 1] == '\\')) g_dir[--n] = '\0';

    ensure_dir(g_dir);
    if (campus_load() == 0) seed_cards();
    return 1;
}

/* ---------------- 查询 ---------------- */
int campus_count(void) { return g_n_card; }
int campus_card_truncated(void) { return g_card_truncated; }
int campus_card_max(void) { return CARD_MAX; }

Card *campus_at(int i) {
    return (i >= 0 && i < g_n_card) ? &g_cards[i] : NULL;
}

Card *campus_find(const char *id) {
    for (int i = 0; i < g_n_card; i++)
        if (!strcmp(g_cards[i].id, id)) return &g_cards[i];
    return NULL;
}

const char *campus_state_text(int state) {
    static const char *text[4] = { "正常", "挂失", "冻结", "已过期" };
    return (state >= 0 && state < 4) ? text[state] : "未知";
}

const char *campus_type_text(int type) {
    static const char *text[5] = { "充值", "消费", "退款", "工本费", "补助" };
    return (type >= 0 && type < 5) ? text[type] : "其他";
}

int campus_check_pwd(const char *id, const char *pwd) {
    Card *k = campus_find(id);
    return k && !strcmp(k->pwd, pwd);
}

int campus_check_pay_pwd(const char *id, const char *pwd) {
    Card *k = campus_find(id);
    return k && !strcmp(k->pay_pwd, pwd);
}

int campus_merchants(const char **out, int max) {
    int n = g_n_merchant < max ? g_n_merchant : max;
    for (int i = 0; i < n; i++) out[i] = g_merchants[i];
    return n;
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
        field_at(line, 0, r.id, sizeof(r.id));
        if (!r.id[0]) continue;
        field_at(line, 1, r.date, sizeof(r.date));
        field_at(line, 2, r.start_time, sizeof(r.start_time));
        r.duration = field_int(line, 3, 0);
        r.cost     = field_num(line, 4, 0);
        field_at(line, 5, r.state, sizeof(r.state));
        field_at(line, 6, r.address, sizeof(r.address));
        out[n++] = r;
    }
    fclose(fp);
    return n;
}

/* 统计某卡 [from, 现在] 的上机情况；from 为 0 表示全部 */
int campus_stats(const char *id, time_t from,
                 int *out_count, int *out_minutes, double *out_cost) {
    static Record rec[REC_MAX];
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

/* 内部：读取流水并可选跳过前 skip 条。paged 为 1 时按 skip 分页；
 * 为 0 时采用环形保留最近 max 条。
 * 返回值：读到的条数；out_truncated 回传是否发生截断 */
static int ledger_read(const char *id, int type, time_t from,
                       int skip, int paged,
                       Transaction *out, int max, int *out_truncated) {
    char line[256];
    FILE *fp = fopen(path_of("ledger.dat"), "r");
    int n = 0, matched = 0, truncated = 0;

    if (out_truncated) *out_truncated = 0;
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        Transaction t;
        char buf[24];
        memset(&t, 0, sizeof(t));

        field_at(line, 0, buf, sizeof(buf));
        if (!buf[0]) continue;                      /* 空行跳过 */
        t.seq = atol(buf);
        field_at(line, 1, t.time, sizeof(t.time));
        if (!t.time[0]) continue;
        field_at(line, 2, t.id, sizeof(t.id));
        t.type = field_int(line, 3, TX_CONSUME);
        field_at(line, 4, t.merchant, sizeof(t.merchant));
        t.amount  = field_num(line, 5, 0);
        t.balance = field_num(line, 6, 0);
        field_at(line, 7, t.device, sizeof(t.device));
        field_at(line, 8, t.memo, sizeof(t.memo));

        /* 过滤条件 */
        if (id && *id && strcmp(t.id, id)) continue;
        if (type >= 0 && t.type != type) continue;
        if (from) {
            time_t tt = parse_time(t.time);
            if (tt != (time_t)-1 && tt < from) continue;
        }
        matched++;

        if (paged) {
            /* 分页模式：跳过前 skip 条，然后顺序取 max 条 */
            if (matched <= skip) continue;
            if (n < max) out[n++] = t;
            else matched--;                         /* 之后的都不再需要 */
        } else {
            /* 覆盖模式：保留最近 max 条 */
            if (n < max) {
                out[n++] = t;
            } else {
                memmove(&out[0], &out[1], sizeof(Transaction) * (size_t)(max - 1));
                out[max - 1] = t;
                truncated = 1;
            }
        }
        if (paged && n >= max) break;
    }
    fclose(fp);
    if (out_truncated) *out_truncated = truncated;
    return n;
}

/* 读取交易流水（保留最近 max 条） */
int campus_load_ledger(const char *id, int type, time_t from,
                       Transaction *out, int max, int *out_truncated) {
    return ledger_read(id, type, from, 0, 0, out, max, out_truncated);
}

/* 分页读取交易流水：跳过前 skip 条后顺序取 max 条 */
int campus_load_ledger_page(const char *id, int type, time_t from,
                            int skip, Transaction *out, int max) {
    return ledger_read(id, type, from, skip, 1, out, max, NULL);
}

/* 只统计条数，便于前端知道总数并分页 */
int campus_count_ledger(const char *id, int type, time_t from) {
    char line[256];
    FILE *fp = fopen(path_of("ledger.dat"), "r");
    int cnt = 0;

    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        char idbuf[10], timebuf[20], buf[24];
        int t;
        field_at(line, 0, buf, sizeof(buf));
        if (!buf[0]) continue;
        field_at(line, 1, timebuf, sizeof(timebuf));
        if (!timebuf[0]) continue;
        field_at(line, 2, idbuf, sizeof(idbuf));
        t = field_int(line, 3, TX_CONSUME);

        if (id && *id && strcmp(idbuf, id)) continue;
        if (type >= 0 && t != type) continue;
        if (from) {
            time_t tt = parse_time(timebuf);
            if (tt != (time_t)-1 && tt < from) continue;
        }
        cnt++;
    }
    fclose(fp);
    return cnt;
}

/* 读取门禁记录。id 为空表示全部卡；同样只保留最近 max 条 */
int campus_load_access(const char *id, AccessLog *out, int max, int *out_truncated) {
    char line[192];
    FILE *fp = fopen(path_of("access.dat"), "r");
    int n = 0, truncated = 0;

    if (out_truncated) *out_truncated = 0;
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        AccessLog a;
        memset(&a, 0, sizeof(a));
        field_at(line, 0, a.id, sizeof(a.id));
        if (!a.id[0]) continue;
        field_at(line, 1, a.time, sizeof(a.time));
        a.dir   = field_int(line, 2, ACC_IN);
        field_at(line, 3, a.place, sizeof(a.place));
        a.kind  = field_int(line, 4, ACCESS_DOOR);

        if (id && *id && strcmp(a.id, id)) continue;
        if (n < max) {
            out[n++] = a;
        } else {
            memmove(&out[0], &out[1], sizeof(AccessLog) * (size_t)(max - 1));
            out[max - 1] = a;
            truncated = 1;
        }
    }
    fclose(fp);
    if (out_truncated) *out_truncated = truncated;
    return n;
}

/* 只统计门禁记录条数 */
int campus_count_access(const char *id) {
    char line[192];
    FILE *fp = fopen(path_of("access.dat"), "r");
    int cnt = 0;

    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        char idbuf[10];
        field_at(line, 0, idbuf, sizeof(idbuf));
        if (!idbuf[0]) continue;
        if (id && *id && strcmp(idbuf, id)) continue;
        cnt++;
    }
    fclose(fp);
    return cnt;
}

/* 汇总某卡 [from, 现在] 的消费（只统计 TX_CONSUME）。
 * 顺序扫描边读边累加，不把流水读进数组——省掉 1MB 级缓冲 */
int campus_sum_consume(const char *id, time_t from, double *out_total) {
    char line[256], idbuf[10], timebuf[20], buf[24];
    FILE *fp = fopen(path_of("ledger.dat"), "r");
    int cnt = 0;
    double total = 0.0;

    if (out_total) *out_total = 0.0;
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        field_at(line, 0, buf, sizeof(buf));
        if (!buf[0]) continue;
        field_at(line, 1, timebuf, sizeof(timebuf));
        if (!timebuf[0]) continue;
        field_at(line, 2, idbuf, sizeof(idbuf));
        if (id && *id && strcmp(idbuf, id)) continue;
        if (field_int(line, 3, TX_CONSUME) != TX_CONSUME) continue;
        if (from) {
            time_t tt = parse_time(timebuf);
            if (tt != (time_t)-1 && tt < from) continue;
        }
        total += field_num(line, 5, 0);
        cnt++;
    }
    fclose(fp);
    if (out_total) *out_total = round2(total);
    return cnt;
}

/* ---------------- 登录与注册 ---------------- */
int campus_login(const char *id, const char *pwd, int as_admin) {
    if (as_admin) return !strcmp(pwd, ADMIN_PWD);
    return campus_check_pwd(id, pwd);
}

/* 新卡注册。交易密码默认取学号末 6 位（不足 6 位则补 DEFAULT_PWD） */
int campus_register(const char *id, const char *name, const char *number,
                    const char *pwd, char *err, int esz) {
    Card *k;
    if (!id || strlen(id) < 2)       return fail(err, esz, "卡号长度不足");
    if (campus_find(id))             return fail(err, esz, "该卡号已存在");
    if (g_n_card >= CARD_MAX)        return fail(err, esz, "已达发卡上限");
    if (!name || !*name)             return fail(err, esz, "姓名不能为空");
    if (!number || !*number)         return fail(err, esz, "学号不能为空");
    if (!pwd || strlen(pwd) < 6)      return fail(err, esz, "登录密码不得少于 6 位");

    k = &g_cards[g_n_card++];
    memset(k, 0, sizeof(*k));
    snprintf(k->id, sizeof(k->id), "%s", id);
    snprintf(k->name, sizeof(k->name), "%s", name);
    snprintf(k->number, sizeof(k->number), "%s", number);
    snprintf(k->pwd, sizeof(k->pwd), "%s", pwd);
    /* 交易密码默认取学号后 6 位 */
    {
        size_t ln = strlen(number);
        const char *src = ln >= 6 ? number + ln - 6 : DEFAULT_PWD;
        snprintf(k->pay_pwd, sizeof(k->pay_pwd), "%s", src);
    }
    k->state = ST_NORMAL;
    k->online = ONLINE_NO;
    k->limit_once = 20.0;               /* 默认单笔限额 20 元 */
    k->limit_day  = 50.0;               /* 默认日累计限额 50 元 */
    snprintf(k->expire, sizeof(k->expire), "%s", "2029-12-31");
    now_fmt(k->used_date, sizeof(k->used_date), "%Y-%m-%d");
    campus_save();
    return 0;
}

/* ---------------- 资金动作 ---------------- */
/* 充值：增加余额与累计充值数额，并记流水 */
int campus_recharge(Card *card, double amount, char *err, int esz) {
    if (!card)          return fail(err, esz, "卡片不存在");
    if (amount <= 0)    return fail(err, esz, "充值金额必须大于 0");
    if (amount > 10000) return fail(err, esz, "单次充值不得超过 10000 元");
    card->remain   += amount;
    card->recharge += amount;
    add_tx(card, TX_RECHARGE, "充值", amount, "ADMIN", "管理员充值");
    add_income(amount);
    campus_save();
    return 0;
}

/* 消费扣款：集中校验状态、交易密码、单笔限额、日累计限额。
 * merchant 商户名；device 终端号；pay_pwd 交易密码 */
int campus_pay(Card *card, const char *merchant, double amount,
               const char *pay_pwd, const char *device, char *err, int esz) {
    if (!card)                return fail(err, esz, "卡片不存在");
    if (amount <= 0)          return fail(err, esz, "消费金额必须大于 0");
    if (!merchant || !*merchant) return fail(err, esz, "商户不能为空");

    /* 1. 卡片状态 */
    if (check_expire(card))   return fail(err, esz, "卡片已过有效期，请到卡务中心延期");
    if (card->state == ST_LOST)   return fail(err, esz, "该卡已挂失，无法消费");
    if (card->state == ST_FROZEN) return fail(err, esz, "该卡已冻结，无法消费");

    /* 2. 交易密码 */
    if (!campus_check_pay_pwd(card->id, pay_pwd ? pay_pwd : ""))
        return fail(err, esz, "交易密码错误");

    /* 3. 限额（跨天自动清零） */
    roll_day(card);
    if (card->limit_once > 0 && amount > card->limit_once)
        return fail(err, esz, "超过单笔消费限额");
    if (card->limit_day > 0 && card->used_day + amount > card->limit_day)
        return fail(err, esz, "超过当日累计消费限额");

    /* 4. 余额 */
    if (card->remain < amount)
        return fail(err, esz, "余额不足，请先充值");

    /* 5. 扣款、累计、记账 */
    card->remain   = round2(card->remain - amount);
    card->used_day = round2(card->used_day + amount);
    add_tx(card, TX_CONSUME, merchant, amount, device, "消费扣款");
    campus_save();
    return 0;
}

/* 退款 / 补助发放：增加余额并记流水（类型由调用方指定） */
int campus_grant(Card *card, int type, double amount, const char *memo,
                 char *err, int esz) {
    if (!card)       return fail(err, esz, "卡片不存在");
    if (amount <= 0) return fail(err, esz, "金额必须大于 0");
    if (type != TX_REFUND && type != TX_GRANT)
        return fail(err, esz, "类型只能是退款或补助");
    card->remain = round2(card->remain + amount);
    add_tx(card, type, type == TX_REFUND ? "退款" : "补助", amount,
           "ADMIN", memo ? memo : "");
    if (type == TX_GRANT) add_income(amount);
    campus_save();
    return 0;
}

/* ---------------- 机房上机（保留原题目要求） ---------------- */
int campus_online(Card *card, const char *address, char *err, int esz) {
    if (!card)                      return fail(err, esz, "卡片不存在");
    if (check_expire(card))         return fail(err, esz, "卡片已过有效期，不能上机");
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
    fp = fopen(path_of(name), "a");
    if (!fp) return;
    fprintf(fp, "%s|%s|%s|%d|%.2f|%s|%s\n", r->id, r->date, r->start_time,
            r->duration, r->cost, r->state, r->address);
    fclose(fp);
}

/* 下机结账：按用时计费、扣余额、写记录、记交易流水与收入 */
int campus_offline(Card *card, double *out_cost, char *err, int esz) {
    Record r;
    char end_time[20];
    time_t t1, t2;
    double minutes, cost;

    if (!card)                      return fail(err, esz, "卡片不存在");
    if (card->online != ONLINE_YES) return fail(err, esz, "该卡当前未上机");

    /* 1. 用时与费用 */
    now_fmt(end_time, sizeof(end_time), "%Y-%m-%d %H:%M:%S");
    t1 = parse_time(card->start_time);
    t2 = parse_time(end_time);
    minutes = (t1 == (time_t)-1) ? 0.0 : difftime(t2, t1) / 60.0;
    if (minutes < 0) minutes = 0;
    cost = round2(minutes / 60.0 * RATE_PER_HOUR);

    /* 2. 扣费（余额不足则清零，视为欠费） */
    if (card->remain >= cost) card->remain = round2(card->remain - cost);
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

    /* 4. 记交易流水与收入，并清除上机状态 */
    if (cost > 0) {
        add_tx(card, TX_CONSUME, "机房", cost, "PC-01", "上机结账");
        add_income(cost);
    }
    card->online = ONLINE_NO;
    card->start_time[0] = '\0';
    card->address[0] = '\0';
    campus_save();

    if (out_cost) *out_cost = cost;
    return 0;
}

/* ---------------- 卡务管理 ---------------- */
int campus_set_lost(Card *card, char *err, int esz) {
    double cost = 0.0;
    if (!card)                    return fail(err, esz, "卡片不存在");
    if (card->state == ST_LOST)   return fail(err, esz, "该卡已处于挂失状态");
    if (card->state == ST_FROZEN) return fail(err, esz, "该卡已冻结，不能挂失");
    if (card->online == ONLINE_YES) campus_offline(card, &cost, NULL, 0);
    card->state = ST_LOST;
    campus_save();
    return 0;
}

int campus_unlost(Card *card, char *err, int esz) {
    if (!card)                  return fail(err, esz, "卡片不存在");
    if (card->state != ST_LOST) return fail(err, esz, "该卡当前不是挂失状态");
    card->state = ST_NORMAL;
    campus_save();
    return 0;
}

int campus_freeze(Card *card, int on, char *err, int esz) {
    if (!card) return fail(err, esz, "卡片不存在");
    if (on) {
        double cost = 0.0;
        if (card->state == ST_FROZEN) return fail(err, esz, "该卡已冻结");
        if (card->online == ONLINE_YES) campus_offline(card, &cost, NULL, 0);
        card->state = ST_FROZEN;
    } else {
        if (card->state != ST_FROZEN && card->state != ST_EXPIRED)
            return fail(err, esz, "该卡当前不是冻结状态");
        card->state = ST_NORMAL;
    }
    campus_save();
    return 0;
}

int campus_reset_pwd(Card *card, char *err, int esz) {
    (void)err; (void)esz;
    if (!card) return -1;
    snprintf(card->pwd, sizeof(card->pwd), "%s", DEFAULT_PWD);
    if (card->state == ST_FROZEN) card->state = ST_NORMAL;
    campus_save();
    return 0;
}

int campus_reset_pay_pwd(Card *card, char *err, int esz) {
    if (!card) return fail(err, esz, "卡片不存在");
    snprintf(card->pay_pwd, sizeof(card->pay_pwd), "%s", DEFAULT_PWD);
    campus_save();
    return 0;
}

/* 补办新卡：收工本费、解除挂失、保留余额 */
int campus_reissue(Card *card, double fee, char *err, int esz) {
    if (!card)             return fail(err, esz, "卡片不存在");
    if (fee < 0)           return fail(err, esz, "工本费不能为负");
    if (card->remain < fee) return fail(err, esz, "余额不足以支付工本费");

    if (fee > 0) {
        card->remain = round2(card->remain - fee);
        add_tx(card, TX_FEE, "卡务中心", fee, "ADMIN", "补办工本费");
        add_income(fee);
    }
    card->state = ST_NORMAL;                /* 补办后解除挂失 */
    snprintf(card->pay_pwd, sizeof(card->pay_pwd), "%s", DEFAULT_PWD);
    /* 实物卡换了，上机状态一并清除 */
    card->online = ONLINE_NO;
    card->start_time[0] = '\0';
    card->address[0] = '\0';
    campus_save();
    return 0;
}

int campus_set_expire(Card *card, const char *date, char *err, int esz) {
    if (!card) return fail(err, esz, "卡片不存在");
    if (!date || strlen(date) != 10 || date[4] != '-' || date[7] != '-')
        return fail(err, esz, "日期格式应为 YYYY-MM-DD");
    snprintf(card->expire, sizeof(card->expire), "%s", date);
    /* 延期后若仍在有效期内，把过期状态改回正常 */
    if (card->state == ST_EXPIRED) {
        char today[12];
        now_fmt(today, sizeof(today), "%Y-%m-%d");
        if (strcmp(today, date) <= 0) card->state = ST_NORMAL;
    }
    campus_save();
    return 0;
}

int campus_set_limits(Card *card, double limit_once, double limit_day,
                      char *err, int esz) {
    if (!card)          return fail(err, esz, "卡片不存在");
    if (limit_once < 0 || limit_day < 0)
        return fail(err, esz, "限额不能为负");
    if (limit_day > 0 && limit_once > limit_day)
        return fail(err, esz, "单笔限额不能大于日累计限额");
    card->limit_once = round2(limit_once);
    card->limit_day  = round2(limit_day);
    campus_save();
    return 0;
}

int campus_change_pwd(Card *card, const char *old_pwd, const char *new_pwd,
                      char *err, int esz) {
    if (!card)                          return fail(err, esz, "卡片不存在");
    if (strcmp(card->pwd, old_pwd))     return fail(err, esz, "原登录密码错误");
    if (strlen(new_pwd) < 6)            return fail(err, esz, "新密码不得少于 6 位");
    snprintf(card->pwd, sizeof(card->pwd), "%s", new_pwd);
    campus_save();
    return 0;
}

int campus_change_pay_pwd(Card *card, const char *old_pwd, const char *new_pwd,
                          char *err, int esz) {
    if (!card)                          return fail(err, esz, "卡片不存在");
    if (strcmp(card->pay_pwd, old_pwd)) return fail(err, esz, "原交易密码错误");
    if (strlen(new_pwd) < 6)            return fail(err, esz, "新交易密码不得少于 6 位");
    snprintf(card->pay_pwd, sizeof(card->pay_pwd), "%s", new_pwd);
    campus_save();
    return 0;
}

/* ---------------- 门禁 / 考勤 ---------------- */
int campus_access(Card *card, int dir, int kind, const char *place,
                  char *err, int esz) {
    if (!card)                    return fail(err, esz, "卡片不存在");
    if (check_expire(card))       return fail(err, esz, "卡片已过有效期");
    if (card->state == ST_LOST)   return fail(err, esz, "该卡已挂失");
    if (card->state == ST_FROZEN) return fail(err, esz, "该卡已冻结");
    if (!place || !*place)        return fail(err, esz, "请提供门禁地点");
    add_access(card, dir, kind, place);
    return 0;
}

/* ---------------- 报表 ---------------- */
/* 当天收入：上机费合计与上机人次（保留原题目要求的口径） */
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
    if (out_fee)   *out_fee = round2(fee);
    if (out_times) *out_times = times;
}

/* 当天全站汇总：消费总额、消费笔数、充值总额，以及各商户消费额。
 * 顺序扫描流水边读边累加——不把上万条流水读进数组，省掉 1MB 级缓冲 */
void campus_today_report(double *out_consume, int *out_times,
                         double *out_recharge, double merchant_sum[]) {
    char line[256], timebuf[20], mname[24], buf[24];
    char today[12];
    FILE *fp;
    double consume = 0.0, recharge = 0.0;
    int times = 0;

    now_fmt(today, sizeof(today), "%Y-%m-%d");
    if (merchant_sum) {
        for (int i = 0; i < MERCHANT_MAX; i++) merchant_sum[i] = 0.0;
    }

    fp = fopen(path_of("ledger.dat"), "r");
    if (!fp) {
        if (out_consume)  *out_consume = 0.0;
        if (out_times)    *out_times = 0;
        if (out_recharge) *out_recharge = 0.0;
        return;
    }
    while (fgets(line, sizeof(line), fp)) {
        int type;
        double amount;
        field_at(line, 0, buf, sizeof(buf));
        if (!buf[0]) continue;
        field_at(line, 1, timebuf, sizeof(timebuf));
        /* 只按日期前缀比较，避免逐条做日期解析 */
        if (strncmp(timebuf, today, 10)) continue;
        type   = field_int(line, 3, TX_CONSUME);
        amount = field_num(line, 5, 0);

        if (type == TX_CONSUME) {
            consume += amount;
            times++;
            if (merchant_sum) {
                field_at(line, 4, mname, sizeof(mname));
                for (int m = 0; m < g_n_merchant; m++) {
                    if (!strcmp(mname, g_merchants[m])) {
                        merchant_sum[m] += amount;
                        break;
                    }
                }
            }
        } else if (type == TX_RECHARGE || type == TX_GRANT) {
            recharge += amount;
        }
    }
    fclose(fp);
    if (out_consume)  *out_consume = round2(consume);
    if (out_times)    *out_times = times;
    if (out_recharge) *out_recharge = round2(recharge);
}
