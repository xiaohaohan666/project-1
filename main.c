/* ============================================================
 *      校园一卡通 —— 上机计费 / 校园消费 / 门禁考勤 综合系统
 * ------------------------------------------------------------
 *  功能总览：
 *    ① 新卡注册      card_register()   自设登录密码，交易密码默认取学号后 6 位
 *    ② 登录模块      student_login()   同学按自己设定的密码登录，
 *                                      连续三次错误则冻结账户
 *    ③ 学生端        查余额 / 交易流水 / 个人信息 / 消费统计
 *                    机房上机登记与下机结账，修改登录密码与交易密码
 *    ④ 商户终端      食堂、超市、文印店、热水洗衣、宿舍水电、图书馆罚款
 *                    与机房上机并列为一卡通消费点
 *    ⑤ 卡务中心      充值、挂失、解挂、冻结、补办、重置密码、限额、延期
 *    ⑥ 报表          当天收入、全站汇总、按商户统计、门禁考勤记录
 *    ⑦ 记录保存      机房上机记录按卡存 <卡号>.txt；交易总账 ledger.dat；
 *                    门禁记录 access.dat —— 全部追加写，永久保存
 *
 *  编译：
 *    gcc -std=c11 -Wall -Wextra -O2 -fexec-charset=UTF-8 -Isrc main.c src/campus.c -o main.exe
 *  运行：main.exe（数据存放在程序目录下的 data/ 子目录）
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>            /* SetConsoleOutputCP：让控制台按 UTF-8 显示中文 */
#  include <conio.h>              /* _getch：读取密码时不回显                       */
#  include <io.h>                 /* _isatty                                        */
#  define isatty _isatty          /* 统一为 POSIX 名称，便于跨平台                  */
#  define CLEAR_CMD "cls"         /* Windows 清屏命令                               */
#else
#  include <termios.h>            /* 关闭终端回显，实现密码输入                      */
#  include <unistd.h>             /* isatty                                         */
#  define CLEAR_CMD "clear"       /* Linux 清屏命令                                 */
#endif

#include "campus.h"

/* 一次登录允许的最大密码尝试次数，超过则冻结账户 */
#define MAX_TRY 3
/* 上机地点数量 */
#define PLACE_N 4
/* 终端号长度 */
#define DEV_LEN 16

/* ============================================================
 *                      基础交互工具
 * ============================================================ */

/* 清屏。先刷新标准输出，避免内容滞留在缓冲区 */
static void clear_screen(void) {
    fflush(stdout);
    system(CLEAR_CMD);
}

/* 暂停等待用户按回车。输入被重定向（自动化测试）时不暂停 */
static void pause_screen(void) {
    int ch;
    if (!isatty(0)) return;
    printf("\n按回车键继续...");
    fflush(stdout);
    while ((ch = getchar()) != '\n' && ch != EOF) ;
}

/* 读取一行输入并去掉行尾换行符。输入结束返回 0 */
static int read_line(const char *prompt, char *buf, int size) {
    printf("%s", prompt);
    if (!fgets(buf, size, stdin)) { buf[0] = '\0'; return 0; }
    buf[strcspn(buf, "\r\n")] = '\0';
    return 1;
}

/* 必须读到一行文本：读不到（输入结束）直接退出，避免死循环 */
static void ask_text(const char *prompt, char *buf, int size) {
    if (!read_line(prompt, buf, size)) exit(0);
}

/* 读取整数。成功返回 1，格式错误返回 0 */
static int ask_int(const char *prompt, int *out) {
    char buf[32];
    ask_text(prompt, buf, sizeof(buf));
    return sscanf(buf, "%d", out) == 1;
}

/* 读取金额。成功返回 1，格式错误返回 0 */
static int ask_money(const char *prompt, double *out) {
    char buf[32];
    ask_text(prompt, buf, sizeof(buf));
    return sscanf(buf, "%lf", out) == 1;
}

/* 读取密码：控制台逐字符读且以 * 回显；输入被重定向时按行读 */
static void ask_password(char *buf, int size) {
    if (!isatty(0)) {
        if (!fgets(buf, size, stdin)) buf[0] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';
        return;
    }
#ifdef _WIN32
    {
        int i = 0, ch;
        while ((ch = _getch()) != '\r' && ch != EOF) {
            if (ch == 8) {
                if (i > 0) { i--; printf("\b \b"); }
            } else if (i < size - 1) {
                buf[i++] = (char)ch;
                putchar('*');
            }
        }
        buf[i] = '\0';
    }
#else
    {
        struct termios old_term, new_term;
        int i = 0, ch;
        tcgetattr(0, &old_term);
        new_term = old_term;
        new_term.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(0, TCSANOW, &new_term);
        while ((ch = getchar()) != '\n' && ch != EOF) {
            if ((ch == 8 || ch == 127) && i > 0) { i--; printf("\b \b"); }
            else if (ch != 8 && ch != 127 && i < size - 1) { buf[i++] = (char)ch; putchar('*'); }
        }
        buf[i] = '\0';
        tcsetattr(0, TCSANOW, &old_term);
    }
#endif
    printf("\n");
}

