"""Regression checks for the final-page rain/magic transition clock."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "TextAnimator.cpp").read_text(encoding="utf-8")


class TextAnimatorTransitionTests(unittest.TestCase):
    def test_last_page_does_not_wrap_back_to_its_previous_scene(self) -> None:
        self.assertIn("elapsed - pageStart, transitionSlot - 1", SOURCE)
        transition_setup = SOURCE[SOURCE.index("uint32_t transitionSlot") :]
        transition_setup = transition_setup[: transition_setup.index("display.clear()")]
        self.assertNotIn("elapsed % transitionSlot", transition_setup)


if __name__ == "__main__":
    unittest.main()
