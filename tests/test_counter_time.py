from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class CounterTimeNativeTests(unittest.TestCase):
    def test_counter_time_regressions(self) -> None:
        compiler = shutil.which("c++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("no host C++ compiler is available")
        with tempfile.TemporaryDirectory(prefix="pikos-counter-test-") as temp:
            executable = Path(temp) / "counter_time_test"
            subprocess.run(
                [
                    compiler,
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include"),
                    str(ROOT / "tests" / "native" / "test_counter_time.cpp"),
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
            self.assertIn("CounterTime native tests passed", completed.stdout)


if __name__ == "__main__":
    unittest.main()
