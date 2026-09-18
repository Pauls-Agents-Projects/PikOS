#!/usr/bin/env python3
"""Compatibility launcher for ``pikos_credentials.py netatmo``.

New setup instructions use the unified helper.  This filename remains so old
bookmarks still reach the safe flow, but client secrets are no longer accepted
as command-line arguments where they can appear in shell history/process lists.
"""

from __future__ import annotations

import sys
from typing import Sequence

try:
    from tools import pikos_credentials
except ImportError:  # ``python tools/netatmo_auth.py``
    import pikos_credentials  # type: ignore[no-redef]


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if any(
        argument == "--client-secret" or argument.startswith("--client-secret=")
        for argument in arguments
    ):
        print(
            "--client-secret is no longer accepted; run "
            "tools/pikos_credentials.py netatmo and enter it at the hidden prompt.",
            file=sys.stderr,
        )
        return 2
    print(
        "netatmo_auth.py is now a compatibility launcher for the unified "
        "PikOS credential helper.",
        file=sys.stderr,
    )
    return pikos_credentials.main(["netatmo", *arguments])


if __name__ == "__main__":
    raise SystemExit(main())
