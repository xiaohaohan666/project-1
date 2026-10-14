/* ============================================================
 *  校园一卡通 —— 业务核心层
 *  （命令行版 main.c 与 Web 服务 src/server.c 共用同一套规则）
 * ------------------------------------------------------------
 *  数据文件（均位于数据目录）：
 *    cards.dat     卡片
 *                  卡号|姓名|学号|登录密码|交易密码|累计充值|余额|状态
 *                  |是否上机|开始时间|地点|日限额|今日已用|今日日期|单笔限额|有效期
 *    ledger.dat    交易总账（追加写，永不覆盖）
 *                  流水号|日期时间|卡号|类型|商户|金额|交易后余额|终端|备注
 *    access.dat    门禁 / 考勤记录（追加写）
 *                  卡号|日期时间|方向|地点
 *    <卡号>.txt    该卡机房上机记录（保留原题目要求）
 *                  卡号|上机日期|开始时间|用时(分)|费用|状态|地点
 *    income.dat    收入流水（兼容旧数据）
 *
 *  设计要点：
 *    · 卡片全量载入内存；交易总账采用追加写，避免全量覆写造成历史丢失
 *    · 消费统一走 campus_pay()，集中处理限额、交易密码与扣款，便于审计
 *    · 机房上机计费作为一个特殊消费点保留，与一卡通共用同一余额
 * ============================================================ */
#ifndef CAMPUS_H
#define CAMPUS_H

#include <time.h>

/* ---------------- 容量与费率 ---------------- */
#define CARD_MAX      200           /* 最多发卡数量                     */
#define REC_MAX       5000          /* 单张卡最多读取的记录条数         */
#define LEDGER_MAX    20000         /* 内存中最多保留的交易流水条数     */
#define ACCESS_MAX    20000         /* 内存中最多保留的门禁记录条数     */
#define INCOME_MAX    5000
#define RATE_PER_HOUR 2.0           /* 机房上机费率：元 / 小时          */
#define ADMIN_PWD     "admin123"    /* 管理员密码                       */
#define DEFAULT_PWD   "123456"      /* 重置后的学生登录密码             */
#define MERCHANT_MAX  16            /* 支持的商户 / 消费点数量          */

/* 卡片状态：正常 / 挂失 / 冻结 / 已过期 */
enum { ST_NORMAL, ST_LOST, ST_FROZEN, ST_EXPIRED };
/* 上机状态：未上机 / 上机中 */
enum { ONLINE_NO, ONLINE_YES };
/* 交易类型：充值 / 消费 / 退款 / 工本费 / 补助 */
enum { TX_RECHARGE, TX_CONSUME, TX_REFUND, TX_FEE, TX_GRANT };
/* 门禁方向：进 / 出 */
enum { ACC_IN, ACC_OUT };
/* 门禁用途：门禁 / 考勤 */
enum { ACCESS_DOOR, ACCESS_ATTEND };

/* ---------------- 数据结构 ---------------- */
/* 校园卡（一卡通）：卡号、姓名、学号、充值数额、余额、状态，并含限额与交易密码 */
typedef struct {
    char   id[10];              /* 卡号（同时是上机记录文件名前缀）   */
    char   name[12];            /* 姓名                               */
    char   number[12];          /* 学号                               */
    char   pwd[20];             /* 登录密码（同学自己设定）            */
    char   pay_pwd[20];         /* 交易密码（消费时校验）              */
    double recharge;            /* 累计充值数额（元）                 */
    double remain;              /* 卡内余额（元）                     */
    int    state;               /* 卡片状态，见 ST_*                  */
    int    online;              /* 是否上机中，见 ONLINE_*            */
    char   start_time[20];      /* 本次上机开始时间（上机中有效）      */
    char   address[24];         /* 本次上机地点（上机中有效）          */
    double limit_day;           /* 日累计消费限额（0 表示不限）        */
    double used_day;            /* 今日已消费金额（元）                */
    char   used_date[12];       /* used_day 对应的日期，跨天自动清零   */
    double limit_once;          /* 单笔消费限额（0 表示不限）          */
    char   expire[12];          /* 卡片有效期 YYYY-MM-DD，空表示长期   */
} Card;