/* 菜单项：key 功能编号，text 功能说明 */
typedef struct {
    int         key;
    const char *text;
} MenuItem;

/* 通用功能菜单。返回用户选中的功能编号；选 0 返回 0（表示返回/退出） */
static int menu(const char *head, const char *extra, const MenuItem *items, int n) {
    while (1) {
        int choice;
        clear_screen();
        printf("================ %s ================\n", head);
        if (extra && *extra) printf("%s", extra);
        for (int i = 0; i < n; i++)
            printf("    %2d. %s\n", items[i].key, items[i].text);
        printf("     0. 返回\n");
        printf("==========================================\n");

        if (!ask_int("请选择功能：", &choice)) {
            printf("输入无效，请输入功能编号！\n");
            pause_screen();
            continue;
        }
        if (choice == 0) return 0;
        for (int i = 0; i < n; i++)
            if (items[i].key == choice) return choice;
        printf("无效的选择！\n");
        pause_screen();
    }
}

/* 按卡号选择卡片。找不到时提示原因并返回 NULL */
static Card *pick_card(const char *prompt) {
    char id[10];
    Card *card;
    ask_text(prompt, id, sizeof(id));
    card = campus_find(id);
    if (!card) {
        printf("找不到该卡号，请核对后重试！\n");
        pause_screen();
    }
    return card;
}

/* 让学生 / 持卡人确认消费：校验交易密码。成功返回 1 */
static int confirm_pay(Card *card, double amount, const char *merchant) {
    char pwd[20];
    printf("    消费商户：%s\n    消费金额：%.2f 元\n", merchant, amount);
    printf("    当前余额：%.2f 元\n", card->remain);
    printf("请输入交易密码：");
    ask_password(pwd, sizeof(pwd));
    if (!campus_check_pay_pwd(card->id, pwd)) {
        printf("交易密码错误，交易取消！\n");
        pause_screen();
        return 0;
    }
    return 1;
}

/* ============================================================
 *                        显示辅助
 * ============================================================ */

/* 显示卡片基本信息（余额、状态、限额、上机情况） */
static void show_card(const Card *card) {
    printf("---------------- 卡内信息 ----------------\n");
    printf("    卡号：%-6s 姓名：%-6s 学号：%s\n", card->id, card->name, card->number);
    printf("    累计充值：%.2f 元    余额：%.2f 元\n", card->recharge, card->remain);
    printf("    卡片状态：%s    有效期：%s\n", campus_state_text(card->state),
           card->expire[0] ? card->expire : "长期");
    printf("    单笔限额：%s    日累计限额：%s\n",
           card->limit_once > 0 ? "有" : "不限",
           card->limit_day > 0 ? "有" : "不限");
    if (card->limit_once > 0 || card->limit_day > 0)
        printf("    今日已消费：%.2f 元（单笔上限 %.2f，日上限 %.2f）\n",
               card->used_day, card->limit_once, card->limit_day);
    if (card->online == ONLINE_YES)
        printf("    正在上机：地点 %s，开始时间 %s\n", card->address, card->start_time);
    else
        printf("    上机状态：未上机\n");
    printf("------------------------------------------\n");
}

/* 显示交易流水表。id 为空显示全部；type 为 -1 显示全部类型；limit 为条数上限 */
static void show_ledger(const char *id, int type, int limit) {
    static Transaction tx[LEDGER_MAX];
    int n = campus_load_ledger(id, type, 0, tx, LEDGER_MAX);
    int start = n > limit ? n - limit : 0;

    if (n == 0) {
        printf("    暂无交易流水。\n");
        return;
    }
    printf("    流水号  交易时间             卡号   类型    商户         金额      余额\n");
    printf("    ----------------------------------------------------------------------\n");
    for (int i = start; i < n; i++) {
        Transaction *t = &tx[i];
        printf("    %-7ld %-19s %-6s %-6s %-12s %8.2f %9.2f\n",
               t->seq, t->time, t->id, campus_type_text(t->type),
               t->merchant, t->amount, t->balance);
    }
    printf("    共 %d 条（显示最近 %d 条）\n", n, n - start);
}

/* ============================================================
 *                        学生端功能
 * ============================================================ */

/* 查询卡中余额、状态与上机信息 */
static void student_info(Card *card) {
    clear_screen();
    show_card(card);
    if (card->online == ONLINE_YES) {
        time_t now = time(NULL);
        struct tm tm_start;
        double minutes;
        memset(&tm_start, 0, sizeof(tm_start));
        sscanf(card->start_time, "%d-%d-%d %d:%d:%d",
               &tm_start.tm_year, &tm_start.tm_mon, &tm_start.tm_mday,
               &tm_start.tm_hour, &tm_start.tm_min, &tm_start.tm_sec);
        tm_start.tm_year -= 1900;
        tm_start.tm_mon  -= 1;
        tm_start.tm_isdst = -1;
        {
            time_t t1 = mktime(&tm_start);
            minutes = (t1 == (time_t)-1) ? 0.0 : difftime(now, t1) / 60.0;
        }
        if (minutes < 0) minutes = 0;
        printf("    已上机约 %.0f 分钟，预计费用 %.2f 元（费率 %.2f 元/小时）\n",
               minutes, minutes / 60.0 * RATE_PER_HOUR, RATE_PER_HOUR);
    }
    pause_screen();
}

