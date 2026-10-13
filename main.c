/* ============================================================
 *      模拟校园卡 —— 机房上机计费与消费记录查询系统
 * ------------------------------------------------------------
 *  题目要求与实现的对应关系：
 *    ① 登录模块    student_login()   同学按自己设定的密码登录，
 *                                   连续三次错误则冻结账户
 *    ② 管理员      admin_menu()      充值、挂失、重置学生密码
 *    ③ 学生查询    student_info()    卡中余额、上机时间与地点
 *    ④ 学生统计    student_stats()   上机时间与费用（可选时间段）
 *    ⑤ 上机记录    每生一个文本文件 <卡号>.txt，追加写入、永久保存
 *    ⑥ 当天收入    admin_income()    管理员统计全部当天收入情况
 *    ⑦ 菜单选择    角色选择、时间选择、功能选择
 *    ⑧ 输入检查    所有输入均校验，非法输入提示后重新输入
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

/* ============================================================
 *                      基础交互工具
 * ============================================================ */

/* 清屏。先刷新标准输出，避免内容滞留在缓冲区 */
static void clear_screen(void) {
    fflush(stdout);
    system(CLEAR_CMD);
}

/* 暂停等待用户按回车。输入被重定向（自动化测试）时不暂停，避免吞掉后续输入 */
static void pause_screen(void) {
    int ch;
    if (!isatty(0)) return;
    printf("\n按回车键继续...");
    fflush(stdout);
    while ((ch = getchar()) != '\n' && ch != EOF) ;
}

/* 读取一行输入并去掉行尾换行符。
 * prompt 提示语；buf 输出缓冲区；size 缓冲区大小。
 * 输入流结束返回 0（调用方据此安全退出），正常返回 1 */
static int read_line(const char *prompt, char *buf, int size) {
    printf("%s", prompt);
    if (!fgets(buf, size, stdin)) { buf[0] = '\0'; return 0; }
    buf[strcspn(buf, "\r\n")] = '\0';
    return 1;
}

/* 必须读到一行文本：读不到（输入结束）直接退出，避免死循环。
 * prompt 提示语；buf 输出缓冲区；size 缓冲区大小 */
static void ask_text(const char *prompt, char *buf, int size) {
    if (!read_line(prompt, buf, size)) exit(0);
}

/* 读取整数。prompt 提示语；out 输出结果。
 * 成功返回 1，格式错误返回 0 */
static int ask_int(const char *prompt, int *out) {
    char buf[32];
    ask_text(prompt, buf, sizeof(buf));
    return sscanf(buf, "%d", out) == 1;
}

/* 读取金额。prompt 提示语；out 输出结果。
 * 成功返回 1，格式错误返回 0 */
static int ask_money(const char *prompt, double *out) {
    char buf[32];
    ask_text(prompt, buf, sizeof(buf));
    return sscanf(buf, "%lf", out) == 1;
}

/* 读取密码：控制台逐字符读且以 * 回显；输入被重定向时按行读。
 * buf 输出缓冲区；size 缓冲区大小 */
static void ask_password(char *buf, int size) {
    if (!isatty(0)) {                       /* 管道/文件输入：直接读一行 */
        if (!fgets(buf, size, stdin)) buf[0] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';
        return;
    }
#ifdef _WIN32
    int i = 0, ch;
    while ((ch = _getch()) != '\r' && ch != EOF) {
        if (ch == 8) {                      /* 退格键 */
            if (i > 0) { i--; printf("\b \b"); }
        } else if (i < size - 1) {
            buf[i++] = (char)ch;
            putchar('*');
        }
    }
    buf[i] = '\0';
#else
    struct termios old_term, new_term;
    int i = 0, ch;
    tcgetattr(0, &old_term);
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);   /* 关闭行缓冲与回显 */
    tcsetattr(0, TCSANOW, &new_term);
    while ((ch = getchar()) != '\n' && ch != EOF) {
        if ((ch == 8 || ch == 127) && i > 0) { i--; printf("\b \b"); }
        else if (ch != 8 && ch != 127 && i < size - 1) { buf[i++] = (char)ch; putchar('*'); }
    }
    buf[i] = '\0';
    tcsetattr(0, TCSANOW, &old_term);       /* 恢复终端设置 */
#endif
    printf("\n");
}

/* 菜单项：key 功能编号，text 功能说明 */
typedef struct {
    int         key;
    const char *text;
} MenuItem;

