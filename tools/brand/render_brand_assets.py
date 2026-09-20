#!/usr/bin/env python3
"""apitab 品牌资产生成：水母徽标 SVG + 随包光栅图（托盘 PNG / 应用图标 PNG+ICO）。

造型（参考品牌设计板，可爱向）：圆润钟形伞体（兼作 apiTAB 的"标签"轮廓）+ 两只
圆眼睛与微笑（用 evenodd 在伞体上镂空，任意底色都成立）+ 三条垂落触手 + 右上角
四角星。伞体/眼睛/触手用绝对坐标命令书写，按"墨迹高度"线性缩放，因此 24 网格、
256 应用图标、32 托盘三套尺寸完全同形。

用法（在仓库根目录）：
    python3 tools/brand/render_brand_assets.py            # 生成 SVG + 光栅图
    python3 tools/brand/render_brand_assets.py --svg-only # 只重写 SVG

依赖：Python 3 + Pillow（光栅化经无头 google-chrome 渲染 4x 母版后降采样）；
ICO 需要 ImageMagick（magick/convert）——缺失时跳过并保留已有 .ico。
产物：
    resources/images/apitab_mark.svg          界面内单色徽标（currentColor + Tint）
    resources/images/apitab_app_icon_dark.svg 256 深色圆角底板图标
    resources/images/apitab_app_icon_light.svg 256 浅色圆角底板图标
    resources/images/apitab_tray.svg          32 托盘图标（透明底单色）
    resources/images/tray.png / @2x / @3x     托盘光栅（32/64/96，随包）
    assets/icon.png / assets/icon.ico         Linux/Windows 打包图标
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STROKE = 1.9  # 24 网格下的触手线宽

BELL = [("M", 4.2, 15.1), ("V", 10.9),
        ("C", [(4.2, 7.0), (7.7, 4.0), (12.0, 4.0)]),
        ("C", [(16.3, 4.0), (19.8, 7.0), (19.8, 10.9)]),
        ("V", 15.1), ("C", [(19.8, 16.0), (19.1, 16.7), (18.2, 16.7)]),
        ("H", 5.8), ("C", [(4.9, 16.7), (4.2, 16.0), (4.2, 15.1)]), ("Z",)]
SMILE = [("M", 9.7, 12.7), ("C", [(10.6, 14.1), (13.4, 14.1), (14.3, 12.7)]),
         ("C", [(13.4, 13.3), (10.6, 13.3), (9.7, 12.7)]), ("Z",)]
TENTACLES = [
    [("M", 8.4, 17.1), ("C", [(8.2, 18.4), (7.5, 19.0), (7.5, 20.3)])],
    [("M", 12.0, 17.1), ("L", 12.0, 20.7)],
    [("M", 15.6, 17.1), ("C", [(15.8, 18.4), (16.5, 19.0), (16.5, 20.3)])],
]
SPARKLE = [("M", 20.6, 2.9), ("L", 21.06, 4.34), ("L", 22.5, 4.8), ("L", 21.06, 5.26),
           ("L", 20.6, 6.7), ("L", 20.14, 5.26), ("L", 18.7, 4.8), ("L", 20.14, 4.34), ("Z",)]
# 24 网格下的整幅墨迹范围（含描边半宽）：左缘、右缘（星尖）、上缘（星顶）、下缘（中触手末端）
INK = (4.2, 22.8, 2.9, 21.6)


def eyes():
    """两只圆眼睛（evenodd 镂空）：用 a 圆弧命令，缩放时半径同步缩放。"""
    out = []
    for cx in (9.3, 14.7):
        out.append([("M", cx - 1.35, 10.4),
                    ("a", 1.35, 1.35, 0.0, 1, 0, 2.7, 0.0),
                    ("a", 1.35, 1.35, 0.0, 1, 0, -2.7, 0.0), ("Z",)])
    return out


def emit(cmds, k, dx, dy):
    """按 k 缩放 + (dx,dy) 平移输出路径（只支持绝对命令 M/L/C/V/H/Z）。"""
    out = []
    for cmd in cmds:
        op = cmd[0]
        if op in "ML":
            out.append(f"{op}{cmd[1] * k + dx:.2f} {cmd[2] * k + dy:.2f}")
        elif op == "C":
            out.append("C" + " ".join(f"{x * k + dx:.2f} {y * k + dy:.2f}" for x, y in cmd[1]))
        elif op in "VH":
            out.append(f"{op}{cmd[1] * k + (dy if op == 'V' else dx):.2f}")
        elif op == "a":  # a rx ry rot large sweep dx dy：半径与位移都按 k 缩放，标志位不变
            _, rx, ry, rot, large, sweep, ax, ay = cmd
            out.append(f"a{rx * k:.2f} {ry * k:.2f} {rot:g} {large} {sweep} "
                       f"{ax * k:.2f} {ay * k:.2f}")
        elif op == "Z":
            out.append("Z")
    return " ".join(out)


def fitted(ink_height):
    """按目标墨迹高度求缩放与平移，并把墨迹中心对准画布中心。"""
    cx = (INK[0] + INK[1]) / 2
    cy = (INK[2] + INK[3]) / 2
    k = ink_height / (INK[3] - INK[2])
    return k, -cx * k, -cy * k


def jelly(k, dx, dy, ink):
    # 伞体与五官合成一条 path：evenodd 让眼睛/微笑成为镂空，落在任何底色上都成立。
    body = [emit(BELL, k, dx, dy)]
    for hole in (eyes() + [SMILE]):
        body.append(emit(hole, k, dx, dy))
    parts = [f'  <path d="{" ".join(body)}" fill="{ink}" fill-rule="evenodd"/>']
    for tentacle in TENTACLES:
        parts.append(f'  <path d="{emit(tentacle, k, dx, dy)}" fill="none" stroke="{ink}" '
                     f'stroke-width="{STROKE * k:.2f}" stroke-linecap="round"/>')
    parts.append(f'  <path d="{emit(SPARKLE, k, dx, dy)}" fill="{ink}"/>')
    return "\n".join(parts)


def tile(size, bg, border, ink, comment):
    k, dx, dy = fitted(size * 0.60)
    dx += size / 2
    dy += size / 2
    inset = size * 0.0078
    return f'''<svg width="{size}" height="{size}" viewBox="0 0 {size} {size}" xmlns="http://www.w3.org/2000/svg">
  <!-- {comment} -->
  <rect width="{size}" height="{size}" rx="{size * 0.25:.0f}" fill="{bg}"/>
  <rect x="{inset:.1f}" y="{inset:.1f}" width="{size - inset * 2:.1f}" height="{size - inset * 2:.1f}" rx="{size * 0.242:.0f}" fill="none" stroke="{border}" stroke-width="{size * 0.0156:.0f}"/>
{jelly(k, dx, dy, ink)}
</svg>
'''


def write_svgs():
    (ROOT / "resources/images/apitab_mark.svg").write_text(
        f'''<svg width="24" height="24" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg">
  <!-- apitab 水母徽标：圆润伞体（兼作 apiTAB 的"标签"轮廓）+ 镂空圆眼与微笑 + 三条触手
       + 右上四角星，取自品牌设计板的可爱水母造型。单色 currentColor，由 Image::Tint
       适配深浅主题与岛屿底色。
       本文件与其余品牌资产由 tools/brand/render_brand_assets.py 生成，改造型请改脚本。 -->
{jelly(1.0, 0.0, 0.0, "currentColor")}
</svg>
''')
    (ROOT / "resources/images/apitab_app_icon_dark.svg").write_text(
        tile(256, "#07151B", "#21434B", "#43D3DC",
             "深色应用图标：深海蓝圆角底板 + 冰川青可爱水母徽标（与 apitab_mark.svg 同形，"
             "由 tools/brand/render_brand_assets.py 生成）。"))
    (ROOT / "resources/images/apitab_app_icon_light.svg").write_text(
        tile(256, "#F7FCFD", "#D9EAEC", "#28B8C7",
             "浅色应用图标：雾白圆角底板 + 冰川青可爱水母徽标（与 apitab_mark.svg 同形，"
             "由 tools/brand/render_brand_assets.py 生成）。"))
    k, dx, dy = fitted(25.0)
    (ROOT / "resources/images/apitab_tray.svg").write_text(
        f'''<svg width="32" height="32" viewBox="0 0 32 32" xmlns="http://www.w3.org/2000/svg">
  <!-- 托盘图标：透明底、单色冰川青可爱水母徽标，浅色/深色系统栏都清晰。
       由 tools/brand/render_brand_assets.py 生成。 -->
{jelly(k, dx + 16.0, dy + 16.0, "#28B8C7")}
</svg>
''')


def chrome_binary():
    for name in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser"):
        found = shutil.which(name)
        if found:
            return found
    return None


def render_master(chrome, svg: Path, px: int, out: Path, work: Path):
    html = work / f"{out.stem}.html"
    svg_text = svg.read_text()
    head, _, tail = svg_text.partition('width="')
    _, _, rest = tail.partition('"')
    _, _, tail2 = rest.partition('height="')
    _, _, rest2 = tail2.partition('"')
    html.write_text(
        '<!doctype html><html><head><meta charset="utf-8">'
        '<style>html,body{margin:0;padding:0;background:transparent}svg{display:block}</style>'
        f'</head><body>{head}width="{px}" height="{px}"{rest2}</body></html>')
    profile = work / f"profile-{px}"
    shutil.rmtree(profile, ignore_errors=True)
    subprocess.run(
        [chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
         "--force-device-scale-factor=1", "--default-background-color=00000000",
         f"--user-data-dir={profile}", f"--window-size={px},{px}",
         f"--screenshot={out}", str(html)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)


def downscale(master: Path, size: int, out: Path):
    from PIL import Image

    image = Image.open(master).convert("RGBA").resize((size, size), Image.LANCZOS)
    image.save(out)
    ink = sum(1 for pixel in image.getchannel("A").get_flattened_data() if pixel > 0)
    if ink == 0:
        raise SystemExit(f"{out} 光栅化为空，渲染失败")
    print(f"  {out.relative_to(ROOT)}: {size}x{size} ink={ink}px")


def write_rasters():
    chrome = chrome_binary()
    if chrome is None:
        print("未找到无头 Chrome，跳过光栅图（SVG 已更新）", file=sys.stderr)
        return
    from PIL import Image  # noqa: F401  （缺 Pillow 时给出明确报错）

    work = ROOT / "build/brand-raster"
    work.mkdir(parents=True, exist_ok=True)
    tray_master, icon_master = work / "tray-master.png", work / "icon-master.png"

    render_master(chrome, ROOT / "resources/images/apitab_tray.svg", 512, tray_master, work)
    for size, name in ((32, "tray.png"), (64, "tray@2x.png"), (96, "tray@3x.png")):
        downscale(tray_master, size, ROOT / "resources/images" / name)

    render_master(chrome, ROOT / "resources/images/apitab_app_icon_dark.svg", 1024, icon_master, work)
    icon_png = ROOT / "assets/icon.png"
    downscale(icon_master, 256, icon_png)

    magick = shutil.which("magick") or shutil.which("convert")
    if magick is None:
        print("未找到 ImageMagick，保留既有 assets/icon.ico", file=sys.stderr)
        return
    subprocess.run([magick, str(icon_png), "-define", "icon:auto-resize=256,128,64,48,32,24,20,16",
                    str(ROOT / "assets/icon.ico")], check=True)
    print(f"  assets/icon.ico 已更新")


def main():
    parser = argparse.ArgumentParser(description="生成 apitab 品牌资产")
    parser.add_argument("--svg-only", action="store_true", help="只写 SVG，不光栅化")
    args = parser.parse_args()
    write_svgs()
    print("SVG 已写入 resources/images/")
    if not args.svg_only:
        write_rasters()


if __name__ == "__main__":
    main()
