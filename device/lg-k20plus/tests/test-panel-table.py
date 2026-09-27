#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""LG4894 init-table transcription guard.

Parses lg4894_init[] out of linux/lg_msm8917_panel.c and checks every entry
against the vendor qcom,mdss-dsi-on-command byte counts: the LG() macro's
len must equal the payload length (vendor len field minus the command
byte). Sleep-out/display-on carries the 100 ms wait.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "linux" / "lg_msm8917_panel.c"

# cmd -> (payload bytes, wait_ms), from the vendor on-command table.
EXPECTED = {
    0xB0: (1, 0), 0xB1: (4, 0), 0xB2: (12, 0), 0xB4: (3, 0),
    0xB5: (5, 0), 0xB6: (3, 0), 0xBD: (9, 0), 0xBE: (7, 0),
    0xC1: (6, 0), 0xC2: (3, 0), 0xC3: (6, 0), 0xC4: (3, 0),
    0xC5: (5, 0), 0xC7: (12, 0), 0xC8: (4, 0), 0xCC: (14, 0),
    0xD0: (12, 0), 0xD1: (12, 0), 0xD2: (12, 0), 0xD3: (12, 0),
    0xD4: (12, 0), 0xD5: (12, 0), 0xE4: (21, 0), 0xE5: (12, 0),
    0x11: (0, 100), 0x29: (0, 0),
}


def main() -> None:
    text = SRC.read_text()
    m = re.search(r"lg4894_init\[\] = \{(.*?)\n\};", text, re.S)
    assert m, "lg4894_init not found"
    seen = set()
    for em in re.finditer(r"LG_CMD\((0x[0-9A-Fa-f]+),\s*(\d+)(.*?)\)", m.group(1)):
        cmd, wait, rest = int(em.group(1), 0), int(em.group(2)), em.group(3)
        payload = len([b for b in rest.split(",") if b.strip().startswith("0x")])
        assert cmd in EXPECTED, f"unexpected DCS {cmd:#04x}"
        exp_len, exp_wait = EXPECTED[cmd]
        assert payload == exp_len, \
            f"DCS {cmd:#04x}: {payload} bytes, vendor has {exp_len}"
        assert wait == exp_wait, f"DCS {cmd:#04x}: wait {wait}, vendor {exp_wait}"
        seen.add(cmd)
    assert seen == set(EXPECTED), f"missing: {set(EXPECTED) - seen}"
    print(f"PASS: {len(seen)} LG4894 init entries match the vendor table")


if __name__ == "__main__":
    sys.exit(main())