/* 通用功能菜单（满足题目"功能选择"要求）。
 * head 菜单标题；extra 标题下的附加信息（可为 NULL）；
 * items 菜单项数组；n 菜单项数量。
 * 返回用户选中的功能编号；选 0 返回 0（表示返回/退出） */
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

        if (!ask_int("请选择功能：", &choice)) {     /* 非数字：提示后重选 */
            printf("输入无效，请输入功能编号！\n");
            pause_screen();
            continue;
        }
        if (choice == 0) return 0;
        for (int i = 0; i < n; i++)
            if (items[i].key == choice) return choice;  /* 命中合法功能 */
        printf("无效的选择！\n");
        pause_screen();
    }
}

/* 按卡号选择卡片（管理员操作与登录共用）。
 * prompt 提示语。成功返回卡片指针；找不到时提示原因并返回 NULL */
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

/* ============================================================
 *                        显示辅助
 * ============================================================ */

/* 显示卡片基本信息（余额、状态、上机情况） */
static void show_card(const Card *card, const char *extra) {
    printf("---------------- 卡内信息 ----------------\n");
    printf("    卡号：%-6s 姓名：%-6s 学号：%s\n", card->id, card->name, card->number);
    printf("    累计充值：%.2f 元    余额：%.2f 元\n", card->recharge, card->remain);
    printf("    卡片状态：%s\n", campus_state_text(card->state));
    if (card->online == ONLINE_YES)
        printf("    正在上机：地点 %s，开始时间 %s\n", card->address, card->start_time);
    else
        printf("    上机状态：未上机\n");
    if (extra && *extra) printf("%s", extra);
    printf("------------------------------------------\n");
}

/* ============================================================
 *                        学生端功能
 * ============================================================ */

/* ③ 查询卡中余额、上机时间和地点 */
static void student_info(Card *card) {
    clear_screen();
    show_card(card, NULL);
    if (card->online == ONLINE_YES) {
        /* 上机中：估算已产生的费用，便于学生随时了解消费情况 */
        time_t t0 = time(NULL);
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
            minutes = (t1 == (time_t)-1) ? 0.0 : difftime(t0, t1) / 60.0;
        }
        if (minutes < 0) minutes = 0;
        printf("    已上机约 %.0f 分钟，预计费用 %.2f 元（费率 %.2f 元/小时）\n",
               minutes, minutes / 60.0 * RATE_PER_HOUR, RATE_PER_HOUR);
    }
    pause_screen();
}

