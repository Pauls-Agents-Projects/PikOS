from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class GarminTokenParserNativeTests(unittest.TestCase):
    def test_large_tokens_are_parsed_without_a_second_large_pool(self) -> None:
        compiler = shutil.which("c++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("no host C++ compiler is available")
        with tempfile.TemporaryDirectory(prefix="pikos-garmin-token-") as temp:
            executable = Path(temp) / "garmin_token_parser_test"
            subprocess.run(
                [
                    compiler,
                    "-std=c++11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include"),
                    "-I",
                    str(ROOT / ".pio/libdeps/tc001/ArduinoJson/src"),
                    str(ROOT / "tests/native/test_garmin_token_parser.cpp"),
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
            self.assertIn("Garmin token parser tests passed", completed.stdout)


if __name__ == "__main__":
    unittest.main()