/* 机房上机记录（保留原题目要求：每生一个文本文件，永久保存） */
typedef struct {
    char   id[10];              /* 卡号                               */
    char   date[12];            /* 上机日期 YYYY-MM-DD                */
    char   start_time[10];      /* 开始时间 HH:MM:SS                  */
    int    duration;            /* 上机用时（分钟）                   */
    double cost;                /* 本次费用（元）                     */
    char   state[12];           /* 结束状态："已下机"                 */
    char   address[24];         /* 上机地点                           */
} Record;

/* 交易流水（一卡通总账的一条记录） */
typedef struct {
    long   seq;                 /* 流水号（自增）                     */
    char   time[20];            /* 交易时间 YYYY-MM-DD HH:MM:SS       */
    char   id[10];              /* 卡号                               */
    int    type;                /* 交易类型，见 TX_*                  */
    char   merchant[24];        /* 商户 / 消费点名称                  */
    double amount;              /* 金额（正数；方向由 type 决定）      */
    double balance;             /* 交易后余额                         */
    char   device[16];          /* 终端号（如 POS-01）                */
    char   memo[32];            /* 备注                               */
} Transaction;

/* 门禁 / 考勤记录 */
typedef struct {
    char   id[10];              /* 卡号                               */
    char   time[20];            /* 刷卡时间 YYYY-MM-DD HH:MM:SS       */
    int    dir;                 /* 方向，见 ACC_*                     */
    char   place[24];           /* 地点（如 3 号宿舍楼）               */
    int    kind;                /* 用途，见 ACCESS_*                  */
} AccessLog;

/* ---------------- 生命周期 ---------------- */
/* 初始化并载入数据；无数据时建立演示卡片。成功返回 1 */
int campus_init(const char *data_dir);
/* 保存全部卡片。成功返回 1 */
int campus_save(void);
/* 当前卡片总数 */
int campus_count(void);
/* 载入时是否因超过 CARD_MAX 而丢弃了卡片。返回丢弃的风险提示：
 * 1 表示可能被截断（已载满上限），0 表示正常 */
int campus_card_truncated(void);
/* 发卡上限 */
int campus_card_max(void);
/* 取第 i 张卡片，越界返回 NULL */
Card *campus_at(int i);

/* ---------------- 查询 ---------------- */
Card *campus_find(const char *id);
/* 卡片状态文字：正常 / 挂失 / 冻结 / 已过期 */
const char *campus_state_text(int state);
/* 交易类型文字：充值 / 消费 / 退款 / 工本费 / 补助 */
const char *campus_type_text(int type);
/* 校验登录密码；正确返回 1 */
int  campus_check_pwd(const char *id, const char *pwd);
/* 校验交易密码；正确返回 1 */
int  campus_check_pay_pwd(const char *id, const char *pwd);
/* 读取某卡机房上机记录，返回条数 */
int  campus_load_records(const char *id, Record *out, int max);
/* 统计某卡 [from, 现在] 的上机情况，返回条数并回传合计 */
int  campus_stats(const char *id, time_t from,
                  int *out_count, int *out_minutes, double *out_cost);
/* 读取交易流水。id 为空表示全部卡；type 为 -1 表示全部类型；
 * from 为 0 表示不限时间。返回条数。
 * 采用「环形保留」：超过 max 条时保留最近 max 条；
 * out_truncated（可传 NULL）回传是否发生了截断 */
int  campus_load_ledger(const char *id, int type, time_t from,
                        Transaction *out, int max, int *out_truncated);
