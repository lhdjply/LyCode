#!/usr/bin/env python3
"""用 astyle 按 scripts/code-format.cfg 格式化本仓库的 C/C++ 源码。

刻意**不用相对路径**。这个脚本可以在任何工作目录下被调用（编辑器任务、
pre-commit、CI 都各有各的 cwd），而 astyle 的 --recursive 是相对**当前工作目录**
解析的：

  * 旧写法 `--options=code-format.cfg --recursive "../*.cpp,*.c,*.h"` 隐含"cwd 必须
    是仓库根"，但 cfg 又只跟脚本同目录——两个前提互相矛盾，没有哪个 cwd 能同时
    满足。
  * 更糟的是从 scripts/ 里跑（脚本所在位置，也是最自然的选择）时，"../" 指向的是
    仓库的**上一级**：它会去格式化同级目录下的其它项目，而本仓库一行都没被碰到。
    实测那一跑会命中 icd-generate、sharescreen 等仓库，甚至 node_modules。

所以这里一律使用**绝对路径**：目标目录和 cfg 都由 __file__ 推出，与 cwd 无关。
"""

import os
import re
import subprocess
import sys
from pathlib import Path

# 需要格式化的源码树（相对于仓库根）。只列真实源码目录：
# 加进 build/、node_modules 这类目录只会白白耗费时间。
SOURCE_DIRS = ("src", "tests")

# 与 --recursive 配合的文件模式，逗号分隔的多扩展名由 astyle 自己展开。
FILE_PATTERNS = "*.cpp,*.c,*.h"

# astyle 输出里"动作列"与"文件名列"之间的分隔（至少两个空格）。
_COLUMN_GAP = re.compile(r"\s{2,}(.+)$")


def main() -> int:
  script_dir = Path(__file__).resolve().parent
  root = script_dir.parent
  config = script_dir / "code-format.cfg"

  if not config.is_file():
    print(f"找不到 astyle 配置：{config}", file=sys.stderr)
    return 2

  targets = []
  for name in SOURCE_DIRS:
    directory = root / name
    if not directory.is_dir():
      print(f"跳过不存在的目录：{directory}", file=sys.stderr)
      continue
    # 用绝对路径 + 正斜杠：反斜杠在 Windows 上会与 astyle 的转义/引号规则打架
    # （路径以反斜杠结尾时尤其危险），正斜杠两端都能吃。
    targets.append(f"{directory.as_posix()}/{FILE_PATTERNS}")

  if not targets:
    print("没有任何可格式化的源码目录。", file=sys.stderr)
    return 1

  # astyle 找不到时给出可操作的提示，而不是让它抛 FileNotFoundError。
  executable = os.environ.get("ASTYLE", "astyle")
  try:
    subprocess.run([executable, "--version"], capture_output=True, check=True)
  except (OSError, subprocess.CalledProcessError):
    print(
      f"找不到可用的 astyle（尝试过 {executable!r}）。\n"
      "请安装 Artistic Style（https://astyle.sourceforge.net/），"
      "或用 ASTYLE 环境变量指定可执行文件路径。",
      file=sys.stderr,
    )
    return 127

  command = [
    executable,
    f"--options={config.as_posix()}",
    "--recursive",
    *targets,
  ]
  # cwd 固定为仓库根：目标已是绝对路径，这里只是为了异常输出里的相对路径好读。
  completed = subprocess.run(command, cwd=root, text=True, capture_output=True)
  sys.stdout.write(completed.stdout)
  sys.stderr.write(completed.stderr)

  if completed.returncode != 0:
    print(f"\nastyle 以退出码 {completed.returncode} 结束。", file=sys.stderr)
    return completed.returncode

  changed = _collect_changed(completed.stdout)
  if changed:
    print(f"\n已格式化 {len(changed)} 个文件：")
    for path in changed:
      print(f"  {path}")
  else:
    print("\n所有文件都已符合格式，无需改动。")
  return 0


# astyle --formatted 的输出形态（每处理一个目录一段）：
#
#   ------------------------------------------------------------
#   <表头词>  /abs/path/src/*.cpp,*.c,*.h     ← 表头词随 locale 变
#   ------------------------------------------------------------
#   <动作词>  foo.cpp                          ← 裸文件名，不带目录
#
# 两个坑：
#   1. 被改写的文件只打印**裸文件名**，得靠上面的表头行还原目录；按"行里是否含 /"
#      来猜是错的（第一版就这么错的，于是永远统计出 0 个改动，把"静默就地改写"
#      伪装成"无需改动"）。
#   2. 表头词与动作词都随 locale 变（zh 是"目录"/"格式化"，C 是 "Directory"/
#      "Formatted"）。所以两者都**不按词匹配**：表头按 glob 结构识别，动作列按
#      列宽裁掉。硬编码任一个词都会在另一种 locale 下静默失灵。
def _collect_changed(output: str) -> list[str]:
  changed: list[str] = []
  current_dir = ""
  for raw in output.splitlines():
    line = raw.rstrip()
    if not line.strip():
      continue
    # 表头行：目录 + glob。按结构识别，不按表头词——见上方说明。
    if "*.cpp" in line:
      current_dir = line.split(None, 1)[1].split("*")[0].rstrip("/")
      continue
    if set(line.strip()) <= {"-"}:
      continue
    # 裁掉动作列：第一个"两个以上连续空格"之后就是文件名。
    match = _COLUMN_GAP.search(line)
    name = match.group(1) if match else line.strip()
    if name:
      changed.append(f"{current_dir}/{name}" if current_dir else name)
  return changed


if __name__ == "__main__":
  sys.exit(main())
