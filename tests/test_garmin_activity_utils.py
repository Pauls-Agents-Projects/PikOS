from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class GarminActivityNativeTests(unittest.TestCase):
    def test_activity_parsing_and_bounds(self) -> None:
        compiler = shutil.which("c++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("no host C++ compiler is available")
        with tempfile.TemporaryDirectory(prefix="pikos-garmin-test-") as temp:
            executable = Path(temp) / "garmin_activity_test"
            subprocess.run(
                [
                    compiler,
                    "-std=c++11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include"),
                    str(
                        ROOT
                        / "tests"
                        / "native"
                        / "test_garmin_activity_utils.cpp"
                    ),
                    "-o",
                    str(executable),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            completed = subprocess.run(
                [str(executable)],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn(
                "Garmin activity utility tests passed", completed.stdout
            )


if __name__ == "__main__":
    unittest.main()