/* 只统计符合条件的流水条数，不取内容。参数含义同上 */
int  campus_count_ledger(const char *id, int type, time_t from);
/* 分页读取交易流水：skip 为跳过的条数（按时间正序）。返回条数 */
int  campus_load_ledger_page(const char *id, int type, time_t from,
                             int skip, Transaction *out, int max);
/* 读取门禁记录。id 为空表示全部卡；保留最近 max 条。
 * out_truncated 回传是否截断 */
int  campus_load_access(const char *id, AccessLog *out, int max,
                        int *out_truncated);
/* 只统计门禁记录条数 */
int  campus_count_access(const char *id);
/* 汇总某卡在 [from, 现在] 的消费。返回笔数，回传合计金额 */
int  campus_sum_consume(const char *id, time_t from, double *out_total);
/* 商户/消费点名称表。返回商户数量，out 指向名称数组 */
int  campus_merchants(const char **out, int max);

/* ---------------- 业务动作（失败返回 -1 并写入 err） ---------------- */
/* 管理员登录校验 */
int  campus_login(const char *id, const char *pwd, int as_admin);

/* 新卡注册：自行设定登录密码，交易密码默认取学号后 6 位 */
int  campus_register(const char *id, const char *name, const char *number,
                     const char *pwd, char *err, int esz);
/* 充值：增加余额与累计充值数额，并记一条交易流水 */
int  campus_recharge(Card *card, double amount, char *err, int esz);
/* 消费扣款：集中校验状态、交易密码、单笔与日累计限额。
 * merchant 为商户名；device 为终端号（可传 NULL） */
int  campus_pay(Card *card, const char *merchant, double amount,
                const char *pay_pwd, const char *device, char *err, int esz);
/* 退款 / 补助发放（金额为正，计入余额，类型为 TX_REFUND / TX_GRANT） */
int  campus_grant(Card *card, int type, double amount, const char *memo,
                  char *err, int esz);

/* 机房上机 */
int  campus_online(Card *card, const char *address, char *err, int esz);
int  campus_offline(Card *card, double *out_cost, char *err, int esz);

/* 卡务管理 */
int  campus_set_lost(Card *card, char *err, int esz);        /* 挂失 */
int  campus_unlost(Card *card, char *err, int esz);          /* 解除挂失 */
int  campus_freeze(Card *card, int on, char *err, int esz);  /* 冻结 / 解冻 */
int  campus_reset_pwd(Card *card, char *err, int esz);       /* 重置登录密码 */
int  campus_reset_pay_pwd(Card *card, char *err, int esz);   /* 重置交易密码 */
/* 补办新卡：卡号不变，收工本费，余额保留，挂失状态解除 */
int  campus_reissue(Card *card, double fee, char *err, int esz);
/* 卡片延期：设置有效期 YYYY-MM-DD */
int  campus_set_expire(Card *card, const char *date, char *err, int esz);
/* 设置限额：limit_once 单笔限额，limit_day 日累计限额（0 表示不限） */
int  campus_set_limits(Card *card, double limit_once, double limit_day,
                       char *err, int esz);
/* 学生自行修改登录密码 / 交易密码 */
int  campus_change_pwd(Card *card, const char *old_pwd, const char *new_pwd,
                       char *err, int esz);
int  campus_change_pay_pwd(Card *card, const char *old_pwd, const char *new_pwd,
                           char *err, int esz);

/* 门禁 / 考勤刷卡：dir 为方向，kind 为用途，place 为地点 */
int  campus_access(Card *card, int dir, int kind, const char *place,
                   char *err, int esz);

/* ---------------- 报表 ---------------- */
/* 当天收入：上机费合计与上机人次 */
void campus_today_income(double *out_fee, int *out_times);
/* 当天全站汇总：消费总额、消费笔数、充值总额、各商户消费额（按序号） */
void campus_today_report(double *out_consume, int *out_times,
                         double *out_recharge, double merchant_sum[]);

#endif
