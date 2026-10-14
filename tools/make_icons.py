#!/usr/bin/env python3
"""生成 PWA 图标（纯标准库，不依赖 Pillow）。

用法：python tools/make_icons.py
输出：web/icons/icon-192.png、icon-512.png、maskable-512.png、apple-touch-icon.png

图标内容与站内 favicon 一致：蓝色圆角方块 + 白色卡片 + 蓝色磁条。
"""
import io
import os
import struct
import zlib

# 配色（与 web/css/app.css 的 --brand / --brand-dark 保持一致）
BRAND = (37, 99, 235)        # #2563eb 主蓝
BRAND_D = (29, 78, 216)      # #1d4ed8 深蓝（磁条）
WHITE = (255, 255, 255)

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       '..', 'web', 'icons')


def write_png(path, width, height, pixels):
    """把 RGB 像素写成 PNG。pixels 是逐行的 bytes 列表。"""
    raw = b''.join(b'\x00' + row for row in pixels)   # 每行前置 filter 字节 0

    def chunk(tag, data):
        c = tag + data
        return (struct.pack('>I', len(data)) + c +
                struct.pack('>I', zlib.crc32(c) & 0xffffffff))

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)


def rounded_rect_covers(px, py, x0, y0, x1, y1, r):
    """判断像素中心是否落在圆角矩形内（用于画圆角与挖空）。"""
    cx = min(max(px, x0 + r), x1 - r)      # 最近的内缩矩形边界
    cy = min(max(py, y0 + r), y1 - r)
    if x0 <= px <= x1 and y0 <= py <= y1:
        dx, dy = px - cx, py - cy
        if dx == 0 and dy == 0:
            return True
        return dx * dx + dy * dy <= r * r
    return False


def render(size, maskable=False):
    """渲染一张图标，返回逐行 bytes 列表。

    maskable=True 时把图形缩小到中间 60%，留出安全边距，
    以适配 Android 自适应图标的各种裁切形状。
    """
    # 背景圆角方块：maskable 时铺满整块（由系统裁切），普通图标留 6% 边距
    bg_inset = 0.0 if maskable else 0.03
    bg_r = 0.0 if maskable else 0.22

    # 卡片主体：maskable 时收缩到中间，普通图标占比更大
    scale = 0.60 if maskable else 0.62
    card_w = size * scale
    card_h = card_w * 0.62
    card_x0 = (size - card_w) / 2
    card_y0 = (size - card_h) / 2
    card_r = card_h * 0.16

    # 磁条：卡片上方 40% 高度处，宽 45%，高 12%
    strip_w = card_w * 0.45
    strip_h = card_h * 0.13
    strip_x0 = (size - strip_w) / 2
    strip_y0 = card_y0 + card_h * 0.42

    rows = []
    for y in range(size):
        row = bytearray()
        py = y + 0.5
        for x in range(size):
            px = x + 0.5
            r, g, b = BRAND
            # 背景
            if not rounded_rect_covers(px, py, size * bg_inset, size * bg_inset,
                                       size * (1 - bg_inset), size * (1 - bg_inset),
                                       size * bg_r):
                r = g = b = 0            # 圆角外透明区用黑（PNG 无 alpha，桌面显示无碍）
            # 卡片
            if rounded_rect_covers(px, py, card_x0, card_y0,
                                   card_x0 + card_w, card_y0 + card_h, card_r):
                r, g, b = WHITE
                # 磁条
                if rounded_rect_covers(px, py, strip_x0, strip_y0,
                                       strip_x0 + strip_w, strip_y0 + strip_h,
                                       strip_h / 2):
                    r, g, b = BRAND_D
            row += bytes((r, g, b))
        rows.append(bytes(row))
    return rows


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    jobs = [('icon-192.png', 192, False),
            ('icon-512.png', 512, False),
            ('maskable-512.png', 512, True),
            ('apple-touch-icon.png', 180, False)]
    for name, size, maskable in jobs:
        path = os.path.normpath(os.path.join(OUT_DIR, name))
        write_png(path, size, size, render(size, maskable))
        print(f'  {name:<22} {size}x{size}  {os.path.getsize(path):>6} 字节')
    print('图标生成完成 ->', os.path.normpath(OUT_DIR))


if __name__ == '__main__':
    main()
