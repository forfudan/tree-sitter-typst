#!/usr/bin/env bash
# **How much real Typst this grammar parses.**
#
#     script/baseline.sh          # one line per corpus, and the total
#     script/baseline.sh --why    # and the line each failure stops on, and
#                                 # which kinds of node Typst reads otherwise
#
# The corpus is Typst written by the people who write Typst, three parts:
#
# 1. the compiler's test suite, `tests/suite/**/*.typ` in typst/typst. A file
#    there holds many tests, each under a `--- name ---` line, and many of them
#    are meant to fail; every test is cut out into a file of its own so that one
#    deliberate error does not hide the rest of its file;
# 2. every other `.typ` in that repository: the documentation and the test
#    packages;
# 3. the newest version of every package in typst/packages, `packages/preview`.
#
# ---- Two numbers, and why ---------------------------------------------------
#
# Typst's own parser is the judge (`script/typst-check`, typst-syntax pinned to
# the version the grammar was written against). What it rejects is not Typst,
# and the first number leaves it out: **that is the number to drive.** The
# second counts every file, so a change in how many the judge rejects cannot
# pass for progress.
#
# ---- And whether the trees are Typst's --------------------------------------
#
# A file can parse without an ERROR node and still be read wrong: a heading the
# grammar took for text parses fine. So the judge also parses every file it
# accepts into Typst's own tree and compares it with this grammar's on a few
# dozen kinds of node (`typst-check --compare`): how many there are, where each
# starts, where each ends. The three percentages under the table are that.
#
# Point it at your own checkouts with TYPST_SRC= and PACKAGES_SRC=. The packages
# repository is large; a sparse checkout of the `.typ` files is enough.
set -euo pipefail
export LC_ALL=C  # `comm` and `sort` must agree on the order
HERE=$(cd "$(dirname "$0")/.." && pwd)
TYPST=${TYPST_SRC:-$HOME/Programs/typst}
PACKAGES=${PACKAGES_SRC:-$HOME/Programs/typst-packages}
[ -d "$TYPST/tests/suite" ] || { echo "no typst checkout at $TYPST (set TYPST_SRC=)" >&2; exit 2; }
[ -d "$PACKAGES/packages/preview" ] || { echo "no packages checkout at $PACKAGES (set PACKAGES_SRC=)" >&2; exit 2; }

cd "$HERE"
# Warning: **`--abi 15` is pinned on purpose.** Downstream loads this parser with
# the tree-sitter 0.25 runtime, which reads ABI 13 to 15. A newer CLI may default
# to a newer ABI, and that parser.c would build and then refuse to load.
tree-sitter generate --abi 15 >/dev/null
cargo build --quiet --release --manifest-path script/typst-check/Cargo.toml --target-dir target
judge=target/release/typst-check

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/suite"

# Part 1: one file per test.
python3 -I - "$TYPST/tests/suite" "$work/suite" <<'EOF'
import pathlib, re, sys
root, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
header = re.compile(r"^--- ([^\s]+)(?: [^-]*)? ---$")
for path in sorted(root.rglob("*.typ")):
    stem = str(path.relative_to(root)).replace("/", "__")[:-4]
    name, lines = None, []
    def flush():
        if name is not None:
            (out / f"{stem}__{name}.typ").write_text("".join(lines))
    for line in path.read_text(errors="replace").splitlines(keepends=True):
        m = header.match(line.rstrip("\r\n"))
        if m:
            flush()
            name, lines = m.group(1), []
        else:
            lines.append(line)
    flush()
EOF
find "$work/suite" -name '*.typ' | sort > "$work/suite.list"

# Part 2: the rest of the typst repository.
find "$TYPST" -name '*.typ' -not -path '*/tests/suite/*' -not -path '*/.*' | sort > "$work/repo.list"

# Part 3: the newest version of each package.
find "$PACKAGES/packages/preview" -name '*.typ' | python3 -I -c '
import re, sys, collections
paths = [l.rstrip("\n") for l in sys.stdin]
def key(v): return tuple(int(x) if x.isdigit() else -1 for x in re.split(r"[.+-]", v))
newest = collections.defaultdict(str)
for p in paths:
    parts = p.split("/packages/preview/", 1)[1].split("/")
    if not newest[parts[0]] or key(parts[1]) > key(newest[parts[0]]):
        newest[parts[0]] = parts[1]
