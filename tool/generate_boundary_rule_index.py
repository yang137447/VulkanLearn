"""Generate the boundary-rule index from the audit script itself.

The script is the executable architecture contract, and its 64 rule names were
previously transcribed into `documents/routing.md` by hand -- two copies of the same
fact, kept in step only by a validator that shouted when they diverged. This removes
the second copy: the index is generated from the script, and
`tool/validate-docs.py` fails when the generated file is stale.

Parsing is deliberately shallow: for each `Test-*(...)` invocation it captures the
helper, the rule name, and the arguments that describe what is being constrained
(paths, patterns, directories, files). It never interprets the rules, so it cannot
become a second opinion about what they mean.
"""

import argparse
import re
import sys
from pathlib import Path

SCRIPT = "tool/ue-lite-boundary-audit.ps1"
OUTPUT = "documents/contracts/boundary-rules.generated.md"

# `Test-Foo ` at the start of a line begins an invocation; its argument list runs
# until the closing backtick-continued `-RuleName "..."` line.
INVOCATION = re.compile(r'^(Test-[A-Za-z]+)\s+`\s*$', re.MULTILINE)
RULE_NAME = re.compile(r'-RuleName\s+"([^"]+)"')
ARGUMENT = re.compile(r'-(RelativePaths|Patterns|Directories|Files|Path)\s+@\(([^)]*)\)', re.DOTALL)
PATH_LIST = re.compile(r'^\s*"([^"]+)"', re.MULTILINE)
PATTERN_LIST = re.compile(r"^\s*'((?:[^'\\]|\\.)*)'", re.MULTILINE)

KIND = {
    "Test-PathsAbsent": "path must not exist",
    "Test-PathsPresent": "path must exist",
    "Test-FilePatternsAbsent": "pattern must not match in a file",
    "Test-FilePatternsPresent": "pattern must match in a file",
    "Test-DirectoryPatternsAbsent": "pattern must not match in a directory",
    "Test-SourcePatternsAbsent": "pattern must not match anywhere in source",
    # Rules expressed through bespoke helpers rather than the generic pattern checks.
    "Test-RuntimeValidationAdapterBoundary": "bespoke boundary check",
    "Test-VulkanManagerBoundary": "bespoke boundary check",
    "Test-RendererBackendRawGetterBoundary": "bespoke boundary check",
    "Test-RendererSubmitBoundary": "bespoke boundary check",
    "Test-RHIDeviceVulkanPublicRawGetterBoundary": "bespoke boundary check",
    "Test-VulkanManagerPublicBoundary": "bespoke boundary check",
}


def parse(text):
    """Split the script at each invocation and read the rule name from that segment.

    A fixed-size window is wrong here: some rules carry long path lists, so a window
    wide enough for the longest one swallows the next rule's name and every rule
    after it is attributed to the wrong subject. Segmenting on the invocation
    boundaries keeps each rule's arguments and its name together.
    """
    starts = [(match.start(), match.group(1)) for match in INVOCATION.finditer(text)]
    invocations = []
    for index, (start, helper) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else len(text)
        segment = text[start:end]
        name = RULE_NAME.search(segment)
        if name is None:
            continue
        arguments = {}
        for argument in ARGUMENT.finditer(segment):
            key, body = argument.group(1), argument.group(2)
            if key == "Patterns":
                arguments[key] = PATTERN_LIST.findall(body)
            else:
                arguments[key] = PATH_LIST.findall(body)
        invocations.append({
            "helper": helper,
            "name": name.group(1),
            "arguments": arguments,
        })
    return invocations


def render(invocations, revision):
    lines = [
        "# 架构边界规则索引（生成物，请勿手工编辑）",
        "",
        "> 状态：**当前契约**（实现已落地）。本文件由 `tool/generate_boundary_rule_index.py` 从 "
        "`tool/ue-lite-boundary-audit.ps1` 生成；`tool/validate-docs.py` 会在生成物过期时判失败。"
        "改规则请改脚本再重新生成，不要编辑本文件。",
        "",
        "脚本是**可执行的架构契约**：它定义代码不得违反什么。规则正文以脚本为唯一真相，本表只是索引——",
        "它说明每条规则的名字、它做什么检查、以及它作用于哪些路径。",
        "",
        "生成源 revision：`%s`" % revision,
        "",
        "| 规则名 | 检查类型 | 作用对象 |",
        "| --- | --- | --- |",
    ]
    for invocation in invocations:
        arguments = invocation["arguments"]
        targets = []
        for key in ("RelativePaths", "Files", "Directories", "Path"):
            for value in arguments.get(key, []):
                targets.append(value)
        if not targets:
            # No path argument in the invocation: for the *-Source* helpers that means
            # the whole source tree, but for the file helpers the path list is computed
            # by script logic above the call, so claiming "whole repository" would be a
            # guess. Say which of the two it is.
            if invocation["helper"] in ("Test-SourcePatternsAbsent", "Test-DirectoryPatternsAbsent"):
                targets = ["（全仓库源码树）"]
            else:
                targets = ["（路径由脚本内条件生成）"]
        shown = targets[:6]
        suffix = " 等 %d 项" % len(targets) if len(targets) > len(shown) else ""
        kind = KIND.get(invocation["helper"], invocation["helper"])
        lines.append("| %s | %s | %s |"
                     % (invocation["name"], kind, ", ".join("`%s`" % item for item in shown) + suffix))
    lines.append("")
    lines.append("共 %d 条规则。" % len(invocations))
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", nargs="?", default=".", type=Path)
    parser.add_argument("--check", action="store_true",
                        help="fail instead of writing when the generated file is stale")
    args = parser.parse_args()
    root = args.repo.resolve()

    script = root / SCRIPT
    text = script.read_text(encoding="utf-8-sig")
    invocations = parse(text)
    if not invocations:
        print("no rule invocations parsed from %s" % SCRIPT, file=sys.stderr)
        return 1

    import subprocess
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                          capture_output=True, text=True)
    revision = head.stdout.strip() if head.returncode == 0 else "unknown"

    # The revision line is metadata about when the index was produced, not part of the
    # contract, so a stale check compares everything after it.
    rendered = render(invocations, revision)
    target = root / OUTPUT

    def body(value):
        return "\n".join(line for line in value.splitlines()
                         if not line.startswith("生成源 revision："))

    if args.check:
        if not target.is_file():
            print("%s is missing; run tool/generate_boundary_rule_index.py" % OUTPUT,
                  file=sys.stderr)
            return 1
        if body(target.read_text(encoding="utf-8-sig")) != body(rendered):
            print("%s is stale; run tool/generate_boundary_rule_index.py" % OUTPUT,
                  file=sys.stderr)
            return 1
        print("rule index OK (%d rules)" % len(invocations))
        return 0

    target.write_text(rendered, encoding="utf-8", newline="")
    print("wrote %s (%d rules)" % (OUTPUT, len(invocations)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
