# -*- coding: utf-8 -*-
"""扫描/清理 PNG 的 iCCP 块。

libpng 在解码带 iCCP 块、且块内容与标准 sRGB profile 不一致的 PNG 时会打印：
    libpng warning: iCCP: known incorrect sRGB profile
这个块只是颜色配置元数据，删掉不影响像素数据，警告也就不会再出现。

用法：
    python tools\\png_iccp_check.py              # 只扫描并列出
    python tools\\png_iccp_check.py --fix        # 就地删掉这些 PNG 的 iCCP 块
    python tools\\png_iccp_check.py --fix 目录    # 只处理指定目录
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

SKIP_DIRS = {".git", "node_modules", "__pycache__", "build", "dist", "out", "bin", "obj"}


def find_pngs(root):
    for path in root.rglob("*.png"):
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        yield path


def read_iccp(path):
    """返回 iCCP 的 profile 名（没有则返回 None）；文件损坏时抛异常。"""
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return None
    offset = 8
    while offset + 8 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        tag = data[offset + 4:offset + 8]
        if tag == b"iCCP":
            body = data[offset + 8:offset + 8 + length]
            name, _, _ = body.partition(b"\x00")
            return name.decode("latin-1", "replace")
        if tag == b"IEND":
            return None
        offset += 12 + length
    return None


def strip_iccp(path):
    """删除 iCCP 块，保持其它块原样（字节级删除，不动 IDAT 数据）。"""
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return False
    output = bytearray(data[:8])
    offset = 8
    removed = False
    while offset + 8 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        tag = data[offset + 4:offset + 8]
        end = offset + 12 + length
        if tag == b"iCCP":
            removed = True
        else:
            output += data[offset:end]
        offset = end
    if removed:
        path.write_bytes(bytes(output))
    return removed


def main():
    parser = argparse.ArgumentParser(description="扫描或清理 PNG 的 iCCP 块")
    parser.add_argument("root", nargs="?", default=".", help="要处理的目录，默认当前目录")
    parser.add_argument("--fix", action="store_true", help="就地删除 iCCP 块")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    print(f"扫描目录：{root}")
    found = []
    broken = []
    for png in find_pngs(root):
        try:
            name = read_iccp(png)
        except Exception as error:  # 坏文件不该中断扫描
            broken.append((png, str(error)))
            continue
        if name is not None:
            found.append((png, name))

    print(f"\n带 iCCP 块的 PNG：{len(found)} 个")
    for png, name in found:
        action = ""
        if args.fix:
            action = "，已删除 iCCP" if strip_iccp(png) else "，删除失败"
        print(f"  {png.relative_to(root)}  (profile={name!r}){action}")
    if broken:
        print(f"\n无法解析的 PNG：{len(broken)} 个")
        for png, error in broken[:10]:
            print(f"  {png.relative_to(root)}  {error}")
    if found and not args.fix:
        print("\n加 --fix 可删除这些 iCCP 块（只删元数据，像素不变），警告即消失。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