for p in sorted(paths):
    parts = p.split("/packages/preview/", 1)[1].split("/")
    if parts[1] == newest[parts[0]]:
        print(p)
' > "$work/packages.list"

cat "$work/suite.list" "$work/repo.list" "$work/packages.list" | "$judge" > "$work/verdicts"
grep '^rejected ' "$work/verdicts" | cut -d' ' -f2- | sort > "$work/rejected" || true

# Warning: **`tree-sitter parse` exits non-zero when any file fails**, and
# counting the failures is the whole point. Keep the `|| true`.
# Warning: **some package files have spaces in their names**, hence `-0`, and
# the output is split on its tab, not on whitespace.
cat "$work/suite.list" "$work/repo.list" "$work/packages.list" | tr '\n' '\0' \
  | xargs -0 tree-sitter parse --quiet --stat > "$work/parse" 2>&1 || true
# Warning: **a failure is an `ERROR` or a `MISSING` node.** Both print as a
# line naming the file; grepping for ERROR alone undercounts.
# The name is padded with spaces up to the tab.
sed -nE $'s/^(.*\\.typ) *\tParse:.*/\\1/p' "$work/parse" | sort -u > "$work/failed" || true

pct() { python3 -c "import sys; n,f=int(sys.argv[1]),int(sys.argv[2]); print(f'{(n-f)*100/max(n,1):.2f}')" "$1" "$2"; }
report() { # name list
  local n r f fr
  n=$(grep -c . "$2" || true)
  r=$(comm -12 "$2" "$work/rejected" | grep -c . || true)
  f=$(comm -12 "$2" "$work/failed" | grep -c . || true)
  fr=$(comm -12 "$2" "$work/failed" | comm -23 - "$work/rejected" | grep -c . || true)
  printf '%-10s %6s files, %5s rejected by Typst; of the %6s Typst accepts %4s fail: %6s%%   (all files: %6s%%)\n' \
    "$1" "$n" "$r" "$((n - r))" "$fr" "$(pct $((n - r)) "$fr")" "$(pct "$n" "$f")"
}
sort -o "$work/suite.list" "$work/suite.list"
sort -o "$work/repo.list" "$work/repo.list"
sort -o "$work/packages.list" "$work/packages.list"
cat "$work/suite.list" "$work/repo.list" "$work/packages.list" | sort > "$work/all.list"
printf 'typst:    %s\npackages: %s\n' "$TYPST" "$PACKAGES"
report tests "$work/suite.list"
report repo "$work/repo.list"
report packages "$work/packages.list"
report total "$work/all.list"

"$judge" --compare 3 < "$work/all.list" > "$work/compare"
printf '\n'
sed -n 1,4p "$work/compare"

[ "${1:-}" = "--why" ] || exit 0
printf '\nKinds of node where Typst reads some file otherwise, with examples:\n'
sed -n '7,$p' "$work/compare" | awk '/^    / || $NF != 0'
printf '\nFiles Typst accepts that fail here, with the line of the first error:\n'
comm -23 "$work/failed" "$work/rejected" | python3 -I -c '
import re, sys, pathlib
failed = set(l.strip() for l in sys.stdin)
pat = re.compile(r"^(.+?\.typ) *\t.*?\((?:ERROR|MISSING[^[]*) \[(\d+), (\d+)\]")
seen = set()
for line in open(sys.argv[1]):
    m = pat.match(line)
    if not m or m.group(1) not in failed or m.group(1) in seen: continue
    seen.add(m.group(1))
    row = int(m.group(2))
    try: src = pathlib.Path(m.group(1)).read_text(errors="replace").split("\n")[row]
    except Exception: src = ""
    print(f"{m.group(1)}:{row + 1}:{int(m.group(3)) + 1}  {src.strip()[:100]}")
' "$work/parse"
