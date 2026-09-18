"""Regression coverage for punctuation seen in real RSS headlines.

PixelFont is compiled against Arduino's String type, so the host test checks
the small, declarative glyph table directly.  The firmware build still checks
the C++ itself; this test makes an accidental return to the fallback question
mark visible in the ordinary desktop suite.
"""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "PixelFont.cpp").read_text(encoding="utf-8")


class PixelFontHeadlineGlyphTests(unittest.TestCase):
    def glyph(self, case_label: str) -> tuple[int, ...]:
        pattern = re.compile(
            rf"case\s+{re.escape(case_label)}\s*:\s*(?:\/\/[^\n]*\n\s*)*"
            r"return\s+makeGlyph\(([^;]+)\);"
        )
        match = pattern.search(SOURCE)
        self.assertIsNotNone(match, f"missing PixelFont glyph for {case_label}")
        values = tuple(int(token.strip(), 0) for token in match.group(1).split(","))
        self.assertEqual(len(values), 8)
        self.assertGreater(values[0], 0, "glyph width must be positive")
        self.assertTrue(any(values[1:]), "glyph must light at least one pixel")
        return values

    def test_rss_punctuation_has_real_glyphs(self) -> None:
        for case_label in (
            "'|'",
            "'&'",
            "0x00ab",
            "0x00bb",
            "0x00b7",
            "0x00c9",
            "0x00e9",
            "0x27e9",
        ):
            with self.subTest(case_label=case_label):
                self.glyph(case_label)

    def test_right_chevron_is_not_a_parenthesis(self) -> None:
        chevron = self.glyph("0x27e9")
        parenthesis = self.glyph("')'")
        self.assertNotEqual(chevron, parenthesis)
        self.assertEqual(chevron[1], 0, "chevron should have an open top row")
        self.assertEqual(chevron[-1], 0, "chevron should have an open bottom row")


if __name__ == "__main__":
    unittest.main()
