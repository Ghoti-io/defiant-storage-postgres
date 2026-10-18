#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# What this library may contain, checked from its source:
#
# - libpq's header is included by src/model.c alone, never by a public header
#   (a caller of this library needs no libpq header), and SQLite's not at all;
# - SQL text appears in src/ only in the dialect (src/model.c, between the
#   "the dialect" and "the executor's state" sections): every other
#   statement comes from defiant-storage-sql, and the session settings are
#   libpq options, not statements. A string literal holding an SQL keyword
#   anywhere else is refused.
#
# With --self-test it also plants each kind of defect in a scratch copy and
# requires the check to fail on it, so the gate is seen to catch them.
#
# Usage: check-seams.sh <root> [--self-test]

set -u

ROOT="${1:?usage: check-seams.sh <root> [--self-test]}"
SELF_TEST="${2:-}"

check() {
  root=$1
  bad=0
  hits=$(grep -rl '#include <libpq-fe.h>' "$root/src" "$root/include" 2>/dev/null |
    grep -v "^$root/src/model.c\$" || true)
  if [ -n "$hits" ]; then
    printf 'check-seams: libpq-fe.h is included outside src/model.c: %s\n' "$hits" >&2
    bad=1
  fi
  hits=$(grep -rl 'sqlite3\.h' "$root/src" "$root/include" 2>/dev/null || true)
  if [ -n "$hits" ]; then
    printf 'check-seams: sqlite3.h is included: %s\n' "$hits" >&2
    bad=1
  fi
  for f in "$root"/src/*.c "$root"/src/*.h; do
    [ -e "$f" ] || continue
    out=$(awk -v file="$f" '
      /\/\* ---- the dialect / { in_dialect = 1 }
      /\/\* ---- the executor.s state / { in_dialect = 0 }
      {
        line = $0
        # Only what is inside string literals.
        lits = ""
        while (match(line, /"([^"\\]|\\.)*"/)) {
          lits = lits " " substr(line, RSTART, RLENGTH)
          line = substr(line, RSTART + RLENGTH)
        }
        if (!in_dialect && lits ~ /(^|[^A-Za-z_])(SELECT|INSERT|UPDATE|DELETE|CREATE|DROP|ALTER|BEGIN|COMMIT|ROLLBACK|SET|SHOW|RESET|DISCARD|TRUNCATE|GRANT|VACUUM|ANALYZE|LISTEN|NOTIFY|LOCK)([^A-Za-z_]|$)/) {
          printf "%s:%d: %s\n", file, NR, $0
        }
      }' "$f")
    if [ -n "$out" ]; then
      printf 'check-seams: SQL text outside the dialect:\n%s\n' "$out" >&2
      bad=1
    fi
  done
  return $bad
}

check "$ROOT" || exit 1

if [ "$SELF_TEST" = --self-test ]; then
  scratch=$(mktemp -d "${TMPDIR:-/tmp}/gdefspg-seams-XXXXXX") || exit 1
  trap 'rm -rf "$scratch"' EXIT
  for plant in sql-outside-dialect libpq-in-public-header; do
    rm -rf "$scratch/tree"
    mkdir -p "$scratch/tree"
    cp -a "$ROOT/src" "$ROOT/include" "$scratch/tree/"
    case $plant in
    sql-outside-dialect)
      printf '%s\n' 'static const char planted[] = "SET search_path TO x";' \
        >> "$scratch/tree/src/wire.c" ;;
    libpq-in-public-header)
      printf '%s\n' '#include <libpq-fe.h>' \
        >> "$scratch/tree/include/ghoti.io/defiant-storage-postgres/model.h" ;;
    esac
    if check "$scratch/tree" 2>/dev/null; then
      printf 'check-seams: the planted %s passed the check\n' "$plant" >&2
      exit 1
    fi
    printf 'check-seams: plant %s caught\n' "$plant"
  done
fi
printf 'check-seams: libpq is reached from src/model.c alone, and SQL text only from the dialect\n'
