#!/usr/bin/env bash
# Single entry point for the formatting check, used identically locally and in CI.
#
# Checks (never rewrites) every file in the formatted scope with the reference clang-format and
# fails if any file would change. It also fails - rather than passing vacuously - when the tool is
# missing, is not the reference major version, or the scope resolves to no files.
#
# Scope (kept in sync with CONTRIBUTING.md): include/**/*.cppm, tests/**/*.cpp, tests/**/*.hpp,
# examples/**/*.cpp.
#
# Usage: scripts/check-format.sh
# Env:   CLANG_FORMAT  path to the clang-format binary (default: clang-format-21, then
#                      /usr/lib/llvm-21/bin/clang-format)
set -euo pipefail

readonly REFERENCE_MAJOR=21

fail() {
    echo "check-format: ERROR: $*" >&2
    exit 1
}

cd "$(dirname "${BASH_SOURCE[0]}")/.."

tool="${CLANG_FORMAT:-}"
if [ -z "$tool" ]; then
    if command -v "clang-format-${REFERENCE_MAJOR}" >/dev/null 2>&1; then
        tool="clang-format-${REFERENCE_MAJOR}"
    elif [ -x "/usr/lib/llvm-${REFERENCE_MAJOR}/bin/clang-format" ]; then
        tool="/usr/lib/llvm-${REFERENCE_MAJOR}/bin/clang-format"
    else
        fail "clang-format-${REFERENCE_MAJOR} not found (set CLANG_FORMAT to override)."
    fi
fi
command -v "$tool" >/dev/null 2>&1 || [ -x "$tool" ] || fail "'$tool' is not an executable clang-format."

version_line="$("$tool" --version)"
echo "check-format: using $tool: $version_line"
major="$(printf '%s\n' "$version_line" | sed -n 's/.*clang-format version \([0-9][0-9]*\).*/\1/p' | head -1)"
[ "$major" = "$REFERENCE_MAJOR" ] || fail "clang-format major version is '${major:-unknown}', expected ${REFERENCE_MAJOR}."

mapfile -t files < <(
    {
        find include -name '*.cppm'
        find tests \( -name '*.cpp' -o -name '*.hpp' \)
        find examples -name '*.cpp'
    } 2>/dev/null | LC_ALL=C sort
)
[ "${#files[@]}" -gt 0 ] || fail "the formatted scope resolved to no files; refusing to pass vacuously."

echo "check-format: checking ${#files[@]} files"
if ! "$tool" --style=file --dry-run --Werror "${files[@]}"; then
    fail "formatting differs. Run: $tool --style=file -i <file>  (see CONTRIBUTING.md)."
fi
echo "check-format: OK (${#files[@]} files, clang-format ${major})"
