#!/usr/bin/env bash
# Host test (#522): `badge.sh new-app --sd` scaffolds sdapps/<id>/ into a TEMP tree (never the real one), edits no shared
# file, refuses bad or taken ids and hostile names with the tree untouched; the generated sources pass the SD app
# boundary check and compile against the real headers; the boundary check catches a forbidden include and symbol;
# `badge.sh sdapp-push` refuses bad ids and a missing build and sends the two files to SD0:[lichen.apps.<id>].
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(dirname "$(dirname "$here")")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

# A tree the scaffold and the checks accept: all linked except sdapps, which starts with a copy of the real ones
t=$tmp/tree
mkdir -p "$t"
for d in "$repo"/*; do
    case $(basename "$d") in sdapps) ;; *) ln -s "$d" "$t/$(basename "$d")" ;; esac
done
mkdir "$t/sdapps"
cp "$repo/sdapps/sdapp.cmake" "$t/sdapps/"
cp -r "$repo/sdapps/hello" "$t/sdapps/"
snap() { (cd "$t" && find sdapps -type f | sort | xargs sha256sum; ls sdapps) | sha256sum; }
real_before=$(cd "$repo" && { find sdapps firmware/components tools/layers.conf host_tests/CMakeLists.txt -type f | sort | xargs sha256sum; } | sha256sum)
new_sd() { python3 -I "$repo/tools/new_app.py" --root "$t" --sd "$@"; }
refuses() { # refuses <why> <id> <name>
    local why=$1 before out rc
    shift
    before=$(snap)
    out=$(new_sd "$@" 2>&1); rc=$?
    [ "$rc" = 1 ] || bad "$why: exit $rc, expected 1 ($out)"
    grep -q '^new-app: ' <<<"$out" || bad "$why: no refusal message ($out)"
    [ "$before" = "$(snap)" ] || bad "$why: the tree changed"
}

for id in "" Foo 1abc a-b a/b ../x .. a.b "a b" toolongidentifier1 'a;b'; do refuses "bad id '$id'" "$id" Fine; done
for id in notes calc settings meshcore lichen c6 crash test hello; do refuses "taken id '$id'" "$id" Fine; done
refuses "empty name" ok1 ""
refuses "quote in name" ok1 'My "App"'
refuses "newline in name" ok1 $'My\nApp'
refuses "template slot in name" ok1 '@ID@'

# A write that fails after its temp file exists leaves nothing behind
before=$(snap)
out=$(NEW_APP_TEST_FAIL_AFTER_TEMP=app.c new_sd demo "Demo app" 2>&1); rc=$?
[ "$rc" = 1 ] && grep -q 'removed' <<<"$out" || bad "failing write: exit $rc: $out"
[ "$before" = "$(snap)" ] || bad "failing write: the tree changed"
[ ! -e "$t/sdapps/demo" ] || bad "failing write: a directory is left"

before=$(snap)
out=$(new_sd demo "Demo app" 2>&1) || bad "good id refused: $out"
for f in CMakeLists.txt manifest main/CMakeLists.txt main/app.c; do [ -s "$t/sdapps/demo/$f" ] || bad "missing $f"; done
grep -q '^id=demo$' "$t/sdapps/demo/manifest" || bad "manifest id"
grep -q '^name=Demo app$' "$t/sdapps/demo/manifest" || bad "manifest name"
grep -Eq '^api=[0-9]+\.[0-9]+$' "$t/sdapps/demo/manifest" || bad "manifest api"
for k in entry caps internal_ram_kib psram_kib stack_kib; do grep -q "^$k=" "$t/sdapps/demo/manifest" || bad "manifest key $k"; done
grep -q 'project(demo)' "$t/sdapps/demo/CMakeLists.txt" || bad "CMake does not name the project"
grep -rq '@[A-Z_]*@' "$t/sdapps/demo" && bad "an unfilled template slot"
# Nothing shared was edited: the only new path is sdapps/demo
after=$(cd "$t/sdapps" && ls | tr '\n' ' ')
[ "$after" = "demo hello sdapp.cmake " ] || bad "unexpected sdapps content: $after"
refuses "second run" demo "Demo app"

# The boundary check: the scaffold and hello pass; a lichen_sys_ include, a non-API header and a forbidden call fail
python3 -I "$repo/tools/check_sdapp_boundary.py" --root "$t" || bad "boundary check fails on the scaffold and hello"
"$repo/tools/check_app_boundary.sh" --root "$t" --no-symbols --no-merge-base >/dev/null 2>&1 || bad "check_app_boundary.sh fails on the scaffold"
for case in '#include "lichen_sys_ui.h"' '#include "esp_system.h"' '#include "lora_app.h"' 'void f(void){ esp_restart(); }' 'void f(void){ free(0); }'; do
    cp -r "$t/sdapps/demo" "$t/sdapps/zz"
    printf '%s\n' "$case" >> "$t/sdapps/zz/main/app.c"
    out=$(python3 -I "$repo/tools/check_sdapp_boundary.py" --root "$t" 2>&1) && bad "boundary check accepted: $case"
    grep -q 'sdapps/zz/main/app.c' <<<"$out" || bad "boundary message does not name the file for: $case ($out)"
    mv "$t/sdapps/zz" "$tmp/zz-gone"; rm -rf "$tmp/zz-gone"
done

# The generated app compiles against the real headers (syntax only), as the app build does
lvgl=$repo/firmware/managed_components/lvgl__lvgl
if [ -f "$lvgl/lvgl.h" ] && [ -f "$repo/firmware/build/config/sdkconfig.h" ]; then
    grep '^#define CONFIG_LV_' "$repo/firmware/build/config/sdkconfig.h" > "$tmp/lv.h"
    out=$("${CC:-cc}" -std=gnu11 -fsyntax-only -Wall -Wextra -Werror -DLV_CONF_INCLUDE_SIMPLE -include "$tmp/lv.h" -I"$lvgl" \
        -I"$repo/firmware/components/lichen_api" "$t/sdapps/demo/main/app.c" 2>&1) || bad "the generated app.c does not compile: $out"
else
    echo "SKIP: no configured firmware tree (lvgl or build/config/sdkconfig.h missing): the generated app.c is compiled by the build only" >&2
fi

# The real tree was not touched
real_after=$(cd "$repo" && { find sdapps firmware/components tools/layers.conf host_tests/CMakeLists.txt -type f | sort | xargs sha256sum; } | sha256sum)
[ "$real_before" = "$real_after" ] || bad "the real tree changed"
[ ! -e "$repo/sdapps/demo" ] || bad "a generated app is in the real tree"

# badge.sh: new-app --sd needs 4 arguments; a bad id is refused host-only
out=$(bash "$repo/tools/badge.sh" new-app --sd onlyone 2>&1); rc=$?
[ "$rc" = 1 ] && grep -q '^Usage:' <<<"$out" || bad "new-app --sd with one argument: exit $rc"
out=$(bash "$repo/tools/badge.sh" new-app --sd 'Bad Id' x 2>&1); rc=$?
[ "$rc" = 1 ] && grep -q '^new-app: ' <<<"$out" || bad "new-app --sd with a bad id: exit $rc: $out"

# sdapp-push: refusals first, then the two files go to SD0:[lichen.apps.<id>] (curl is a stub that records X-Path)
push() { env BADGE_SDAPPS_DIR="$tmp/built" BADGE_OTA_TOKEN=x XDG_CONFIG_HOME="$tmp/none" PATH="$tmp/stub:$PATH" bash "$repo/tools/badge.sh" sdapp-push "$@" 2>&1; }
mkdir -p "$tmp/stub" "$tmp/built/demo"
cat > "$tmp/stub/curl" <<STUB
#!/usr/bin/env bash
for a in "\$@"; do case \$a in X-Path:*) echo "\$a" >> "$tmp/paths" ;; esac; done
exit 0
STUB
chmod +x "$tmp/stub/curl"
out=$(push host 'Bad Id'); rc=$?
[ "$rc" = 1 ] && grep -q '^sdapp-push: bad id' <<<"$out" || bad "sdapp-push bad id: exit $rc: $out"
out=$(push host demo); rc=$?
[ "$rc" = 1 ] && grep -q 'build first' <<<"$out" || bad "sdapp-push without a build: exit $rc: $out"
echo elf > "$tmp/built/demo/app.elf"; echo id=demo > "$tmp/built/demo/manifest"
out=$(push host demo); rc=$?
[ "$rc" = 0 ] || bad "sdapp-push: exit $rc: $out"
grep -qx 'X-Path: SD0:\[lichen.apps.demo\]app.elf' "$tmp/paths" 2>/dev/null || bad "app.elf not sent to SD0:[lichen.apps.demo]: $(cat "$tmp/paths" 2>/dev/null)"
grep -qx 'X-Path: SD0:\[lichen.apps.demo\]manifest' "$tmp/paths" 2>/dev/null || bad "manifest not sent"

[ "$fail" = 0 ] && echo "new_app_sd_test: ok"
exit $fail
