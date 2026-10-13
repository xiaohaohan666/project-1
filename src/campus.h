/* ============================================================
 *  模拟校园卡 —— 业务核心层
 *  （命令行版 main.c 与 Web 服务 src/server.c 共用同一套规则）
 * ------------------------------------------------------------
 *  数据文件（均位于数据目录）：
 *    cards.dat    卡片：卡号|姓名|学号|密码|累计充值|余额|状态|是否上机|开始时间|地点
 *    <卡号>.txt   该卡上机记录：卡号|上机日期|开始时间|用时(分)|费用|状态|地点
 *    income.dat   收入流水：日期|金额(元)
 * ============================================================ */
#ifndef CAMPUS_H
#define CAMPUS_H

#include <time.h>

/* ---------------- 容量与费率 ---------------- */
#define CARD_MAX      200
#define REC_MAX       5000
#define INCOME_MAX    5000
#define RATE_PER_HOUR 2.0
#define ADMIN_PWD     "admin123"
#define DEFAULT_PWD   "123456"

/* 卡片状态：正常 / 挂失 / 冻结 */
enum { ST_NORMAL, ST_LOST, ST_FROZEN };
/* 上机状态：未上机 / 上机中 */
enum { ONLINE_NO, ONLINE_YES };

/* ---------------- 数据结构 ---------------- */
/* 校园卡：卡号、姓名、学号、充值数额、余额、状态（另含密码与上机状态） */
typedef struct {
    char   id[10];              /* 卡号                         */
    char   name[12];            /* 姓名                         */
    char   number[12];          /* 学号                         */
    char   pwd[20];             /* 登录密码                     */
    double recharge;            /* 累计充值数额（元）           */
    double remain;              /* 卡内余额（元）               */
    int    state;               /* 卡片状态 ST_*                */
    int    online;              /* 是否上机中 ONLINE_*          */
    char   start_time[20];      /* 本次上机开始时间             */
    char   address[24];         /* 本次上机地点                 */
} Card;

/* 上机记录：卡号、上机日期、开始时间、上机用时、状态、上机地点 */
typedef struct {
    char   id[10];
    char   date[12];
    char   start_time[10];
    int    duration;            /* 分钟                         */
    double cost;                /* 元                           */
    char   state[12];
    char   address[24];
} Record;

/* ---------------- 生命周期 ---------------- */
/* 初始化并载入数据；无数据时建立演示卡片。成功返回 1 */
int campus_init(const char *data_dir);
/* 保存全部卡片。成功返回 1 */
int campus_save(void);
/* 当前卡片总数 */
int campus_count(void);
/* 取第 i 张卡片（0 <= i < campus_count()），越界返回 NULL */
Card *campus_at(int i);

/* ---------------- 查询 ---------------- */
/* 按卡号查找卡片；找不到返回 NULL */
Card *campus_find(const char *id);
/* 状态文字：正常 / 挂失 / 冻结 */
const char *campus_state_text(int state);
/* 校验学生密码；正确返回 1 */
int  campus_check_pwd(const char *id, const char *pwd);
/* 读取某卡上机记录，返回条数 */
int  campus_load_records(const char *id, Record *out, int max);
/* 统计某卡 [from, 现在] 的上机情况；from 为 0 表示全部。
 * 返回记录条数，并通过输出参数回传合计（输出参数可传 NULL） */
int  campus_stats(const char *id, time_t from,
                  int *out_count, int *out_minutes, double *out_cost);

/* ---------------- 业务动作（失败返回 -1 并写入 err） ---------------- */
/* 学生登录校验：管理员密码或学生密码。成功返回 1 */
int campus_login(const char *id, const char *pwd, int as_admin);
int campus_recharge(Card *card, double amount, char *err, int esz);
int campus_online(Card *card, const char *address, char *err, int esz);
int campus_offline(Card *card, double *out_cost, char *err, int esz);
int campus_set_lost(Card *card, char *err, int esz);
int campus_reset_pwd(Card *card, char *err, int esz);
int campus_change_pwd(Card *card, const char *old_pwd, const char *new_pwd,
                      char *err, int esz);
/* 当天收入：上机费合计与上机人次（输出参数可传 NULL） */
void campus_today_income(double *out_fee, int *out_times);

#endif
