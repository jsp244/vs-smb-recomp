"""Compare a harness trace against the reference nestest.log.

usage: python compare_nestest.py our_trace.txt nestest.log
"""
import re
import sys

ref_re = re.compile(r"^([0-9A-F]{4}).*A:([0-9A-F]{2}) X:([0-9A-F]{2}) Y:([0-9A-F]{2}) P:([0-9A-F]{2}) SP:([0-9A-F]{2}).*CYC:(\d+)")
our_re = re.compile(r"^([0-9A-F]{4}) A:([0-9A-F]{2}) X:([0-9A-F]{2}) Y:([0-9A-F]{2}) P:([0-9A-F]{2}) SP:([0-9A-F]{2}) CYC:(\d+)")


def parse(path, rx):
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = rx.match(line.rstrip("\n"))
            if m:
                out.append(m.groups())
    return out


ours = parse(sys.argv[1], our_re)
ref = parse(sys.argv[2], ref_re)
n = min(len(ours), len(ref))
for i in range(n):
    if ours[i] != ref[i]:
        print(f"MISMATCH at line {i + 1}:")
        print("  ref :", " ".join(ref[i]))
        print("  ours:", " ".join(ours[i]))
        for j in range(max(0, i - 3), i):
            print("  prev:", " ".join(ref[j]))
        sys.exit(1)
print(f"OK: {n} instructions match (ours={len(ours)} ref={len(ref)})")
