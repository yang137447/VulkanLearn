"""Prove that the UI visual regression checker catches the defects it exists for.

A checker that only ever prints "passed" is worthless, so this feeds it a healthy reply
plus five doctored ones and asserts every doctored case fails.

This is a **pure local comparison**: it never calls a vision model and costs nothing, so
unlike `check_ui_visual_regression.py` itself it is cheap enough to run any time. The
healthy reply is embedded as a fixture rather than read from `artifacts/`, which is
gitignored and would make this test silently disappear.

The five defects are the ones actually observed or feared in this project:

  unnamed tab         a tab rendering as an icon with no label (a 94px blank tab was
                      measured in this repository, which is why the check exists)
  renamed tab         a label that no longer matches the authored markup
  toolbar duplicates  document commands restated as toolbar buttons
  missing menu        a menu that did not render
  invented menu       a menu the repository does not declare

Usage: py -3 tool/test_ui_visual_checker.py
"""
import copy
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CHECKER = REPO / "tool" / "check_ui_visual_regression.py"
SCRATCH = REPO / "artifacts" / "rmlui-editor" / "_visual-checker-case.json"

# A reply the vision model actually produced from a healthy capture at 1280x720.
HEALTHY = {
    "menu": ["File", "Edit", "Window", "Help"],
    "product": "VulkanLearn",
    "tabs": [
        {"label": "Viewport", "has_text": True},
        {"label": "Asset Editor", "has_text": True},
        {"label": "Scene Outliner", "has_text": True},
        {"label": "Details", "has_text": True},
        {"label": "Content Browser", "has_text": True},
        {"label": "Renderer", "has_text": True},
    ],
    "toolbar_between_product_and_buttons": [],
}


def run_case(reply, label, expect_failure):
    SCRATCH.parent.mkdir(parents=True, exist_ok=True)
    SCRATCH.write_text(json.dumps(reply, indent=2), encoding="utf-8")
    try:
        result = subprocess.run(
            [sys.executable, str(CHECKER), "check", "--reply", str(SCRATCH)],
            capture_output=True, text=True)
    finally:
        SCRATCH.unlink(missing_ok=True)

    problems = [line.strip() for line in result.stderr.splitlines()
                if line.strip().startswith("- ")]
    verdict = "failed as expected" if expect_failure else "passed"
    status = "ok " if (result.returncode != 0) == expect_failure else "BAD"
    print("  [%s] %-20s exit=%d  %s" % (status, label, result.returncode, verdict))
    if problems:
        print("        %s" % problems[0][:120])
    return (result.returncode != 0) == expect_failure


def with_tab(mutate):
    case = copy.deepcopy(HEALTHY)
    mutate(case)
    return case


def main():
    print("healthy reply and five injected defects:")
    results = []

    results.append(run_case(HEALTHY, "healthy", expect_failure=False))

    def drop_text(case):
        case["tabs"].insert(1, {"label": "", "has_text": False})
    results.append(run_case(with_tab(drop_text), "unnamed tab", expect_failure=True))

    def rename(case):
        for entry in case["tabs"]:
            if entry["label"] == "Scene Outliner":
                entry["label"] = "Outliner"
    results.append(run_case(with_tab(rename), "renamed tab", expect_failure=True))

    def duplicate_commands(case):
        case["toolbar_between_product_and_buttons"] = ["Save", "Revert", "Validate"]
    results.append(run_case(with_tab(duplicate_commands), "toolbar duplicates",
                            expect_failure=True))

    def drop_menu(case):
        case["menu"] = ["File", "Edit", "Window"]
    results.append(run_case(with_tab(drop_menu), "missing menu", expect_failure=True))

    def invent_menu(case):
        case["menu"] = case["menu"] + ["Assets"]
    results.append(run_case(with_tab(invent_menu), "invented menu", expect_failure=True))

    print()
    if all(results):
        print("all %d cases behaved correctly (1 healthy pass, %d defects caught)"
              % (len(results), len(results) - 1))
        return 0
    print("%d case(s) behaved incorrectly" % results.count(False), file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