/* 上机登记：记录开始时间与上机地点 */
static void student_online(Card *card) {
    static const char *places[4] = { "A101 机房", "A102 机房",
                                     "B201 电子阅览室", "B203 创新实验室" };
    int choice;
    char err[64];

    clear_screen();
    printf("---------------- 上机登记 ----------------\n");
    show_card(card, NULL);

    if (card->state != ST_NORMAL) {              /* 挂失或冻结不能上机 */
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
        printf("卡内余额不足，请先到管理员处充值再上机！\n");
        pause_screen();
        return;
    }

    printf("    请选择上机地点：\n");
    for (int i = 0; i < 4; i++) printf("      %d. %s\n", i + 1, places[i]);
    printf("      0. 返回\n");
    if (!ask_int("请选择：", &choice) || choice < 1 || choice > 4) return;

    if (campus_online(card, places[choice - 1], err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("\n上机登记成功！\n    开始时间：%s\n    上机地点：%s\n    计费费率：%.2f 元/小时\n",
           card->start_time, card->address, RATE_PER_HOUR);
    pause_screen();
}

/* 下机结账：按用时计费并写入上机记录 */
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
    printf("本次上机记录已写入 %s.txt，可随时查询与统计。\n", card->id);
    pause_screen();
}

/* ④ 统计自己上机时间与费用，支持时间段选择 */
static void student_stats(Card *card) {
    static const MenuItem periods[4] = {
        { 1, "今天" }, { 2, "本周" }, { 3, "本月" }, { 4, "全部" }
    };
    int choice, count = 0, minutes = 0;
    double cost = 0.0;
    time_t from = 0;
    struct tm tm_now;

    choice = menu("统计时间段", "  请选择要统计的时间范围：\n", periods, 4);
    if (choice == 0) return;

    if (choice != 4) {                          /* 4 表示全部，起始时间取 0 */
        time_t now = time(NULL);
        tm_now = *localtime(&now);
        if (choice == 2) {                      /* 本周：退到本周一 */
            int wday = tm_now.tm_wday;          /* 0 表示周日 */
            tm_now.tm_mday -= (wday == 0) ? 6 : (wday - 1);
        } else if (choice == 3) {               /* 本月：退到 1 日 */
            tm_now.tm_mday = 1;
        }
        tm_now.tm_hour = 0;
        tm_now.tm_min = 0;
        tm_now.tm_sec = 0;
        tm_now.tm_isdst = -1;
        from = mktime(&tm_now);
    }

    campus_stats(card->id, from, &count, &minutes, &cost);
    clear_screen();
    printf("---------------- 上机统计（%s） ----------------\n", card->id);
    printf("    统计时段：%s\n", choice == 4 ? "全部记录"
                                          : (choice == 1 ? "今天"
                                          : (choice == 2 ? "本周" : "本月")));
    printf("    上机次数：%d 次\n", count);
    printf("    累计用时：%d 分钟（约 %.2f 小时）\n", minutes, minutes / 60.0);
    printf("    累计费用：%.2f 元\n", cost);
    if (count > 0)
        printf("    平均每次：%.1f 分钟 / %.2f 元\n",
               (double)minutes / count, cost / count);
    printf("------------------------------------------------\n");
    pause_screen();
}

/* 学生菜单（登录成功后进入） */
static void student_menu(Card *card) {
    static const MenuItem items[5] = {
        { 1, "查询卡中余额、上机时间和地点" },
        { 2, "上机登记" },
        { 3, "下机结账" },
        { 4, "统计上机时间与费用" },
        { 5, "修改登录密码" }
    };
    while (1) {
        char extra[160];
        snprintf(extra, sizeof(extra),
                 "  卡号：%s  姓名：%s  学号：%s\n  余额：%.2f 元  状态：%s  %s\n",
                 card->id, card->name, card->number, card->remain,
                 campus_state_text(card->state),
                 card->online == ONLINE_YES ? "（上机中）" : "");

        switch (menu("学生菜单", extra, items, 5)) {
            case 0: return;
            case 1: student_info(card);    break;
            case 2: student_online(card);  break;
            case 3: student_offline(card); break;
            case 4: student_stats(card);   break;
            default: {                        /* 修改自己设定的密码 */
                char old_pwd[20], new1[20], new2[20];
                clear_screen();
                printf("---------------- 修改登录密码 ----------------\n");
                printf("请输入原密码：");
                ask_password(old_pwd, sizeof(old_pwd));
                if (!campus_check_pwd(card->id, old_pwd)) {
                    printf("原密码错误！\n");
                    pause_screen();
                    break;
                }
                printf("请输入新密码：");
                ask_password(new1, sizeof(new1));
                if (strlen(new1) < 6) {
                    printf("新密码不得少于 6 位！\n");
                    pause_screen();
                    break;
                }
                printf("请再次输入新密码：");
                ask_password(new2, sizeof(new2));
                if (strcmp(new1, new2)) {
                    printf("两次输入的新密码不一致！\n");
                    pause_screen();
                    break;
                }
                snprintf(card->pwd, sizeof(card->pwd), "%s", new1);
                campus_save();
                printf("密码修改成功！\n");
                pause_screen();
                break;
            }
        }
    }
}

/* ============================================================
 *                        登录模块
 * ============================================================ */

/* ① 学生登录：同学按自己设定的密码登录，连续三次错误则冻结账户。
 * c 上下文；out_card 登录成功后回传卡片。
 * 返回 0 登录成功；-1 卡号不存在；-2 密码连续错误账户被冻结 */
static int student_login(Card **out_card) {
    Card *card;
    int failed = 0;                             /* 已连续失败次数 */

    clear_screen();
    printf("=============== 学生登录 ===============\n");
    card = pick_card("请输入卡号：");
    if (!card) return -1;

    while (failed < MAX_TRY) {
        char pwd[20];
        printf("请输入密码：");
        ask_password(pwd, sizeof(pwd));

        if (campus_check_pwd(card->id, pwd)) {       /* 密码正确 */
            printf("登录成功，欢迎 %s 同学！\n", card->name);
            if (card->state == ST_FROZEN)               /* 冻结账户仅可查看 */
                printf("提示：该卡已冻结，只能查询信息，请联系管理员。\n");
            else if (card->state == ST_LOST)
                printf("提示：该卡已挂失，不能上机。\n");
            pause_screen();
            *out_card = card;
            return 0;
        }
        failed++;
        if (failed < MAX_TRY)
            printf("密码错误！还可尝试 %d 次。\n", MAX_TRY - failed);
    }

    /* 三次均错误：冻结账户（题目要求） */
    card->state = ST_FROZEN;
    campus_save();
    printf("连续 %d 次密码错误，账户已冻结！请联系管理员重置密码。\n", MAX_TRY);
    pause_screen();
    return -2;
}

/* 管理员登录，密码正确返回 1，否则返回 0 */
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

/* ============================================================
 *                        管理员功能
 * ============================================================ */

/* ② 充值：增加余额与累计充值数额 */
static void admin_recharge(void) {
    Card *card;
    double amount;
    char err[64];

    clear_screen();
    printf("---------------- 学生充值 ----------------\n");
    card = pick_card("请输入卡号：");
    if (!card) return;
    show_card(card, NULL);

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

/* ② 挂失：挂失期间不能上机（正在上机则先自动结账） */
static void admin_lost(void) {
    Card *card;
    char err[64];
    double cost = 0.0;

    clear_screen();
    printf("---------------- 卡片挂失 ----------------\n");
    card = pick_card("请输入卡号：");
    if (!card) return;
    show_card(card, NULL);

    if (card->online == ONLINE_YES)
        printf("提示：该卡正在上机，挂失将先按下机结账。\n");
    if (campus_set_lost(card, err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("挂失成功！该卡当前状态为「挂失」，不能再上机。\n");
    if (cost > 0) printf("已自动结账，本次费用 %.2f 元。\n", cost);
    pause_screen();
}

/* ② 重置学生密码：恢复为默认密码，并解除冻结状态 */
static void admin_reset_pwd(void) {
    Card *card;
    char err[64];

    clear_screen();
    printf("---------------- 重置学生密码 ----------------\n");
    card = pick_card("请输入卡号：");
    if (!card) return;
    if (campus_reset_pwd(card, err, sizeof(err))) {
        printf("%s\n", err);
        pause_screen();
        return;
    }
    printf("卡号 %s（%s）的密码已重置为 %s，账户冻结已解除。\n",
           card->id, card->name, DEFAULT_PWD);
    pause_screen();
}

/* ⑥ 统计全部当天收入情况（题目选做要求） */
static void admin_income(void) {
    double fee = 0.0;                           /* 当天上机费合计 */
    int times = 0;                              /* 当天上机人次   */
    char today[12];
    time_t t = time(NULL);

    campus_today_income(&fee, &times);
    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&t));

    clear_screen();
    printf("------------ 当天收入统计（%s） ------------\n", today);
    printf("    上机费收入：%.2f 元\n", fee);
    printf("    上机人次：%d 次\n", times);
    printf("    平均每人次：%.2f 元\n", times ? fee / times : 0.0);
    printf("--------------------------------------------\n");
    pause_screen();
}

/* 管理员菜单 */
static void admin_menu(void) {
    static const MenuItem items[4] = {
        { 1, "学生充值" },
        { 2, "卡片挂失" },
        { 3, "重置学生密码（同时解除冻结）" },
        { 4, "统计当天收入情况" }
    };
    while (1) {
        switch (menu("管理员菜单", NULL, items, 4)) {
            case 0: return;
            case 1: admin_recharge();  break;
            case 2: admin_lost();      break;
            case 3: admin_reset_pwd(); break;
            default: admin_income();   break;
        }
    }
}

/* ============================================================
 *                          主函数
 * ============================================================ */
int main(void) {
    /* 业务数据由业务层全局持有，主函数只负责菜单调度 */
    static const MenuItem roles[2] = {          /* 角色选择菜单   */
        { 1, "学生登录" },
        { 2, "管理员登录" }
    };

#ifdef _WIN32
    SetConsoleOutputCP(65001);                  /* 控制台按 UTF-8 输出中文 */
#endif
    campus_init("data");               /* 数据存放于 data/ 目录  */

    while (1) {
        switch (menu("模拟校园卡系统", "  请选择登录角色：\n", roles, 2)) {
            case 1: {                           /* 学生：登录后进入学生菜单 */
                Card *card = NULL;
                if (student_login(&card) == 0) student_menu(card);
                break;
            }
            case 2:                             /* 管理员：登录后进入管理菜单 */
                if (admin_login()) admin_menu();
                break;
            default:                            /* 选 0 退出系统 */
                campus_save();
                printf("\n数据已保存，感谢使用，再见！\n");
                return 0;
        }
    }
}
