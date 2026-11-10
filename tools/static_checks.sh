#!/usr/bin/env bash
# Fast static checks run by firmware/build.sh build (#196):
#  - shellcheck (warning level) on every tracked shell script;
#  - tools/check_rules.py: critical-section PSRAM (#271), ESP_LOGE retry/dropped wording (#113), LP RAM / PSRAM task
#    functions never return (#269), lv_* in task entry functions (#118); baselines tools/rules-baseline-<check>.txt;
#  - cppcheck (warning + portability) on the C/C++ files changed since origin/main, committed or not.
# Existing accepted findings live in tools/cppcheck-baseline.txt (id:file); new code gets no baseline entries,
# fix the finding or add an inline "// cppcheck-suppress <id>" with the reason.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
fail=0

if command -v shellcheck >/dev/null; then
    mapfile -t scripts < <(git ls-files '*.sh')
    shellcheck -x -S warning "${scripts[@]}" || { echo "static checks: shellcheck found warnings" >&2; fail=1; }
else
    echo "note: shellcheck not installed, skipped" >&2
fi

python3 -I tools/check_rules.py || { echo "static checks: source rule checks failed (tools/check_rules.py)" >&2; fail=1; }

if command -v cppcheck >/dev/null; then
    base=$(git merge-base HEAD origin/main 2>/dev/null || git merge-base HEAD main 2>/dev/null || echo HEAD)
    mapfile -t srcs < <( { git diff --name-only --diff-filter=d "$base" -- '*.c' '*.cpp'; git ls-files --others --exclude-standard -- '*.c' '*.cpp'; } |
        grep -E '^(firmware|c6|shared|host_tests)/' | grep -v '/thirdparty/' | sort -u)
    if [ ${#srcs[@]} -gt 0 ]; then
        cppcheck --enable=warning,portability --inline-suppr --std=c11 -q --error-exitcode=1 \
            --suppress=missingIncludeSystem --suppress=missingInclude --suppress=unknownMacro \
            --suppress=intToPointerCast --suppress=varFuncNullUB \
            --suppressions-list=tools/cppcheck-baseline.txt \
            --template='{file}:{line}: {severity}: {message} [{id}]' "${srcs[@]}" ||
            { echo "static checks: cppcheck findings in changed files (fix, or suppress inline with a reason)" >&2; fail=1; }
    fi
else
    echo "note: cppcheck not installed, skipped" >&2
fi
exit $fail