/* 查询交易流水（支持时间段选择） */
static void student_ledger(Card *card) {
    static const MenuItem periods[4] = {
        { 1, "今天" }, { 2, "本周" }, { 3, "本月" }, { 4, "全部" }
    };
    int choice = menu("交易流水时间段", "  请选择要查询的时间范围：\n", periods, 4);
    time_t from = 0;
    struct tm tm_now;

    if (choice == 0) return;
    if (choice != 4) {
        time_t now = time(NULL);
        tm_now = *localtime(&now);
        if (choice == 2) {
            int wday = tm_now.tm_wday;
            tm_now.tm_mday -= (wday == 0) ? 6 : (wday - 1);
        } else if (choice == 3) {
            tm_now.tm_mday = 1;
        }
        tm_now.tm_hour = 0; tm_now.tm_min = 0; tm_now.tm_sec = 0; tm_now.tm_isdst = -1;
        from = mktime(&tm_now);
    }

    clear_screen();
    printf("------------ 交易流水（%s） ------------\n", card->id);
    {
        static Transaction tx[LEDGER_MAX];
        int n = campus_load_ledger(card->id, -1, from, tx, LEDGER_MAX);
        if (!n) {
            printf("    该时间段暂无交易。\n");
        } else {
            printf("    流水号  交易时间             类型    商户         金额      余额\n");
            printf("    ------------------------------------------------------------------\n");
            for (int i = 0; i < n; i++)
                printf("    %-7ld %-19s %-6s %-12s %8.2f %9.2f\n",
                       tx[i].seq, tx[i].time, campus_type_text(tx[i].type),
                       tx[i].merchant, tx[i].amount, tx[i].balance);
            printf("    共 %d 条\n", n);
        }
    }
    pause_screen();
}

/* 消费统计：按时间段汇总消费金额与笔数 */
static void student_consume_stats(Card *card) {
    static const MenuItem periods[4] = {
        { 1, "今天" }, { 2, "本周" }, { 3, "本月" }, { 4, "全部" }
    };
    int choice = menu("消费统计时间段", "  请选择统计范围：\n", periods, 4);
    time_t from = 0;
    struct tm tm_now;
    double total = 0.0;
    int n;
    const char *label;

    if (choice == 0) return;
    if (choice != 4) {
        time_t now = time(NULL);
        tm_now = *localtime(&now);
        if (choice == 2) {
            int wday = tm_now.tm_wday;
            tm_now.tm_mday -= (wday == 0) ? 6 : (wday - 1);
        } else if (choice == 3) {
            tm_now.tm_mday = 1;
        }
        tm_now.tm_hour = 0; tm_now.tm_min = 0; tm_now.tm_sec = 0; tm_now.tm_isdst = -1;
        from = mktime(&tm_now);
    }
    label = choice == 1 ? "今天" : (choice == 2 ? "本周" : (choice == 3 ? "本月" : "全部"));

    n = campus_sum_consume(card->id, from, &total);
    clear_screen();
    printf("---------------- 消费统计（%s） ----------------\n", card->id);
    printf("    统计时段：%s\n", label);
    printf("    消费笔数：%d 笔\n", n);
    printf("    消费金额：%.2f 元\n", total);
    printf("    当前余额：%.2f 元\n", card->remain);
    if (n > 0) printf("    平均每笔：%.2f 元\n", total / n);
    printf("------------------------------------------------\n");
    pause_screen();
}

