#!/usr/bin/env python3
"""守住"给模型看的文本不进翻译文件"这条边界。

界面文案与提示词用的是同一种语言（中文），把前者包进 tr() 时很容易顺手把后者也
包进去。而工具描述、系统提示词、参数说明是**发给模型的指令**：翻译它们会改变模型
行为，也拿不到任何本地化收益。这类错误在界面上完全看不出来——中文用户看不到任何
差别，只有英文用户会得到一套被翻过的工具契约。

所以这条检查放在 **.ts 文件**上，而不是源码上：.ts 是"哪些文案被标记为可翻译"的
权威产物，只有真的被 tr() 包住的字符串才会出现在里面。

用法：
  python3 scripts/check-translations.py          # 边界检查
  python3 scripts/check-translations.py --list   # 顺带列出全部已标记文案
"""
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TS_FILES = sorted((ROOT / "translations").glob("*.ts"))

# 给模型看的文案**自动**从源码里提取，而不是硬编码一份名单。
#
# 硬编码的名单会随"新增一个工具"而失效——新工具的描述照样可能被人顺手包进
# translate()，而名单里没有它，检查就通过了。按位置提取则天然跟着代码走。
SCHEMA_KEY = re.compile(r'QStringLiteral\("(?:description|enum)"\)\s*,?\s*$')
SCHEMA_PROP = re.compile(r'\b(?:string|number|bool|enum|array|object)Prop\(\s*$')
LITERAL = re.compile(r'QStringLiteral\(\s*("(?:[^"\\]|\\.)*")' + r'(?:\s*"(?:[^"\\]|\\.)*")*\s*\)')


def schema_descriptions(root):
    """扫源码，取出所有 JSON Schema 说明（发给模型的工具/参数描述）。"""
    found = set()
    for path in sorted((root / "src").rglob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        for match in LITERAL.finditer(text):
            source = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(0)))
            if not source.strip():
                continue
            before = text[max(0, match.start() - 220):match.start()]
            if SCHEMA_KEY.search(before) or SCHEMA_PROP.search(before):
                found.add(source)
    return found


def sources(ts_path):
    """产出 (context, source, translation) 三元组。"""
    tree = ET.parse(ts_path)
    for context in tree.getroot().findall("context"):
        name = context.findtext("name") or ""
        for message in context.findall("message"):
            source = message.findtext("source") or ""
            translation_node = message.find("translation")
            translation = (translation_node.text or "") if translation_node is not None else ""
            yield name, source, translation


def main():
    if not TS_FILES:
        print("translations/ 下没有 .ts 文件", file=sys.stderr)
        return 1

    problems = []
    total = 0
    untranslated = []

    # 自动提取的"给模型看的"文案：它们绝不该出现在 .ts 的 <source> 里。
    forbidden = schema_descriptions(ROOT)
    print(f"从源码提取到 {len(forbidden)} 条工具/参数说明（这些必须留在 .ts 之外）")

    for ts_path in TS_FILES:
        for context, source, translation in sources(ts_path):
            total += 1
            if source in forbidden:
                problems.append((ts_path.name, context, source))
            # 尚未翻译的条目：不是错误（翻译是逐步补的），但要能看见进度。
            if not translation.strip():
                untranslated.append((context, source))

    for name, context, source in problems:
        print(f"❌ {name} [{context}] 疑似把「给模型的文案」标记成了可翻译：{source}",
              file=sys.stderr)

    print(f"扫描 {len(TS_FILES)} 个 .ts，共 {total} 条可翻译文案，"
          f"其中未翻译 {len(untranslated)} 条")

    if "--list" in sys.argv:
        for context, source in untranslated:
            print(f"  [{context}] {source}")

    if problems:
        print("边界检查失败：请把上面的字符串改回 QStringLiteral（不要用 tr()）。",
              file=sys.stderr)
        return 1
    print("边界检查通过：翻译文件里没有发给模型的文案。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
