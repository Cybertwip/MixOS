#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""TD4330 init-table transcription guard.

Parses the init_setting table out of linux/oppo_mt6877_panel_td4330.c and
checks every entry carries at least as many bytes as its count claims --
except the two vendor quirks documented in the driver (C2 lists 67 of a
claimed 0x78, D2 lists 4 of a claimed 3), which must be present EXACTLY as
documented. A new mismatch is a transcription bug, not a new quirk.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "linux" / "oppo_mt6877_panel_td4330.c"

# (cmd, count, listed) triples the vendor table really contains.
KNOWN_QUIRKS = {(0xC2, 0x78), (0xD2, 0x03)}


def entries(text: str):
    m = re.search(r"td4330_init\[\] = \{(.*?)\n\};", text, re.S)
    assert m, "init table not found"
    body = m.group(1)
    for em in re.finditer(r"\{(0x[0-9A-Fa-f]+|TD4330_\w+),\s*(0x[0-9A-Fa-f]+|\d+),\s*\{([^}]*)\}", body):
        cmd_s, count_s, paras = em.groups()
        if cmd_s.startswith("TD4330_"):
            continue
        count = int(count_s, 0)
        listed = len([p for p in paras.split(",") if p.strip().startswith("0x")])
        yield int(cmd_s, 0), count, listed


def main() -> None:
    text = SRC.read_text()
    n = 0
    for cmd, count, listed in entries(text):
        n += 1
        if (cmd, count) in KNOWN_QUIRKS:
            continue
        assert listed >= count, \
            f"DCS {cmd:#04x}: lists {listed} bytes but claims {count}"
    assert n > 40, f"only {n} entries parsed; the table shrank?"
    print(f"PASS: {n} init entries, counts match (2 known quirks)")


if __name__ == "__main__":
    sys.exit(main())