/* 机房上机登记 */
static void student_online(Card *card) {
    static const char *places[PLACE_N] = { "A101 机房", "A102 机房",
                                           "B201 电子阅览室", "B203 创新实验室" };
    int choice;
    char err[64];

    clear_screen();
    printf("---------------- 上机登记 ----------------\n");
    show_card(card);

    if (card->state != ST_NORMAL) {
        printf("该卡状态为「%s」，不能上机！\n", campus_state_text(card->state));
        pause_screen();
        return;
    }
    if (card->online == ONLINE_YES) {
        printf("您已在上机中，无需重复登记。\n");
        pause_screen();
        return;
    }
    if (card->remain <= 0) {
        printf("卡内余额不足，请先充值再上机！\n");
        pause_screen();
        return;
    }

    printf("    请选择上机地点：\n");
    for (int i = 0; i < PLACE_N; i++) printf("      %d. %s\n", i + 1, places[i]);
    printf("      0. 返回\n");
    if (!ask_int("请选择：", &choice) || choice < 1 || choice > PLACE_N) return;

    if (campus_online(card, places[choice - 1], err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("\n上机登记成功！\n    开始时间：%s\n    上机地点：%s\n    计费费率：%.2f 元/小时\n",
           card->start_time, card->address, RATE_PER_HOUR);
    pause_screen();
}

/* 机房下机结账 */
static void student_offline(Card *card) {
    double cost = 0.0;
    char err[64];

    clear_screen();
    printf("---------------- 下机结账 ----------------\n");
    if (campus_offline(card, &cost, err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("下机成功！本次费用 %.2f 元，卡内余额 %.2f 元。\n", cost, card->remain);
    printf("本次上机记录已写入 %s.txt，消费流水已记入一卡通总账。\n", card->id);
    pause_screen();
}

/* 学生修改登录密码 */
static void student_change_pwd(Card *card) {
    char old_pwd[20], new1[20], new2[20];
    char err[64];

    clear_screen();
    printf("---------------- 修改登录密码 ----------------\n");
    printf("请输入原密码：");
    ask_password(old_pwd, sizeof(old_pwd));
    printf("请输入新密码：");
    ask_password(new1, sizeof(new1));
    printf("请再次输入新密码：");
    ask_password(new2, sizeof(new2));
    if (strcmp(new1, new2)) {
        printf("两次输入的新密码不一致！\n");
        pause_screen();
        return;
    }
    if (campus_change_pwd(card, old_pwd, new1, err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("登录密码修改成功！\n");
    pause_screen();
}

/* 学生修改交易密码 */
static void student_change_pay_pwd(Card *card) {
    char old_pwd[20], new1[20], new2[20];
    char err[64];

    clear_screen();
    printf("---------------- 修改交易密码 ----------------\n");
    printf("请输入原交易密码：");
    ask_password(old_pwd, sizeof(old_pwd));
    printf("请输入新交易密码：");
    ask_password(new1, sizeof(new1));
    printf("请再次输入新交易密码：");
    ask_password(new2, sizeof(new2));
    if (strcmp(new1, new2)) {
        printf("两次输入的新密码不一致！\n");
        pause_screen();
        return;
    }
    if (campus_change_pay_pwd(card, old_pwd, new1, err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("交易密码修改成功！\n");
    pause_screen();
}

/* 学生自行挂失 */
static void student_self_lost(Card *card) {
    char err[64];
    clear_screen();
    printf("---------------- 卡片挂失 ----------------\n");
    if (campus_set_lost(card, err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("挂失成功！该卡已不能消费与上机，请到卡务中心补办或解挂。\n");
    pause_screen();
}

/* 学生菜单（登录成功后进入） */
static void student_menu(Card *card) {
    static const MenuItem items[8] = {
        { 1, "查询卡中余额与上机信息" },
        { 2, "查询交易流水" },
        { 3, "消费统计" },
        { 4, "机房上机登记" },
        { 5, "机房下机结账" },
        { 6, "修改登录密码" },
        { 7, "修改交易密码" },
        { 8, "卡片挂失" }
    };
    while (1) {
        char extra[220];
        snprintf(extra, sizeof(extra),
                 "  卡号：%s  姓名：%s  学号：%s\n"
                 "  余额：%.2f 元  状态：%s  今日已消费：%.2f 元  %s\n",
                 card->id, card->name, card->number, card->remain,
                 campus_state_text(card->state), card->used_day,
                 card->online == ONLINE_YES ? "（上机中）" : "");

        switch (menu("学生菜单", extra, items, 8)) {
            case 0: return;
            case 1: student_info(card);            break;
            case 2: student_ledger(card);           break;
            case 3: student_consume_stats(card);    break;
            case 4: student_online(card);           break;
            case 5: student_offline(card);          break;
            case 6: student_change_pwd(card);       break;
            case 7: student_change_pay_pwd(card);   break;
            default: student_self_lost(card);       break;
        }
    }
}

/* ============================================================
 *                        登录与注册
 * ============================================================ */

/* 学生登录：连续三次密码错误则冻结账户。
 * out_card 登录成功后回传卡片。
 * 返回 0 登录成功；-1 卡号不存在；-2 密码错误导致账户被冻结 */
static int student_login(Card **out_card) {
    Card *card;
    int failed = 0;

    clear_screen();
    printf("=============== 学生登录 ===============\n");
    card = pick_card("请输入卡号：");
    if (!card) return -1;

    while (failed < MAX_TRY) {
        char pwd[20];
        printf("请输入密码：");
        ask_password(pwd, sizeof(pwd));

        if (campus_check_pwd(card->id, pwd)) {
            printf("登录成功，欢迎 %s 同学！\n", card->name);
            if (card->state != ST_NORMAL)
                printf("提示：该卡状态为「%s」，仅可查询信息。\n",
                       campus_state_text(card->state));
            pause_screen();
            *out_card = card;
            return 0;
        }
        failed++;
        if (failed < MAX_TRY)
            printf("密码错误！还可尝试 %d 次。\n", MAX_TRY - failed);
    }

    card->state = ST_FROZEN;
    campus_save();
    printf("连续 %d 次密码错误，账户已冻结！请联系管理员重置密码。\n", MAX_TRY);
    pause_screen();
    return -2;
}

/* 管理员登录，成功返回 1 */
static int admin_login(void) {
    int failed;
    for (failed = 0; failed < MAX_TRY; failed++) {
        char pwd[20];
        clear_screen();
        printf("=============== 管理员登录 ===============\n");
        printf("请输入管理员密码：");
        ask_password(pwd, sizeof(pwd));
        if (!strcmp(pwd, ADMIN_PWD)) {
            printf("管理员登录成功！\n");
            pause_screen();
            return 1;
        }
        printf("密码错误！还可尝试 %d 次。\n", MAX_TRY - 1 - failed);
        pause_screen();
    }
    printf("管理员验证失败，返回主菜单。\n");
    pause_screen();
    return 0;
}

/* 新卡注册：学生自设登录密码，交易密码默认取学号后 6 位 */
static void card_register(void) {
    char id[10], name[12], number[12], pwd1[20], pwd2[20];
    char err[64];

    clear_screen();
    printf("=============== 新卡注册 ===============\n");
    ask_text("请输入卡号（如 C004）：", id, sizeof(id));
    ask_text("请输入姓名：", name, sizeof(name));
    ask_text("请输入学号：", number, sizeof(number));
    printf("请设置登录密码（不少于 6 位）：");
    ask_password(pwd1, sizeof(pwd1));
    printf("请再次输入登录密码：");
    ask_password(pwd2, sizeof(pwd2));

    if (strcmp(pwd1, pwd2)) {
        printf("注册失败：两次输入的密码不一致！\n");
        pause_screen();
        return;
    }
    if (campus_register(id, name, number, pwd1, err, sizeof(err))) {
        printf("注册失败：%s\n", err);
        pause_screen();
        return;
    }
    printf("注册成功！\n");
    printf("    卡号：%s  姓名：%s  学号：%s\n", id, name, number);
    printf("    登录密码：你刚才设置的密码\n");
    printf("    交易密码：默认取学号后 6 位，请尽快在「修改交易密码」中更换\n");
    printf("    默认限额：单笔 20 元，日累计 50 元（可到卡务中心调整）\n");
    pause_screen();
}

/* ============================================================
 *                    商户终端（消费）
 * ============================================================ */

/* 通用消费处理。merchant 商户名；device 终端号 */
static void do_pay(const char *merchant, const char *device) {
    char id[10];
    Card *card;
    double amount;
    char err[64];

    clear_screen();
    printf("------------ 商户终端：%s ------------\n", merchant);
    ask_text("请输入卡号：", id, sizeof(id));
    card = campus_find(id);
    if (!card) {
        printf("找不到该卡号！\n");
        pause_screen();
        return;
    }
    if (!ask_money("请输入消费金额（元）：", &amount)) {
        printf("金额格式错误！\n");
        pause_screen();
        return;
    }
    if (!confirm_pay(card, amount, merchant)) return;

    if (campus_pay(card, merchant, amount, card->pay_pwd, device, err, sizeof(err))) {
        printf("交易失败：%s\n", err);
        pause_screen();
        return;
    }
    printf("交易成功！\n    商户：%s\n    金额：%.2f 元\n    余额：%.2f 元\n",
           merchant, amount, card->remain);
    printf("    今日已消费：%.2f 元 / 限额 %.2f 元\n",
           card->used_day, card->limit_day);
    pause_screen();
}

/* 机房按小时自助上机（属于一卡通消费点之一） */
static void merchant_machine(void) {
    char id[10];
    Card *card;
    char err[64];

    clear_screen();
    printf("------------ 商户终端：机房上机 ------------\n");
    ask_text("请输入卡号：", id, sizeof(id));
    card = campus_find(id);
    if (!card) { printf("找不到该卡号！\n"); pause_screen(); return; }

    if (card->online == ONLINE_YES) {
        double cost = 0.0;
        printf("该卡正在上机，是否下机结账？\n");
        printf("1. 下机结账    0. 返回\n");
        {
            int c;
            if (!ask_int("请选择：", &c) || c != 1) return;
        }
        if (campus_offline(card, &cost, err, sizeof(err))) {
            printf("%s\n", err);
            pause_screen();
            return;
        }
        printf("下机成功！本次费用 %.2f 元，余额 %.2f 元。\n", cost, card->remain);
        pause_screen();
        return;
    }

    printf("1. A101 机房   2. A102 机房   3. B201 电子阅览室   4. B203 创新实验室\n");
    {
        static const char *places[PLACE_N] = { "A101 机房", "A102 机房",
                                               "B201 电子阅览室", "B203 创新实验室" };
        int c;
        if (!ask_int("请选择上机地点：", &c) || c < 1 || c > PLACE_N) return;
        if (campus_online(card, places[c - 1], err, sizeof(err))) {
            printf("%s\n", err);
            pause_screen();
            return;
        }
        printf("上机登记成功！地点 %s，开始时间 %s，费率 %.2f 元/小时。\n",
               card->address, card->start_time, RATE_PER_HOUR);
    }
    pause_screen();
}

/* 商户终端菜单 */
static void merchant_menu(void) {
    static const MenuItem items[7] = {
        { 1, "食堂消费" },
        { 2, "超市 / 便利店消费" },
        { 3, "文印 / 打印消费" },
        { 4, "热水 / 洗衣消费" },
        { 5, "宿舍水电缴费" },
        { 6, "图书馆逾期罚款" },
        { 7, "机房上机（登记 / 结账）" }
    };
    while (1) {
        switch (menu("商户终端", "  请选择消费点：\n", items, 7)) {
            case 0:  return;
            case 1:  do_pay("食堂", "POS-CF01");    break;
            case 2:  do_pay("超市", "POS-CS02");    break;
            case 3:  do_pay("文印店", "POS-WY03");  break;
            case 4:  do_pay("热水洗衣", "POS-RS04"); break;
            case 5:  do_pay("宿舍水电", "POS-SD05"); break;
            case 6:  do_pay("图书馆罚款", "POS-TS06"); break;
            default: merchant_machine();            break;
        }
    }
}

/* ============================================================
 *                        卡务中心
 * ============================================================ */

/* 充值 */
static void admin_recharge(void) {
    Card *card;
    double amount;
    char err[64];

    clear_screen();
    printf("---------------- 学生充值 ----------------\n");
    card = pick_card("请输入卡号：");
    if (!card) return;
    show_card(card);

    if (!ask_money("请输入充值金额（元）：", &amount)) {
        printf("金额格式错误！\n");
        pause_screen();
        return;
    }
    if (campus_recharge(card, amount, err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("充值成功！本次充值 %.2f 元，卡内余额 %.2f 元。\n", amount, card->remain);
    pause_screen();
}

/* 挂失 / 解挂 */
static void admin_lost_menu(void) {
    static const MenuItem items[2] = {
        { 1, "挂失（冻结消费与上机）" },
        { 2, "解除挂失" }
    };
    Card *card;
    char err[64];
    int c = menu("挂失与解挂", NULL, items, 2);

    if (c == 0) return;
    clear_screen();
    printf("---------------- 卡片挂失处理 ----------------\n");
    card = pick_card("请输入卡号：");
    if (!card) return;
    show_card(card);

    if (c == 1) {
        if (campus_set_lost(card, err, sizeof(err))) printf("%s\n", err);
        else printf("挂失成功！该卡已不能消费与上机。\n");
    } else {
        if (campus_unlost(card, err, sizeof(err))) printf("%s\n", err);
        else printf("已解除挂失，卡片恢复正常使用。\n");
    }
    pause_screen();
}

/* 冻结 / 解冻 */
static void admin_freeze(void) {
    static const MenuItem items[2] = {
        { 1, "冻结卡片" }, { 2, "解除冻结" }
    };
    Card *card;
    char err[64];
    int c = menu("冻结与解冻", NULL, items, 2);

    if (c == 0) return;
    clear_screen();
    printf("---------------- 卡片冻结处理 ----------------\n");
    card = pick_card("请输入卡号：");
    if (!card) return;
    show_card(card);

    if (campus_freeze(card, c == 1, err, sizeof(err))) printf("%s\n", err);
    else printf("操作成功，当前状态：%s\n", campus_state_text(card->state));
    pause_screen();
}

/* 补办新卡（收工本费，保留余额，解除挂失） */
static void admin_reissue(void) {
    Card *card;
    double fee = 0.0;
    char err[64];

    clear_screen();
    printf("---------------- 补办新卡 ----------------\n");
    card = pick_card("请输入卡号：");
    if (!card) return;
    show_card(card);
    printf("补办说明：卡号不变，余额保留，收工本费，解除挂失，交易密码重置。\n");
    if (!ask_money("请输入工本费（元，可输 0）：", &fee)) {
        printf("金额格式错误！\n");
        pause_screen();
        return;
    }
    if (campus_reissue(card, fee, err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("补办成功！收取工本费 %.2f 元，余额 %.2f 元。\n", fee, card->remain);
    printf("交易密码已重置为 %s，请提醒同学尽快修改。\n", DEFAULT_PWD);
    pause_screen();
}

/* 重置登录密码 / 交易密码 */
static void admin_reset_menu(void) {
    static const MenuItem items[2] = {
        { 1, "重置登录密码（同时解除冻结）" },
        { 2, "重置交易密码" }
    };
    Card *card;
    char err[64];
    int c = menu("重置密码", NULL, items, 2);

    if (c == 0) return;
    clear_screen();
    printf("---------------- 重置密码 ----------------\n");
    card = pick_card("请输入卡号：");
    if (!card) return;

    if (c == 1) {
        if (campus_reset_pwd(card, err, sizeof(err))) printf("%s\n", err);
        else printf("登录密码已重置为 %s，账户冻结已解除。\n", DEFAULT_PWD);
    } else {
        if (campus_reset_pay_pwd(card, err, sizeof(err))) printf("%s\n", err);
        else printf("交易密码已重置为 %s。\n", DEFAULT_PWD);
    }
    pause_screen();
}

/* 设置消费限额 */
static void admin_set_limits(void) {
    Card *card;
    double once, day;
    char err[64];

    clear_screen();
    printf("---------------- 设置消费限额 ----------------\n");
    card = pick_card("请输入卡号：");
    if (!card) return;
    show_card(card);

    if (!ask_money("请输入单笔限额（元，0 表示不限）：", &once)) {
        printf("格式错误！\n"); pause_screen(); return;
    }
    if (!ask_money("请输入日累计限额（元，0 表示不限）：", &day)) {
        printf("格式错误！\n"); pause_screen(); return;
    }
    if (campus_set_limits(card, once, day, err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("限额设置成功！单笔 %.2f 元，日累计 %.2f 元（0 表示不限）。\n", once, day);
    pause_screen();
}

/* 卡片延期 */
static void admin_set_expire(void) {
    Card *card;
    char date[12], err[64];

    clear_screen();
    printf("---------------- 卡片延期 ----------------\n");
    card = pick_card("请输入卡号：");
    if (!card) return;
    printf("    当前有效期：%s\n", card->expire[0] ? card->expire : "长期");
    ask_text("请输入新的有效期（YYYY-MM-DD）：", date, sizeof(date));
    if (campus_set_expire(card, date, err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("延期成功！新的有效期：%s\n", card->expire);
    pause_screen();
}

/* 退款 / 补助发放 */
static void admin_grant(void) {
    static const MenuItem items[2] = {
        { 1, "消费退款" }, { 2, "发放补助" }
    };
    Card *card;
    double amount;
    char memo[32], err[64];
    int c = menu("退款与补助", NULL, items, 2);

    if (c == 0) return;
    clear_screen();
    printf("---------------- %s ----------------\n", c == 1 ? "消费退款" : "发放补助");
    card = pick_card("请输入卡号：");
    if (!card) return;
    show_card(card);
    if (!ask_money("请输入金额（元）：", &amount)) {
        printf("格式错误！\n"); pause_screen(); return;
    }
    ask_text("请输入备注：", memo, sizeof(memo));
    if (campus_grant(card, c == 1 ? TX_REFUND : TX_GRANT, amount, memo,
                     err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("操作成功！余额 %.2f 元。\n", card->remain);
    pause_screen();
}

/* 卡务中心菜单 */
static void card_center_menu(void) {
    static const MenuItem items[7] = {
        { 1, "学生充值" },
        { 2, "挂失 / 解除挂失" },
        { 3, "冻结 / 解除冻结" },
        { 4, "补办新卡（收工本费）" },
        { 5, "重置登录密码 / 交易密码" },
        { 6, "设置消费限额" },
        { 7, "卡片延期 / 退款补助" }
    };
    while (1) {
        switch (menu("卡务中心", NULL, items, 7)) {
            case 0: return;
            case 1: admin_recharge();      break;
            case 2: admin_lost_menu();     break;
            case 3: admin_freeze();        break;
            case 4: admin_reissue();       break;
            case 5: admin_reset_menu();    break;
            case 6: admin_set_limits();    break;
            default: {
                static const MenuItem sub[2] = {
                    { 1, "卡片延期" }, { 2, "退款 / 补助" }
                };
                int c = menu("更多卡务", NULL, sub, 2);
                if (c == 1) admin_set_expire();
                else if (c == 2) admin_grant();
                break;
            }
        }
    }
}

/* ============================================================
 *                          报表
 * ============================================================ */

/* 当天收入统计（保留原题目口径） */
static void report_income(void) {
    double fee = 0.0;
    int times = 0;
    char today[12];
    time_t t = time(NULL);

    campus_today_income(&fee, &times);
    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&t));

    clear_screen();
    printf("------------ 当天机房收入（%s） ------------\n", today);
    printf("    上机费收入：%.2f 元\n", fee);
    printf("    上机人次：%d 次\n", times);
    printf("    平均每人次：%.2f 元\n", times ? fee / times : 0.0);
    printf("--------------------------------------------\n");
    pause_screen();
}

/* 一卡通当天全站汇总（按商户分别统计） */
static void report_all(void) {
    double consume = 0.0, recharge = 0.0;
    double msum[MERCHANT_MAX];
    const char *names[MERCHANT_MAX];
    int times = 0, mn, n;
    char today[12];
    time_t t = time(NULL);

    campus_today_report(&consume, &times, &recharge, msum);
    mn = campus_merchants(names, MERCHANT_MAX);
    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&t));

    clear_screen();
    printf("---------- 一卡通当天汇总（%s） ----------\n", today);
    printf("    消费总额：%.2f 元\n", consume);
    printf("    消费笔数：%d 笔\n", times);
    printf("    充值 / 补助总额：%.2f 元\n", recharge);
    printf("    ---- 各消费点消费额 ----\n");
    for (int i = 0; i < mn; i++)
        printf("      %-10s %8.2f 元\n", names[i], msum[i]);
    printf("    ------------------------\n");
    n = campus_count();
    printf("    发卡总数：%d 张\n", n);
    pause_screen();
}

/* 交易流水查询（管理员按卡或全部） */
static void report_ledger(void) {
    static const MenuItem items[2] = {
        { 1, "查看全部流水" }, { 2, "按卡号查看" }
    };
    int c = menu("交易流水查询", NULL, items, 2);
    if (c == 0) return;

    clear_screen();
    printf("---------------- 交易流水 ----------------\n");
    if (c == 1) {
        show_ledger(NULL, -1, 30);
    } else {
        char id[10];
        ask_text("请输入卡号：", id, sizeof(id));
        if (!campus_find(id)) {
            printf("找不到该卡号！\n");
            pause_screen();
            return;
        }
        show_ledger(id, -1, 50);
    }
    pause_screen();
}

/* 门禁 / 考勤刷卡 */
static void report_access(void) {
    static const MenuItem items[3] = {
        { 1, "宿舍门禁刷卡" },
        { 2, "教学楼考勤签到" },
        { 3, "查看门禁考勤记录" }
    };
    int c = menu("门禁与考勤", NULL, items, 3);
    Card *card;
    char id[10], err[64];

    if (c == 0) return;
    clear_screen();

    if (c == 3) {
        static AccessLog logs[ACCESS_MAX];
        int n = campus_load_access(NULL, logs, ACCESS_MAX);
        int start = n > 40 ? n - 40 : 0;
        printf("---------------- 门禁 / 考勤记录 ----------------\n");
        if (!n) {
            printf("    暂无刷卡记录。\n");
        } else {
            printf("    卡号   刷卡时间             方向   用途   地点\n");
            printf("    ---------------------------------------------------\n");
            for (int i = start; i < n; i++)
                printf("    %-6s %-19s %-6s %-6s %s\n", logs[i].id, logs[i].time,
                       logs[i].dir == ACC_IN ? "进入" : "离开",
                       logs[i].kind == ACCESS_DOOR ? "门禁" : "考勤",
                       logs[i].place);
            printf("    共 %d 条（显示最近 %d 条）\n", n, n - start);
        }
        pause_screen();
        return;
    }

    printf("---------------- %s ----------------\n", c == 1 ? "宿舍门禁刷卡" : "考勤签到");
    ask_text("请输入卡号：", id, sizeof(id));
    card = campus_find(id);
    if (!card) { printf("找不到该卡号！\n"); pause_screen(); return; }

    {
        const char *place = c == 1 ? "3 号宿舍楼" : "教学楼 A 座";
        int dir;
        printf("    地点：%s\n", place);
        printf("    1. 进入 / 签到    2. 离开 / 签退\n");
        if (!ask_int("请选择：", &dir) || dir < 1 || dir > 2) return;
        if (campus_access(card, dir == 1 ? ACC_IN : ACC_OUT,
                          c == 1 ? ACCESS_DOOR : ACCESS_ATTEND,
                          place, err, sizeof(err))) {
            printf("刷卡失败：%s\n", err);
            pause_screen();
            return;
        }
        printf("刷卡成功！%s 于 %s %s %s\n", card->name, place,
               dir == 1 ? "进入" : "离开",
               c == 1 ? "（门禁）" : "（考勤）");
    }
    pause_screen();
}

/* 全部卡片一览 */
static void report_cards(void) {
    int n = campus_count();
    clear_screen();
    printf("---------------- 全部校园卡（%d 张） ----------------\n", n);
    printf("    卡号   姓名   学号         余额      累计充值  状态    今日消费\n");
    printf("    ------------------------------------------------------------------\n");
    for (int i = 0; i < n; i++) {
        Card *k = campus_at(i);
        printf("    %-6s %-6s %-12s %8.2f %9.2f  %-6s %8.2f\n",
               k->id, k->name, k->number, k->remain, k->recharge,
               campus_state_text(k->state), k->used_day);
    }
    printf("----------------------------------------------------------------------\n");
    pause_screen();
}

/* 报表菜单 */
static void report_menu(void) {
    static const MenuItem items[5] = {
        { 1, "当天机房收入" },
        { 2, "一卡通当天汇总（按消费点）" },
        { 3, "交易流水查询" },
        { 4, "门禁 / 考勤" },
        { 5, "全部卡片一览" }
    };
    while (1) {
        switch (menu("统计报表", NULL, items, 5)) {
            case 0: return;
            case 1: report_income(); break;
            case 2: report_all();    break;
            case 3: report_ledger(); break;
            case 4: report_access(); break;
            default: report_cards(); break;
        }
    }
}

/* 管理员主菜单 */
static void admin_menu(void) {
    static const MenuItem items[3] = {
        { 1, "卡务中心（充值 / 挂失 / 补办 / 限额）" },
        { 2, "商户终端（食堂 / 超市 / 文印 / 水电 / 罚款 / 机房）" },
        { 3, "统计报表（收入 / 流水 / 门禁 / 卡片）" }
    };
    while (1) {
        switch (menu("管理员菜单", NULL, items, 3)) {
            case 0: return;
            case 1: card_center_menu(); break;
            case 2: merchant_menu();    break;
            default: report_menu();     break;
        }
    }
}

/* ============================================================
 *                          主函数
 * ============================================================ */
int main(void) {
    static const MenuItem roles[3] = {
        { 1, "学生登录（查询 / 消费 / 上机）" },
        { 2, "管理员登录（卡务 / 商户 / 报表）" },
        { 3, "新卡注册" }
    };

#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
    campus_init("data");

    while (1) {
        switch (menu("校园一卡通系统", "  请选择登录角色：\n", roles, 3)) {
            case 1: {
                Card *card = NULL;
                if (student_login(&card) == 0) student_menu(card);
                break;
            }
            case 2:
                if (admin_login()) admin_menu();
                break;
            case 3:
                card_register();
                break;
            default:
                campus_save();
                printf("\n数据已保存，感谢使用，再见！\n");
                return 0;
        }
    }
}
