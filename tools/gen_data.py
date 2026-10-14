"""生成大样本演示数据，用于压测。

用法：python tools/gen_data.py [数据目录] [卡数] [每卡流水数]
默认：data 200 张卡，每卡 60 笔流水（约 1.2 万笔），每卡 3~8 条上机记录
"""
import io
import os
import sys
import random
import datetime

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

DATA = sys.argv[1] if len(sys.argv) > 1 else 'data'
NCARD = int(sys.argv[2]) if len(sys.argv) > 2 else 200
NTX = int(sys.argv[3]) if len(sys.argv) > 3 else 60

MERCHANTS = ['食堂', '超市', '文印店', '热水洗衣', '宿舍水电', '图书馆罚款', '机房']
DEVICES = {'食堂': 'POS-CF01', '超市': 'POS-CS02', '文印店': 'POS-WY03',
           '热水洗衣': 'POS-RS04', '宿舍水电': 'POS-SD05',
           '图书馆罚款': 'POS-TS06', '机房': 'PC-01'}
SURNAME = '赵钱孙李周吴郑王冯陈褚卫蒋沈韩杨朱秦尤许何吕施张孔曹严华金魏陶姜'
GIVEN = ['小雨', '子涵', '梓萱', '浩然', '欣怡', '宇轩', '诗琪', '俊杰', '雨桐',
         '晨曦', '佳怡', '思远', '梦洁', '文博', '静怡', '泽宇', '紫涵', '明轩']
MAJORS = ['B1701', 'B1702', 'B1703', 'B1704', 'B1801', 'B1802']

random.seed(20261014)
os.makedirs(DATA, exist_ok=True)

today = datetime.date.today()


def day_str(d):
    return d.strftime('%Y-%m-%d')


def ts(d, h, m, s):
    return '%s %02d:%02d:%02d' % (day_str(d), h, m, s)


cards = []
ledger = []
access = []
seq = 0
merchant_sum = {m: 0.0 for m in MERCHANTS}

for i in range(1, NCARD + 1):
    cid = 'C%04d' % i
    name = random.choice(SURNAME) + random.choice(GIVEN)
    number = '%s%03d' % (random.choice(MAJORS), i)
    pwd = '123456'
    pay_pwd = number[-6:]
    recharge = round(random.choice([0, 20, 30, 50, 100, 200, 300]), 2)
    remain = recharge
    state = 0
    online = 0
    limit_once = random.choice([10.0, 15.0, 20.0, 30.0, 50.0])
    limit_day = random.choice([30.0, 50.0, 80.0, 100.0, 200.0])
    used_day = 0.0
    expire = '2029-12-31'

    # 该卡在最近 NTX 天里的消费与充值
    for k in range(NTX):
        d = today - datetime.timedelta(days=random.randint(0, 89))
        h, mi, s = random.randint(6, 22), random.randint(0, 59), random.randint(0, 59)
        r = random.random()
        if r < 0.12:
            # 充值
            amt = float(random.choice([20, 30, 50, 100]))
            seq += 1
            remain = round(remain + amt, 2)
            recharge = round(recharge + amt, 2)
            ledger.append((seq, ts(d, h, mi, s), cid, 0, '充值', amt, remain, 'ADMIN', '管理员充值'))
        else:
            m = random.choice(MERCHANTS)
            amt = round(random.choice([1, 2, 3, 5, 8, 12, 15, 18, 25]) * random.choice([1, 1, 0.5]), 2)
            if remain < amt:
                continue
            seq += 1
            remain = round(remain - amt, 2)
            merchant_sum[m] = round(merchant_sum[m] + amt, 2)
            if d == today:
                used_day = round(used_day + amt, 2)
            ledger.append((seq, ts(d, h, mi, s), cid, 1, m,
                           amt, remain, DEVICES[m], '消费扣款'))
        # 少量门禁记录
        if random.random() < 0.06:
            place = random.choice(['3 号宿舍楼', '教学楼 A 座', '图书馆', '实验楼 B 座'])
            access.append((cid, ts(d, h, mi, s), random.randint(0, 1), place,
                           random.randint(0, 1)))

    cards.append((cid, name, number, pwd, pay_pwd, recharge, remain, state, online,
                  '', '', limit_day, used_day, day_str(today), limit_once, expire))

# ---------------- 写文件 ----------------
with open(os.path.join(DATA, 'cards.dat'), 'w', encoding='utf-8') as f:
    for c in cards:
        f.write('%s|%s|%s|%s|%s|%.2f|%.2f|%d|%d|%s|%s|%.2f|%.2f|%s|%.2f|%s\n' % c)

with open(os.path.join(DATA, 'ledger.dat'), 'w', encoding='utf-8') as f:
    for t in ledger:
        f.write('%d|%s|%s|%d|%s|%.2f|%.2f|%s|%s\n' % t)

with open(os.path.join(DATA, 'access.dat'), 'w', encoding='utf-8') as f:
    for a in access:
        f.write('%s|%s|%d|%s|%d\n' % a)

# 每卡的上机记录文件（取前 40 张卡生成，避免文件过多）
n_machine = min(NCARD, 40)
for i in range(1, n_machine + 1):
    cid = 'C%04d' % i
    with open(os.path.join(DATA, '%s.txt' % cid), 'w', encoding='utf-8') as f:
        for _ in range(random.randint(3, 8)):
            d = today - datetime.timedelta(days=random.randint(0, 30))
            h, mi = random.randint(8, 21), random.randint(0, 59)
            dur = random.randint(10, 240)
            cost = round(dur / 60.0 * 2.0, 2)
            f.write('%s|%s|%02d:%02d:%02d|%d|%.2f|已下机|%s\n' % (
                cid, day_str(d), h, mi, 0, dur, cost,
                random.choice(['A101 机房', 'A102 机房', 'B201 电子阅览室', 'B203 创新实验室'])))

total_consume = round(sum(t[5] for t in ledger if t[3] == 1), 2)
print('已生成到 %s' % os.path.abspath(DATA))
print('  卡片      : %d 张' % len(cards))
print('  交易流水  : %d 笔（消费 %d 笔，合计 %.2f 元）' % (
    len(ledger), sum(1 for t in ledger if t[3] == 1), total_consume))
print('  门禁记录  : %d 条' % len(access))
print('  上机记录文件: %d 个' % n_machine)
print('  各消费点消费额:')
for m in MERCHANTS:
    print('     %-10s %10.2f' % (m, merchant_sum[m]))
