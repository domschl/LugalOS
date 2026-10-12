"""`llp`: placeholder until the serial tools land with 49.3."""

from __future__ import annotations

import sys


def main() -> None:
    print("llp: the serial client arrives with 49.3; for now see `llp-vectors`.", file=sys.stderr)
    sys.exit(2)
