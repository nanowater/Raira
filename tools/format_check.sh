#!/usr/bin/env bash
# Formatting and lint of our own code: clang-format (the repository's .clang-format) over the C++ sources, cargo fmt and clippy for the
# bridge. Vendored code (lvgl/, src/lib/, nlohmann_json) is left alone.
#
#   tools/format_check.sh         report what differs; exit code 1 when anything does
#   tools/format_check.sh --fix   rewrite the files (clang-format -i, cargo fmt); clippy findings are only reported
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FIX=0; [ "${1:-}" = "--fix" ] && FIX=1
cd "$ROOT" || exit 1
status=0

if command -v clang-format >/dev/null; then
  bad=0
  while read -r f; do
    if [ "$FIX" = 1 ]; then
      clang-format -i "$f"
    elif ! clang-format --dry-run --Werror "$f" >/dev/null 2>&1; then
      echo "clang-format: $f"; bad=$((bad + 1))
    fi
  done < <(git ls-files 'src/*.cpp' 'src/*.h' 'firmware/components/music_platform_esp/*.cpp' 'firmware/main/*.cpp' 'tools/*.cpp' |
           grep -v '^src/lib/')
  [ "$bad" -gt 0 ] && { echo "clang-format: $bad file(s) differ (tools/format_check.sh --fix)"; status=1; }
else
  echo "clang-format not found: C++ formatting not checked"
fi

if command -v cargo >/dev/null; then
  if [ "$FIX" = 1 ]; then
    (cd bridge && cargo fmt)
  elif ! (cd bridge && cargo fmt --check >/dev/null); then
    echo "cargo fmt: the bridge differs (tools/format_check.sh --fix)"; status=1
  fi
  if ! out=$(cd bridge && cargo clippy --all-targets --quiet -- -D warnings 2>&1); then
    printf '%s\n' "$out" | grep -E '^error|-->'
    echo "clippy: findings in the bridge"; status=1
  fi
else
  echo "cargo not found: the bridge is not checked"
fi

[ "$status" = 0 ] && echo "formatting and lint: clean"
exit "$status"
