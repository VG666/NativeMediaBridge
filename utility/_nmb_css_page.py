# -*- coding: utf-8 -*-
"""把 hook/css/controls.html（独立编辑页）里的内联样式写回项目真源，或反向同步。

用法（在项目根目录）：
  python utility/_nmb_css_page.py                 # html -> hook/css/*.css（默认）
  python utility/_nmb_css_page.py --dry           # 只打印会改哪些行，不落盘
  python utility/_nmb_css_page.py --gen           # 写回后再跑生成器（hook/inc/*.inc）
  python utility/_nmb_css_page.py --build         # 写回后生成 + 编译 DLL
  python utility/_nmb_css_page.py --to-html       # hook/css/*.css -> html（反向同步）

约定（两侧都必须遵守）：
  * 页面里 <style id="nmb-css"> 内，每份 css 的内容被这两行包着，名字用文件名：
        /* ===== NMB-EDIT-BEGIN base.css ===== */
        ...内容...
        /* ===== NMB-EDIT-END base.css ===== */
  * 标记行本身不参与写回；只在 BEGIN/END 之间取内容。
  * 项目里的 css 文件是 CRLF；页面是 LF。写回时统一转 CRLF，比较时统一转 LF。

没有第三方依赖，只用标准库。
"""

import difflib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HTML_PATH = ROOT / "hook" / "css" / "controls.html"
CSS_DIR = ROOT / "hook" / "css"

# 拼接顺序与 utility/_nmb_gen_hooks_inc.py 的 CSS_ORDER 保持一致（base->tier->audio）。
# 同特异度的规则谁赢由这个顺序定，所以这里也不按字母序。
CSS_ORDER = ["base.css", "tier.css", "audio.css"]

MARK_BEGIN = "/* ===== NMB-EDIT-BEGIN %s ===== */"
MARK_END = "/* ===== NMB-EDIT-END %s ===== */"


def norm(text):
    """统一成 LF，并把首尾的空行收干净（写回/比较都用这个形态）。"""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(lines)


def read_text(path):
    return Path(path).read_bytes().decode("utf-8-sig")


def write_crlf(path, text):
    data = (norm(text) + "\n").replace("\n", "\r\n").encode("utf-8")
    Path(path).write_bytes(data)


def extract(html, name):
    begin = re.escape(MARK_BEGIN % name)
    end = re.escape(MARK_END % name)
    m = re.search(begin + r"(.*?)" + end, html, re.S)
    if not m:
        raise SystemExit(
            "错误：%s 里找不到 %s 的标记块。\n"
            "      标记行必须原样保留（含前后各一个空格）：\n"
            "        %s\n        %s"
            % (HTML_PATH.name, name, MARK_BEGIN % name, MARK_END % name)
        )
    return norm(m.group(1))


def replace_block(html, name, body):
    begin = re.escape(MARK_BEGIN % name)
    end = re.escape(MARK_END % name)
    pattern = re.compile(begin + r".*?" + end, re.S)
    if not pattern.search(html):
        raise SystemExit("错误：%s 里找不到 %s 的标记块。" % (HTML_PATH.name, name))
    repl = (MARK_BEGIN % name) + "\n" + norm(body) + "\n" + MARK_END % name
    return pattern.sub(lambda _m: repl, html, count=1)


def brace_check(name, body):
    """粗查括号是否配平（只数裸括号，够抓"删多了半边"这种事故）。"""
    depth = 0
    ok = True
    for ch in body:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth < 0:
                ok = False
    return ok and depth == 0


def html_to_css(dry=False):
    if not HTML_PATH.exists():
        raise SystemExit("错误：找不到 %s" % HTML_PATH)
    html = read_text(HTML_PATH)
    changed = False
    for name in CSS_ORDER:
        body = extract(html, name)
        css_path = CSS_DIR / name
        old = norm(read_text(css_path)) if css_path.exists() else ""
        if not brace_check(name, body):
            raise SystemExit(
                "错误：%s 里那段样式的花括号不配平（{ 与 } 数量不等），先修好再写回。"
                % name
            )
        if body == old:
            print("  %-10s 无变化" % name)
            continue
        changed = True
        diff = list(difflib.unified_diff(
            old.split("\n"), body.split("\n"),
            fromfile="hook/css/%s（项目现有）" % name,
            tofile="hook/css/%s（页面里的）" % name,
            lineterm="", n=2))
        adds = sum(1 for d in diff if d.startswith("+") and not d.startswith("+++"))
        dels = sum(1 for d in diff if d.startswith("-") and not d.startswith("---"))
        print("  %-10s 有改动  +%d / -%d 行" % (name, adds, dels))
        if dry:
            print("\n".join("      " + d for d in diff))
        else:
            write_crlf(css_path, body)
    if dry:
        print("\n（--dry：什么都没写）")
    elif changed:
        print("\n已写回 hook/css/。下一步：python utility/_nmb_gen_hooks_inc.py 然后编译")
    return changed


def css_to_html():
    if not HTML_PATH.exists():
        raise SystemExit("错误：找不到 %s" % HTML_PATH)
    html = read_text(HTML_PATH)
    for name in CSS_ORDER:
        body = read_text(CSS_DIR / name)
        html = replace_block(html, name, body)
        print("  %-10s 已写入页面" % name)
    HTML_PATH.write_bytes(html.encode("utf-8"))
    print("\n已更新 %s" % HTML_PATH.name)


def run_gen():
    gen = ROOT / "utility" / "_nmb_gen_hooks_inc.py"
    print("\n[gen] %s" % gen.name)
    r = subprocess.run([sys.executable, str(gen)], cwd=str(ROOT))
    if r.returncode != 0:
        raise SystemExit("[gen] 失败（退出码 %d）" % r.returncode)


def run_build():
    bat = ROOT / "build-zig.bat"
    print("\n[build] %s" % bat.name)
    r = subprocess.run(["cmd", "/c", bat.name], cwd=str(ROOT))
    if r.returncode != 0:
        raise SystemExit("[build] 失败（退出码 %d）" % r.returncode)


def main(argv):
    args = argv[1:]
    dry = "--dry" in args
    to_html = "--to-html" in args
    gen = "--gen" in args or "--build" in args
    build = "--build" in args

    if to_html:
        print("同步 hook/css/*.css -> %s" % HTML_PATH.name)
        css_to_html()
        return 0

    print("同步 %s -> hook/css/*.css%s" % (HTML_PATH.name, "（dry run）" if dry else ""))
    changed = html_to_css(dry=dry)
    if changed and not dry:
        if gen:
            run_gen()
        if build:
            run_build()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

