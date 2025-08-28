import re
import sys

"""
Generate data with this command 'cangen can0 -D i -I i -L 8 -g 10'
"""


def parse_can_line(line):
    """
    Parse line of format: (timestamp) can <ID>#<DATA>
    Returns (timestamp, can_id) or None if invalid.
    """
    # Ignore comment/metadata lines starting with "*"
    if line.strip().startswith("*"):
        return None

    match = re.match(r"\(([\d\.]+)\)\s+\w+\s+([0-9A-Fa-f]+)#", line.strip())
    if not match:
        return None
    ts = float(match.group(1))
    can_id = int(match.group(2), 16)
    return ts, can_id


def check_canlog(filename):
    with open(filename, "r") as f:
        lines = f.readlines()

    prev_id = None
    errors = 0
    missing_total = 0
    total_logged = 0
    total_expected = 0

    for idx, line in enumerate(lines):
        parsed = parse_can_line(line)
        if not parsed:
            # skip invalid/comment lines silently
            continue

        ts, can_id = parsed
        total_logged += 1

        if prev_id is None:
            # first frame sets starting point
            prev_id = can_id
            total_expected = 1
            continue

        # expected next CAN ID
        expected_id = (prev_id + 1) & 0x7FF

        if can_id == expected_id:
            total_expected += 1
            prev_id = can_id
            continue

        # check if frames were skipped
        diff = (can_id - prev_id) & 0x7FF
        if 1 < diff <= 50:  # allow search up to 50 frames gap
            print(f"Line {idx + 1}: Skipped {diff - 1} frame(s) before this line")
            missing_total += diff - 1
            total_expected += diff
            prev_id = can_id
        else:
            print(f"Line {idx + 1}: Corrupted ID {can_id:03X}, expected {expected_id:03X}")
            errors += 1
            total_expected += 1
            prev_id = expected_id  # resync expectation

    print("\n===== SUMMARY =====")
    print(f"Total frames logged   : {total_logged}")
    print(f"Total frames expected : {total_expected}")
    print(f"Missing frames        : {missing_total}")
    print(f"Corrupted frames      : {errors}")
    if errors == 0 and missing_total == 0:
        print("✅ CAN ID sequence is correct, no frames missing")
    else:
        loss_pct = (missing_total / total_expected * 100) if total_expected > 0 else 0
        print(f"❌ Issues detected — {missing_total} missing, {errors} corrupted "
              f"({loss_pct:.2f}% data loss)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python check_canlog.py canlog00.log")
        sys.exit(1)
    check_canlog(sys.argv[1])
