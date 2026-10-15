"""生成校园一卡通的 Windows 图标（.ico，含多尺寸）与前端图标。

纯标准库实现：手写 PNG 编码（zlib + CRC32）与 ICO 封装，
不依赖 Pillow。图标造型为一张竖版卡片：深蓝渐变卡面、
芯片、磁条与两行文字条，缩小后仍可辨识。
"""
import io
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 品牌色
C_DARK = (30, 64, 175)      # 深蓝（卡面上部）
C_MAIN = (37, 99, 235)      # 主蓝
C_LIGHT = (96, 165, 250)    # 浅蓝（渐变下部）
C_GOLD = (250, 204, 21)     # 金色（芯片）
C_WHITE = (255, 255, 255)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def rounded_rect_alpha(x, y, w, h, r, size):
    """判断点是否落在圆角矩形内（返回 0..255 覆盖率，做简单抗锯齿）"""
    # 距四条边的距离，判断圆角
    cx = min(max(x, r), w - r)
    cy = min(max(y, r), h - r)
    dx = x - cx
    dy = y - cy
    d = (dx * dx + dy * dy) ** 0.5
    if x < 0 or y < 0 or x > w or y > h:
        return 0
    if d <= r - 1:
        return 255
    if d >= r + 1:
        return 0
    return int(255 * (r + 1 - d) / 2)


def make_card(size):
    """生成 size x size 的 RGBA 像素（行优先）"""
    S = size
    px = bytearray(S * S * 4)

    # 卡片在画布中的位置（留边距）
    m = max(1, int(S * 0.10))
    cw = S - 2 * m
    ch = int(cw * 0.66)                 # 卡片宽高比，横版
    cy0 = (S - ch) // 2
    r = max(1, int(S * 0.08))           # 圆角半径

    # 芯片与磁条的位置
    chip_x = m + int(cw * 0.12)
    chip_y = cy0 + int(ch * 0.44)
    chip_w = max(1, int(cw * 0.20))
    chip_h = max(1, int(ch * 0.26))

    for y in range(S):
        for x in range(S):
            i = (y * S + x) * 4
            a = rounded_rect_alpha(x - m, y - cy0, cw, ch, r, S)
            if a == 0:
                continue
            # 卡面渐变：竖向由深蓝到浅蓝
            t = (y - cy0) / max(1, ch)
            t = min(1.0, max(0.0, t))
            if t < 0.5:
                col = lerp(C_DARK, C_MAIN, t * 2)
            else:
                col = lerp(C_MAIN, C_LIGHT, (t - 0.5) * 2)

            # 顶部高光带
            if cy0 + ch * 0.16 <= y <= cy0 + ch * 0.30:
                col = lerp(col, C_WHITE, 0.14)

            # 金色芯片
            if (chip_x <= x < chip_x + chip_w and
                    chip_y <= y < chip_y + chip_h):
                col = C_GOLD
                # 芯片上的分格线
                if (x - chip_x) % max(2, chip_w // 3) < max(1, chip_w // 12):
                    col = lerp(C_GOLD, (180, 130, 10), 0.55)
                if (y - chip_y) % max(2, chip_h // 2) < max(1, chip_h // 10):
                    col = lerp(C_GOLD, (180, 130, 10), 0.55)

            # 右侧两条白色文字条
            bar_x = m + int(cw * 0.44)
            bar_w = int(cw * 0.42)
            for k, (by, bh) in enumerate([(0.36, 0.09), (0.54, 0.07)]):
                yy = cy0 + int(ch * by)
                hh = max(1, int(ch * bh))
                if bar_x <= x < bar_x + bar_w and yy <= y < yy + hh:
                    col = lerp(col, C_WHITE, 0.85 if k == 0 else 0.55)

            px[i] = col[0]
            px[i + 1] = col[1]
            px[i + 2] = col[2]
            px[i + 3] = a
    return px


def write_png(path, size, pixels):
    """把 RGBA 像素写成 PNG（无过滤，zlib 压缩）"""
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = bytearray()
    stride = size * 4
    for y in range(size):
        raw.append(0)                                   # filter type 0
        raw += pixels[y * stride:(y + 1) * stride]

    ihdr = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
           chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)
    return len(png)


def make_ico(path, sizes):
    """把多个尺寸的 PNG 封装成 ICO（PNG 压缩的 ICO，Vista+ 支持）"""
    imgs = []
    for s in sizes:
        png_path = path + '.%d.png' % s
        write_png(png_path, s, make_card(s))
        with open(png_path, 'rb') as f:
            imgs.append((s, f.read()))
        os.remove(png_path)

    header = struct.pack('<HHH', 0, 1, len(imgs))
    offset = 6 + 16 * len(imgs)
    entries, blobs = b'', b''
    for s, data in imgs:
        w = 0 if s >= 256 else s
        h = 0 if s >= 256 else s
        entries += struct.pack('<BBBBHHII', w, h, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    with open(path, 'wb') as f:
        f.write(header + entries + blobs)
    return os.path.getsize(path)


def main():
    ico_path = os.path.join(ROOT, '校园一卡通.ico')
    n = make_ico(ico_path, [16, 24, 32, 48, 64, 128, 256])
    print('已生成 %s（%d 字节，含 7 种尺寸）' % (os.path.basename(ico_path), n))

    # 前端图标（PWA）
    icons = os.path.join(ROOT, 'web', 'icons')
    os.makedirs(icons, exist_ok=True)
    for name, size in [('icon-192.png', 192), ('icon-512.png', 512),
                       ('maskable-512.png', 512),
                       ('apple-touch-icon.png', 180)]:
        p = os.path.join(icons, name)
        sz = write_png(p, size, make_card(size))
        print('  %-24s %4d x %4d  %6d 字节' % (name, size, size, sz))


if __name__ == '__main__':
    main()
