#!/usr/bin/env bash
# Reproducible clang-tidy run over mddlog's own sources, used identically locally and in CI.
#
# Preconditions (each violation FAILS the run - nothing is silently skipped or reported green):
#   - clang-tidy is present and is the reference major version (21), the same LLVM as the Clang
#     preset that built the module interfaces;
#   - the Clang preset has been configured AND built, so compile_commands.json and the named-module
#     BMIs (std, mddlog.*) exist: clang-tidy cannot analyse C++ named modules without them, and a
#     GCC build or a different LLVM major cannot supply them;
#   - every file in the scope has an entry in compile_commands.json.
# Any diagnostic at all (findings are promoted to errors) or any per-file tool failure fails the run.
#
# Scope (translation units): include/mddlog/**/*.cppm, tests/spec/*.cpp, examples/*.cpp.
# tests/framework/*.hpp is a header, not a translation unit: it is analysed through the spec files
# that include it (see HeaderFilterRegex in .clang-tidy).
#
# Usage: scripts/run-clang-tidy.sh [build-dir]        (default: build-clang)
# Env:   CLANG_TIDY  clang-tidy binary (default: clang-tidy-21, then /usr/lib/llvm-21/bin/clang-tidy)
#        JOBS        parallel analyses (default: nproc)
set -euo pipefail

readonly REFERENCE_MAJOR=21

fail() {
    echo "run-clang-tidy: ERROR: $*" >&2
    exit 1
}

# 'wait -n' (used to bound parallelism) needs bash >= 4.3.
if [ "${BASH_VERSINFO[0]}" -lt 4 ] || { [ "${BASH_VERSINFO[0]}" -eq 4 ] && [ "${BASH_VERSINFO[1]}" -lt 3 ]; }; then
    fail "bash >= 4.3 is required (found ${BASH_VERSION})."
fi

cd "$(dirname "${BASH_SOURCE[0]}")/.."
root="$PWD"
build_dir="${1:-build-clang}"

tool="${CLANG_TIDY:-}"
if [ -z "$tool" ]; then
    if command -v "clang-tidy-${REFERENCE_MAJOR}" >/dev/null 2>&1; then
        tool="clang-tidy-${REFERENCE_MAJOR}"
    elif [ -x "/usr/lib/llvm-${REFERENCE_MAJOR}/bin/clang-tidy" ]; then
        tool="/usr/lib/llvm-${REFERENCE_MAJOR}/bin/clang-tidy"
    else
        fail "clang-tidy-${REFERENCE_MAJOR} not found (set CLANG_TIDY to override)."
    fi
fi
command -v "$tool" >/dev/null 2>&1 || [ -x "$tool" ] || fail "'$tool' is not an executable clang-tidy."

version_text="$("$tool" --version)"
major="$(printf '%s\n' "$version_text" | sed -n 's/.*LLVM version \([0-9][0-9]*\).*/\1/p' | head -1)"
echo "run-clang-tidy: using $tool"
printf '%s\n' "$version_text" | sed 's/^/run-clang-tidy:   /'
[ "$major" = "$REFERENCE_MAJOR" ] || fail "clang-tidy major version is '${major:-unknown}', expected ${REFERENCE_MAJOR}."

db="$build_dir/compile_commands.json"
[ -f "$db" ] || fail "$db not found. Configure and BUILD the Clang preset first (cmake --preset ninja-clang && cmake --build --preset ninja-clang)."
grep -q '\.modmap' "$db" || fail "$db has no module map references; it was not produced by the Clang preset (a GCC build cannot supply Clang BMIs)."
find "$build_dir" -name '*.pcm' -print -quit | grep -q . \
    || fail "no built module interfaces (*.pcm) under $build_dir; build the Clang preset before analysing."

scope=()
collect_scope() {
    local root_dir="$1" pattern="$2" found f
    [ -d "$root_dir" ] || fail "scope directory '$root_dir' does not exist."
    found="$(find "$root_dir" -type f -name "$pattern")" || fail "scope discovery failed under '$root_dir'."
    while IFS= read -r f; do
        [ -n "$f" ] && scope+=("$f")
    done < <(printf '%s\n' "$found" | LC_ALL=C sort)
    return 0
}
collect_scope include/mddlog '*.cppm'
collect_scope tests/spec '*.cpp'
collect_scope examples '*.cpp'
[ "${#scope[@]}" -gt 0 ] || fail "the analysis scope resolved to no files; refusing to pass vacuously."

missing=()
for f in "${scope[@]}"; do
    grep -Fq "\"file\": \"$root/$f\"" "$db" || missing+=("$f")
done
if [ "${#missing[@]}" -gt 0 ]; then
    printf 'run-clang-tidy: ERROR: in scope but absent from %s (would be silently unanalysed):\n' "$db" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
fi

out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT
jobs_n="${JOBS:-$(nproc 2>/dev/null || echo 1)}"
case "$jobs_n" in
    ''|*[!0-9]*|0) fail "JOBS must be a positive integer, got '$jobs_n'." ;;
esac

analyse() {
    local f="$1" log="$out_dir/$(printf '%s' "$1" | tr '/' '_').log"
    if "$tool" -p "$build_dir" --warnings-as-errors='*' --quiet "$f" >"$log" 2>&1; then
        echo "$f" >>"$out_dir/ok.list"
    else
        echo "$f" >>"$out_dir/failed.list"
    fi
}

echo "run-clang-tidy: analysing ${#scope[@]} translation units with $jobs_n jobs"
running=0
for f in "${scope[@]}"; do
    analyse "$f" &
    running=$((running + 1))
    if [ "$running" -ge "$jobs_n" ]; then
        wait -n || true
        running=$((running - 1))
    fi
done
wait || true

ok_count=0; [ -f "$out_dir/ok.list" ] && ok_count="$(wc -l <"$out_dir/ok.list")"
fail_count=0; [ -f "$out_dir/failed.list" ] && fail_count="$(wc -l <"$out_dir/failed.list")"
module_errors="$(cat "$out_dir"/*.log 2>/dev/null | grep -c 'clang-diagnostic-error' || true)"
cppm_total=0; cppm_ok=0
for f in "${scope[@]}"; do
    case "$f" in *.cppm)
        cppm_total=$((cppm_total + 1))
        if [ -f "$out_dir/ok.list" ] && grep -qxF "$f" "$out_dir/ok.list"; then cppm_ok=$((cppm_ok + 1)); fi ;;
    esac
done

summary="clang-tidy $major: analysed ${ok_count}/${#scope[@]} translation units cleanly (.cppm interface units: ${cppm_ok}/${cppm_total}); ${fail_count} failed; clang-diagnostic-error occurrences: ${module_errors}."
echo "run-clang-tidy: $summary"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### clang-tidy report"
        echo
        echo "$summary"
        echo
        echo "Version: \`$(printf '%s' "$version_text" | head -1)\`"
        echo
        echo "Scope (${#scope[@]} translation units):"
        printf -- '- `%s`\n' "${scope[@]}"
    } >>"$GITHUB_STEP_SUMMARY"
fi

if [ "$fail_count" -gt 0 ]; then
    while read -r f; do
        echo "----- $f -----"
        cat "$out_dir/$(printf '%s' "$f" | tr '/' '_').log"
    done <"$out_dir/failed.list"
    fail "$fail_count of ${#scope[@]} translation units failed (findings or tool errors above)."
fi
[ "$ok_count" -eq "${#scope[@]}" ] || fail "only $ok_count of ${#scope[@]} translation units were analysed."
echo "run-clang-tidy: OK"
