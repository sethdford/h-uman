"""Measurement math. A rate over an empty (or too small) denominator is
"not measured", never 0 (.claude/rules/no-number-without-a-measurement.md)."""
import math

NOT_MEASURED = "not measured"


def wilson(k, n, z=1.96):
    if n <= 0:
        return None
    p = k / n
    d = 1 + z * z / n
    center = (p + z * z / (2 * n)) / d
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return max(0.0, center - half), min(1.0, center + half)


def rate(k, n, min_n=1):
    if n <= 0 or n < min_n:
        return NOT_MEASURED
    lo, hi = wilson(k, n)
    return {"k": k, "n": n, "rate": round(k / n, 4), "ci95": [round(lo, 4), round(hi, 4)]}


def cohen_kappa(pairs):
    n = len(pairs)
    if n == 0:
        return None
    labels = {x for p in pairs for x in p}
    po = sum(1 for a, b in pairs if a == b) / n
    pe = sum((sum(1 for a, _ in pairs if a == lab) / n) *
             (sum(1 for _, b in pairs if b == lab) / n) for lab in labels)
    if pe == 1:
        return 1.0 if po == 1 else 0.0
    return (po - pe) / (1 - pe)
