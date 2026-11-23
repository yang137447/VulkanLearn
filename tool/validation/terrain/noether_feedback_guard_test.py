import pathlib
import re
import unittest


def enclosing_conditions(source, marker):
    source = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
        lambda match: " " * len(match.group()),
        source,
        flags=re.S,
    )
    position = source.index(marker)
    stack = []
    for index, char in enumerate(source[:position]):
        if char == "{":
            previous = index - 1
            while previous >= 0 and source[previous].isspace():
                previous -= 1
            condition = ""
            if previous >= 0 and source[previous] == ")":
                end = previous
                depth = 1
                previous -= 1
                while previous >= 0 and depth:
                    depth += (source[previous] == ")") - (source[previous] == "(")
                    previous -= 1
                prefix = source[: previous + 1].rstrip()
                if re.search(r"\bif$", prefix):
                    condition = source[previous + 2 : end]
            stack.append(condition)
        elif char == "}":
            stack.pop()
    return stack


def check_independent_composited(source):
    conditions = enclosing_conditions(source, "compositedFeedbackRing->RecordCopy")
    if any(re.search(r"->feedbackRing\b", condition) for condition in conditions):
        raise AssertionError("Composited readback still depends on the retired coverage ring")
    assert any("compositedFeedbackRing" in condition and "HasSurfaces" in condition
               for condition in conditions)


class FeedbackGuard(unittest.TestCase):
    def test_actual_frame_has_independent_readback_lifetimes(self):
        root = pathlib.Path(__file__).resolve().parents[3]
        source = (root / "source/render/frame/renderFrameRecorder.cpp").read_text("utf-8")
        check_independent_composited(source)
        coverage = enclosing_conditions(source, "feedbackRing->RecordCopy")
        self.assertTrue(any(re.search(r"->feedbackRing\b", condition) for condition in coverage))

    def test_original_game_dead_branch_is_rejected(self):
        old = """if (streaming && streaming->feedbackRing) {
            if (streaming->compositedFeedbackRing && service->HasSurfaces()) {
                streaming->compositedFeedbackRing->RecordCopy();
            }
        }"""
        with self.assertRaises(AssertionError):
            check_independent_composited(old)


if __name__ == "__main__":
    unittest.main()
