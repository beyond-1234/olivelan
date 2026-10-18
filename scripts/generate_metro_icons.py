#!/usr/bin/env python3
"""
Microsoft Fluent 1.5px 单线矢量单色图标生成器 (构建期自动调用)
"""

import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("[!] 提示: PIL 未安装，跳过图标动态重新生成，使用现有缓存")
    sys.exit(0)

# 解析目标目录 (支持参数传入或默认路径)
if len(sys.argv) > 2:
    DARK_PNG_DIR = sys.argv[1]
    PNG_DIR = sys.argv[2]
else:
    SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
    OLIVE_DIR = os.path.dirname(SCRIPT_DIR)
    DARK_PNG_DIR = os.path.join(OLIVE_DIR, "app/ui/style/olive-dark/png")
    PNG_DIR = os.path.join(OLIVE_DIR, "app/ui/style/olive-metro/png")

os.makedirs(PNG_DIR, exist_ok=True)

def draw_metro_icon(name, size):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    scale = size / 64.0
    stroke = max(1, int(round(4 * scale)))
    color = (220, 220, 220, 255)

    def pt(x, y):
        return (int(round(x * scale)), int(round(y * scale)))

    if name == "play":
        draw.polygon([pt(20, 14), pt(50, 32), pt(20, 50)], outline=color, fill=None, width=stroke)
    elif name == "pause":
        draw.line([pt(22, 16), pt(22, 48)], fill=color, width=stroke*2)
        draw.line([pt(42, 16), pt(42, 48)], fill=color, width=stroke*2)
    elif name == "prev":
        draw.line([pt(16, 16), pt(16, 48)], fill=color, width=stroke)
        draw.polygon([pt(48, 16), pt(24, 32), pt(48, 48)], outline=color, fill=None, width=stroke)
    elif name == "next":
        draw.polygon([pt(16, 16), pt(40, 32), pt(16, 48)], outline=color, fill=None, width=stroke)
        draw.line([pt(48, 16), pt(48, 48)], fill=color, width=stroke)
    elif name == "ff":
        draw.polygon([pt(12, 16), pt(32, 32), pt(12, 48)], outline=color, fill=None, width=stroke)
        draw.polygon([pt(32, 16), pt(52, 32), pt(32, 48)], outline=color, fill=None, width=stroke)
    elif name == "rew":
        draw.polygon([pt(32, 16), pt(12, 32), pt(32, 48)], outline=color, fill=None, width=stroke)
        draw.polygon([pt(52, 16), pt(32, 32), pt(52, 48)], outline=color, fill=None, width=stroke)
    elif name == "razor":
        draw.rectangle([pt(14, 20), pt(50, 44)], outline=color, width=stroke)
        draw.line([pt(22, 32), pt(42, 32)], fill=color, width=stroke)
    elif name == "hand":
        draw.rectangle([pt(18, 22), pt(46, 48)], outline=color, width=stroke)
        draw.line([pt(26, 14), pt(26, 22)], fill=color, width=stroke)
        draw.line([pt(34, 12), pt(34, 22)], fill=color, width=stroke)
        draw.line([pt(42, 16), pt(42, 22)], fill=color, width=stroke)
    elif name == "magnet":
        draw.arc([pt(18, 14), pt(46, 42)], start=0, end=180, fill=color, width=stroke)
        draw.line([pt(18, 28), pt(18, 46)], fill=color, width=stroke)
        draw.line([pt(46, 28), pt(46, 46)], fill=color, width=stroke)
    elif name == "new":
        draw.rectangle([pt(16, 12), pt(48, 52)], outline=color, width=stroke)
        draw.line([pt(24, 26), pt(40, 26)], fill=color, width=stroke)
        draw.line([pt(24, 36), pt(40, 36)], fill=color, width=stroke)
    elif name == "save":
        draw.rectangle([pt(14, 14), pt(50, 50)], outline=color, width=stroke)
        draw.rectangle([pt(22, 14), pt(42, 28)], outline=color, width=stroke)
        draw.rectangle([pt(20, 34), pt(44, 50)], outline=color, width=stroke)
    elif name == "undo":
        draw.arc([pt(16, 20), pt(48, 52)], start=180, end=340, fill=color, width=stroke)
        draw.line([pt(16, 26), pt(26, 18)], fill=color, width=stroke)
        draw.line([pt(16, 26), pt(26, 34)], fill=color, width=stroke)
    elif name == "redo":
        draw.arc([pt(16, 20), pt(48, 52)], start=200, end=360, fill=color, width=stroke)
        draw.line([pt(48, 26), pt(38, 18)], fill=color, width=stroke)
        draw.line([pt(48, 26), pt(38, 34)], fill=color, width=stroke)
    elif name == "eye-opened":
        draw.arc([pt(12, 16), pt(52, 48)], start=0, end=180, fill=color, width=stroke)
        draw.arc([pt(12, 16), pt(52, 48)], start=180, end=360, fill=color, width=stroke)
        draw.ellipse([pt(28, 28), pt(36, 36)], outline=color, width=stroke)
    elif name == "eye-closed":
        draw.arc([pt(12, 16), pt(52, 48)], start=0, end=180, fill=color, width=stroke)
        draw.line([pt(14, 46), pt(50, 18)], fill=color, width=stroke)
    elif name == "lock-closed":
        draw.rectangle([pt(18, 26), pt(46, 50)], outline=color, width=stroke)
        draw.arc([pt(24, 14), pt(40, 32)], start=180, end=360, fill=color, width=stroke)
    elif name == "lock-opened":
        draw.rectangle([pt(18, 26), pt(46, 50)], outline=color, width=stroke)
        draw.arc([pt(24, 10), pt(40, 28)], start=180, end=360, fill=color, width=stroke)
        draw.line([pt(40, 19), pt(40, 26)], fill=color, width=stroke)
    elif name == "zoomin":
        draw.ellipse([pt(14, 14), pt(42, 42)], outline=color, width=stroke)
        draw.line([pt(35, 35), pt(50, 50)], fill=color, width=stroke)
        draw.line([pt(28, 22), pt(28, 34)], fill=color, width=stroke)
        draw.line([pt(22, 28), pt(34, 28)], fill=color, width=stroke)
    elif name == "zoomout":
        draw.ellipse([pt(14, 14), pt(42, 42)], outline=color, width=stroke)
        draw.line([pt(35, 35), pt(50, 50)], fill=color, width=stroke)
        draw.line([pt(22, 28), pt(34, 28)], fill=color, width=stroke)
    elif name in ("folder", "open"):
        draw.polygon([pt(14, 18), pt(28, 18), pt(34, 24), pt(50, 24), pt(50, 48), pt(14, 48)], outline=color, fill=None, width=stroke)
    elif name in ("plus", "add-button"):
        draw.line([pt(32, 16), pt(32, 48)], fill=color, width=stroke)
        draw.line([pt(16, 32), pt(48, 32)], fill=color, width=stroke)
    elif name == "minus":
        draw.line([pt(16, 32), pt(48, 32)], fill=color, width=stroke)
    elif name == "clock":
        draw.ellipse([pt(14, 14), pt(50, 50)], outline=color, width=stroke)
        draw.line([pt(32, 22), pt(32, 32)], fill=color, width=stroke)
        draw.line([pt(32, 32), pt(42, 32)], fill=color, width=stroke)
    elif name == "diamond":
        draw.polygon([pt(32, 14), pt(50, 32), pt(32, 50), pt(14, 32)], outline=color, fill=None, width=stroke)
    else:
        draw.rectangle([pt(16, 16), pt(48, 48)], outline=color, width=stroke)

    disabled_img = img.copy()
    alpha = disabled_img.split()[3]
    alpha = alpha.point(lambda p: int(p * 0.25))
    disabled_img.putalpha(alpha)

    return img, disabled_img

if __name__ == "__main__":
    sizes = [16, 32, 64, 128]
    all_dark_pngs = os.listdir(DARK_PNG_DIR) if os.path.exists(DARK_PNG_DIR) else []
    icon_names = set()
    for f in all_dark_pngs:
        parts = f.split(".")
        if len(parts) >= 3 and parts[-1] == "png":
            icon_names.add(parts[0])

    for name in icon_names:
        for s in sizes:
            out_norm = os.path.join(PNG_DIR, f"{name}.{s}.png")
            out_dis = os.path.join(PNG_DIR, f"{name}.{s}.disabled.png")
            if not os.path.exists(out_norm) or not os.path.exists(out_dis):
                normal, disabled = draw_metro_icon(name, s)
                normal.save(out_norm)
                disabled.save(out_dis)

    print(f"[CMake Build Hook] Verified/Generated Fluent icons in {PNG_DIR}")
