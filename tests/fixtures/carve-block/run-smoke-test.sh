#!/usr/bin/env bash
# Smoke test for scripts/carve-block.py (hu_agent_turn carve tooling).
# Run from the repo root: bash tests/fixtures/carve-block/run-smoke-test.sh
set -euo pipefail
root="$(git rev-parse --show-toplevel)"
tool="$root/scripts/carve-block.py"
t="$(mktemp -d "${TMPDIR:-/tmp}/carve-smoke.XXXXXX")"
trap 'rm -rf "$t"' EXIT

cat > "$t/f.c" <<'C'
int f(void) {
    int a = 1;
    /* begin */
    int b = 2;
    int keep_me = 3;
    a += b;
    /* end */
    return a + keep_me;
}
C
printf '    stage(&a, &b);\n@@CARVE_DROPPED@@\n' > "$t/r.c"
python3 "$tool" --file "$t/f.c" --start '    /* begin */' --end-after '    /* end */' \
    --drop '    int keep_me = 3;' --block-out "$t/b.c" --replace-with "$t/r.c" > "$t/out"
grep -q 'lines 3-6 (4 lines), moved 3, kept 1' "$t/out" || { cat "$t/out"; echo "FAIL: range report"; exit 1; }

cat > "$t/f.want" <<'C'
int f(void) {
    int a = 1;
    stage(&a, &b);
    int keep_me = 3;
    /* end */
    return a + keep_me;
}
C
printf '    /* begin */\n    int b = 2;\n    a += b;\n' > "$t/b.want"
diff -u "$t/f.want" "$t/f.c" || { echo "FAIL: file after carve"; exit 1; }
diff -u "$t/b.want" "$t/b.c" || { echo "FAIL: carved block"; exit 1; }

# prefix mode + start offset: include the line above a unique prefix anchor
cat > "$t/g.c" <<'C'
#if X
static void helper(int a,
                   int b) {
}
#endif
int tail;
C
printf '#endif\n' > "$t/r2.c"
python3 "$tool" --file "$t/g.c" --match prefix --start 'static void helper(' --start-offset -1 \
    --end-after 'int tail' --block-out "$t/b2.c" --replace-with "$t/r2.c" > /dev/null
printf '#endif\nint tail;\n' > "$t/g.want"
diff -u "$t/g.want" "$t/g.c" || { echo "FAIL: prefix/offset carve"; exit 1; }

# an anchor that matches twice is refused and changes nothing
printf 'void g(void) {\n    x();\n    x();\n}\n' > "$t/dup.c"
cp "$t/dup.c" "$t/dup.orig"
if python3 "$tool" --file "$t/dup.c" --start '    x();' --end-after '}' \
    --block-out "$t/x" --replace-with "$t/r.c" 2> "$t/err"; then
    echo "FAIL: ambiguous anchor accepted"
    exit 1
fi
grep -q 'matched' "$t/err" || { cat "$t/err"; echo "FAIL: error text"; exit 1; }
diff -q "$t/dup.orig" "$t/dup.c" > /dev/null || { echo "FAIL: file changed on error"; exit 1; }

# dropping lines without a marker in the replacement is refused
printf '    stage();\n' > "$t/r3.c"
cp "$t/f.want" "$t/h.c"
if python3 "$tool" --file "$t/h.c" --start '    stage(&a, &b);' --end-after '    /* end */' \
    --drop '    int keep_me = 3;' --block-out "$t/x" --replace-with "$t/r3.c" 2> /dev/null; then
    echo "FAIL: dropped lines would have been lost"
    exit 1
fi

# (F/ruling a) an anchor that matches ZERO lines is refused and changes nothing
cp "$t/f.want" "$t/zero.c"
cp "$t/f.want" "$t/zero.orig"
if python3 "$tool" --file "$t/zero.c" --start '    /* this text does not appear */' \
    --end-after '    /* end */' --block-out "$t/x" --replace-with "$t/r.c" 2> "$t/err0"; then
    echo "FAIL: a zero-match anchor was accepted"
    exit 1
fi
grep -q 'matched 0 lines' "$t/err0" || { cat "$t/err0"; echo "FAIL: zero-match error text"; exit 1; }
diff -q "$t/zero.orig" "$t/zero.c" > /dev/null || { echo "FAIL: file changed on zero-match error"; exit 1; }

echo "carve-block smoke: PASS"
