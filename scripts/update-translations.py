#!/usr/bin/env python3
"""用 Qt 的 lupdate 从源码抽出可翻译文案，刷新 translations/*.ts。

为什么需要这个脚本：Linux 上 /usr/bin/lupdate 往往是 qtchooser 的符号链接，
没有配置 Qt 环境时会直接报 "could not find a Qt installation of ''"，
而真正的二进制在 /usr/lib/qt6/bin/lupdate（或 Qt 安装目录的 bin 下）。
这里按已知位置依次探测，避免每个开发者自己猜路径。

用法：
  python3 scripts/update-translations.py          # 刷新全部 .ts
  python3 scripts/update-translations.py --check  # 只检查是否需要刷新（CI 用）
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
TS_DIR = ROOT / "translations"

# 候选顺序：PATH 上的 lupdate（配置正确时最好）→ Qt6 常见安装位置。
CANDIDATES = [
    "lupdate",
    "/usr/lib/qt6/bin/lupdate",
    "/usr/lib/x86_64-linux-gnu/qt6/bin/lupdate",
    "/usr/local/opt/qt6/bin/lupdate",
]


def is_working_lupdate(candidate):
    """这个 lupdate 真的能用吗？

    ⚠ 必须**实测**：/usr/bin/lupdate 常常是 qtchooser 的符号链接，没有配置 Qt 环境
    时它会直接失败（"could not find a Qt installation of ''"），而 shutil.which 照样
    找得到它。只按"路径存在"挑选的话，永远会选中这个壳。
    """
    try:
        result = subprocess.run([candidate, "-version"], capture_output=True, text=True,
                                timeout=20)
    except (OSError, subprocess.SubprocessError):
        return False
    return result.returncode == 0


def find_lupdate():
    for candidate in CANDIDATES:
        path = candidate if os.sep in candidate else shutil.which(candidate)
        if path and is_working_lupdate(path):
            return path
    return None


def main():
    lupdate = find_lupdate()
    if lupdate is None:
        print("找不到 lupdate，请安装 Qt6 Linguist 工具", file=sys.stderr)
        return 1

    ts_files = sorted(TS_DIR.glob("*.ts"))
    if not ts_files:
        print(f"{TS_DIR} 下没有 .ts 文件", file=sys.stderr)
        return 1

    # -no-obsolete：已从源码删掉的文案立刻从 .ts 移除，否则文件只会越积越大。
    command = [lupdate, str(SRC), "-no-obsolete",
               "-ts", *[str(path) for path in ts_files]]
    result = subprocess.run(command, cwd=ROOT)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
