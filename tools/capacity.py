"""容量边界测试（改进版）：区分「分页」与「静默截断」，并检查告警标记。"""
import io
import sys
import json
import time
import urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
BASE = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8092'
RAW_CARDS = int(sys.argv[2]) if len(sys.argv) > 2 else 300
RAW_TX = int(sys.argv[3]) if len(sys.argv) > 3 else 30832


def call(path, method='GET', body=None, hdr=None, timeout=180):
    h = dict(hdr or {})
    data = json.dumps(body, ensure_ascii=False).encode() if body is not None else None
    if data:
        h['Content-Type'] = 'application/json'
    req = urllib.request.Request(BASE + path, data=data, headers=h, method=method)
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
        return time.time() - t0, len(raw), json.loads(raw or b'{}')
    except Exception as e:
        return time.time() - t0, 0, {'__error__': '%s: %s' % (type(e).__name__, str(e)[:70])}


A = {'X-Token': 'ADMIN', 'X-Pwd': 'admin123'}
print('capacity test:', BASE, ' 数据集: %d 卡 / %d 流水' % (RAW_CARDS, RAW_TX))
print('=' * 72)

print('【A】卡片上限与告警标记')
dt, size, d = call('/api/admin/cards', hdr=A)
n, cmax = d.get('total'), d.get('cardMax')
trunc = d.get('cardTruncated')
print('  服务载入卡片 : %s 张（上限 %s）' % (n, cmax))
print('  数据文件卡片 : %d 张' % RAW_CARDS)
print('  截断标记     : cardTruncated = %s' % trunc)
if n and n < RAW_CARDS:
    if trunc:
        print('  [OK ] 确实丢卡，且**已通过 cardTruncated 明确告警**（不再是静默截断）')
    else:
        print('  [FAIL] 丢卡但没有任何告警 —— 静默截断')
else:
    print('  [OK ] 未丢卡')

print()
print('【B】流水分页（不再返回畸形 JSON）')
dt, size, d = call('/api/admin/ledger?period=all', hdr=A)
if '__error__' in d:
    print('  [FAIL] 解析失败:', d['__error__'])
else:
    total, cnt, off, lim = d.get('total'), d.get('count'), d.get('offset'), d.get('limit')
    more = d.get('hasMore')
    print('  total=%s count=%s offset=%s limit=%s hasMore=%s  响应 %.0f KB  %.2fs' % (
        total, cnt, off, lim, more, size / 1024.0, dt))
    ok = (total == RAW_TX and cnt == lim and more is True and size < 400 * 1024)
    print('  [%s] 总数为真实总数、单页受限、JSON 完整、响应体积可控' % ('OK ' if ok else 'FAIL'))

    print('  翻页验证：')
    seen = set()
    for off in [0, 300, 600]:
        _, _, dd = call('/api/admin/ledger?period=all&offset=%d' % off, hdr=A)
        recs = dd.get('records', [])
        first = recs[0]['seq'] if recs else None
        dup = sum(1 for r in recs if r['seq'] in seen)
        for r in recs:
            seen.add(r['seq'])
        print('    offset=%-4d 返回 %d 条，首条流水号=%s，与前面重复 %d 条' % (
            off, len(recs), first, dup))

print()
print('【C】门禁记录同样受限')
dt, size, d = call('/api/admin/access?id=', hdr=A)
if '__error__' in d:
    print('  [FAIL]', d['__error__'])
else:
    print('  total=%s count=%s truncated=%s  响应 %.0f KB' % (
        d.get('total'), d.get('count'), d.get('truncated'), size / 1024.0))

print()
print('【D】报表正确性（不受分页影响）')
dt, size, d = call('/api/admin/report', hdr=A)
print('  当天消费 %.2f 元 / %s 笔，充值 %.2f 元，机房 %.2f 元  %.2fs' % (
    d.get('consumeTotal', 0), d.get('consumeTimes'),
    d.get('rechargeTotal', 0), d.get('feeIncome', 0), dt))

print()
print('【E】健康检查暴露容量状态')
dt, size, d = call('/api/health')
print('  cards=%s cardMax=%s cardTruncated=%s' % (
    d.get('cards'), d.get('cardMax'), d.get('cardTruncated')))
