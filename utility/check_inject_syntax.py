"""检查四个页面注入脚本 hook_*.js 的 JS 语法（node --check），并核对生成的
hook_*.inc 是否与 js 一致（防止改了 js 却忘了重新生成 inc）。

注入脚本语法一旦出错，页面里所有媒体钩子都会静默失效（播放控件条、进度条拖动都不可用），
所以改过 hook_*.js 后跑一次这个自检很有必要。
"""
import re
import subprocess
import sys
from pathlib import Path

# 本脚本在项目根的 utility\ 目录：js 真源在上一级 hook\js，生成的 inc 在 hook\inc。
PROJECT_DIR = Path(__file__).resolve().parent.parent
JS_DIR = PROJECT_DIR / "hook" / "js"
INC_DIR = PROJECT_DIR / "hook" / "inc"
HOOKS = [
    "injection.js",
    "media_api_shim.js",
    "compat_shim.js",
    "crypto_shim.js",
]
INC_PATTERN = re.compile(r'R"NMBHOOK\((.*?)\)NMBHOOK"', re.S)


def check_syntax(js_path: Path) -> int:
    result = subprocess.run(
        ["node", "--check", str(js_path)],
        capture_output=True,
        text=True,
        # node 的报错里会带注入脚本的原文（多为 UTF-8 中文），按系统默认编码读会抛
        # UnicodeDecodeError 并把真正的语法错误吞掉——只剩"校验失败"四个字，白查一轮。
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        print(f"{js_path.name} 语法校验失败：")
        print(result.stderr or result.stdout)
        return result.returncode
    size = js_path.stat().st_size
    print(f"{js_path.name} 语法校验通过（{size} 字节）")
    return 0


def check_inc(js_path: Path) -> bool:
    inc_path = INC_DIR / js_path.with_suffix(".inc").name
    if not inc_path.exists():
        print(f"  提示：{inc_path} 不存在，编译前先跑 utility\\_nmb_gen_hooks_inc.py")
        return True
    js_text = js_path.read_text(encoding="utf-8", newline="")
    inc_text = inc_path.read_text(encoding="utf-8", newline="")
    joined = "".join(INC_PATTERN.findall(inc_text))
    if joined != js_text:
        print(f"  {inc_path.name} 与 {js_path.name} 不一致：js 改过但 inc 没重新生成"
              f"（跑 _nmb_gen_hooks_inc.py）")
        return False
    return True


def main():
    failed = False
    for name in HOOKS:
        js_path = JS_DIR / name
        if not js_path.exists():
            print(f"找不到 {js_path}")
            return 1
        if check_syntax(js_path) != 0:
            failed = True
            continue
        if not check_inc(js_path):
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
