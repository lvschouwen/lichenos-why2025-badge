#!/usr/bin/env bash
# Host test and gate (#385): `badge.sh new-app` scaffolds a throwaway app into a TEMP tree (never the real one) and the
# layer checker, the app boundary check and the app's own host test pass on it, so the template cannot go stale. Also:
# bad ids, an existing id, hostile names are refused with the tree untouched, a second run changes nothing, and the
# shared-file inserts are idempotent. The temp tree links to the real components; only the files the scaffold edits
# (lichen_apps, tools/layers.conf, host_tests/CMakeLists.txt) are copies.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(dirname "$(dirname "$here")")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

# A tree the checks accept: everything linked except what the scaffold edits
t=$tmp/tree
mkdir -p "$t/tools" "$t/firmware/components" "$t/host_tests"
for d in "$repo"/*; do
    case $(basename "$d") in firmware|tools|host_tests) ;; *) ln -s "$d" "$t/$(basename "$d")" ;; esac
done
for d in "$repo"/firmware/*; do
    [ "$(basename "$d")" = components ] || ln -s "$d" "$t/firmware/$(basename "$d")"
done
for d in "$repo"/firmware/components/*; do
    n=$(basename "$d")
    if [ "$n" = lichen_apps ]; then cp -r "$d" "$t/firmware/components/$n"; else ln -s "$d" "$t/firmware/components/$n"; fi
done
for f in "$repo"/tools/*; do
    case $(basename "$f") in layers.conf) cp "$f" "$t/tools/" ;; *) ln -s "$f" "$t/tools/$(basename "$f")" ;; esac
done
cp "$repo/host_tests/CMakeLists.txt" "$t/host_tests/"
# the real tree is read-only for this test: remember it to prove that below
# only the paths the scaffold could touch, so a build writing elsewhere in the real tree cannot fail this test
real_snap() { (cd "$repo" && { find firmware/components/lichen_apps tools/layers.conf host_tests/CMakeLists.txt host_tests/*_model_test.c -type f | sort | xargs sha256sum; ls firmware/components; }) | sha256sum; }
real_before=$(real_snap)

snap() { (cd "$t" && { find firmware/components/lichen_apps tools/layers.conf host_tests -type f | sort | xargs sha256sum; ls firmware/components; }) | sha256sum; }
new_app() { python3 -I "$repo/tools/new_app.py" --root "$t" "$@"; }
refuses() { # refuses <why> <id> <name>: exit 1, a message, tree untouched
    local why=$1 before after out
    shift
    before=$(snap)
    out=$(new_app "$@" 2>&1); local rc=$?
    after=$(snap)
    [ "$rc" = 1 ] || bad "$why: exit $rc, expected 1 ($out)"
    grep -q '^new-app: ' <<<"$out" || bad "$why: no refusal message ($out)"
    [ "$before" = "$after" ] || bad "$why: the tree changed"
}

# Refusals first, all on the clean tree
for id in "" Foo 1abc a-b a/b ../x ..  a.b "a b" toolongidentifier1 .hidden 'a;b' 'a$(id)'; do
    refuses "bad id '$id'" "$id" "Fine"
done
for id in notes calc clock meshcore settings lichen c6 crash test qrcode lora_spectrum; do
    refuses "taken id '$id'" "$id" "Fine"
done
refuses "empty name" ok1 ""
refuses "long name" ok1 "$(printf 'n%.0s' $(seq 1 25))"
refuses "quote in name" ok1 'My "App"'
refuses "backslash in name" ok1 'My\App'
refuses "newline in name" ok1 $'My\nApp'
refuses "tab in name" ok1 $'My\tApp'
refuses "comment end in name" ok1 'x */ y?'
refuses "template slot in name" ok1 '@ID@'
refuses "non-ASCII name" ok1 'Caf'$'\xc3\xa9'
refuses "padded name" ok1 ' App'

# A leftover test file or target of that id is refused too, not overwritten
touch "$t/host_tests/zz_model_test.c"
refuses "existing test file" zz "Fine"
rm "$t/host_tests/zz_model_test.c"
cp "$t/host_tests/CMakeLists.txt" "$tmp/host_cmake.bak"
echo 'add_test(NAME yy_model_test COMMAND yy_model_test)' >> "$t/host_tests/CMakeLists.txt"
refuses "existing test target" yy "Fine"
cp "$tmp/host_cmake.bak" "$t/host_tests/CMakeLists.txt"

# A write that fails half way rolls everything back (a read-only tools/ stops the layers.conf write, after the
# lichen_apps files were changed); then the same run works. Not testable as root, which ignores the mode.
if [ "$(id -u)" != 0 ]; then
    chmod a-w "$t/tools"
    refuses "write failure" demo "Demo app"
    chmod u+w "$t/tools"
else
    echo "note: running as root, the rollback case is skipped" >&2
fi

# A good one
before=$(snap)
out=$(new_app demo "Demo app" 2>&1) || bad "good id refused: $out"
[ "$before" != "$(snap)" ] || bad "the good id changed nothing"
for f in firmware/components/lichen_demo/{CMakeLists.txt,lichen_demo.h,app_demo.c,demo_model.c,demo_model.h} host_tests/demo_model_test.c; do
    [ -s "$t/$f" ] || bad "missing $f"
done
grep -q 'PRIV_REQUIRES lichen_api)' "$t/firmware/components/lichen_demo/CMakeLists.txt" || bad "component CMake does not require lichen_api only"
grep -q '\.name .*= "Demo app"' "$t/firmware/components/lichen_demo/app_demo.c" || bad "the name is not in the descriptor"
grep -q 'demo_app_register();' "$t/firmware/components/lichen_apps/lichen_apps.c" || bad "not registered"
grep -q 'lichen_demo' "$t/firmware/components/lichen_apps/CMakeLists.txt" || bad "lichen_apps does not require it"
grep -q '^app demo lichen_demo ' "$t/tools/layers.conf" || bad "no app line"
grep -q 'add_test(NAME demo_model_test' "$t/host_tests/CMakeLists.txt" || bad "no host test target"
if grep -rq '@[A-Z_]*@' "$t/firmware/components/lichen_demo" "$t/host_tests/demo_model_test.c"; then bad "an unfilled template slot"; fi

# A second run, with the same or another name: refused, nothing changes
after_first=$(snap)
refuses "second run" demo "Demo app"
refuses "second run, other name" demo "Other"
[ "$after_first" = "$(snap)" ] || bad "the second run changed the tree"

# The shared-file inserts are idempotent: applied to their own result they change nothing
python3 -I - "$repo/tools" "$t" <<'PY' || bad "an insert is not idempotent"
import os, sys
sys.path.insert(0, sys.argv[1])
import new_app
root = sys.argv[2]
for rel, fn in new_app.SHARED:
    text = new_app.read(os.path.join(root, rel))
    assert fn(text, "demo") == text, rel
    assert text.count("demo") >= 1, rel
PY

# The layer checks on the scaffolded tree (rule C needs a build: --no-symbols), then the app boundary check alone
out=$(python3 -I "$repo/tools/check_layers.py" --root "$t" --no-symbols --no-merge-base 2>&1) || bad "layer check failed on the scaffold: $out"
out=$("$repo/tools/check_app_boundary.sh" --root "$t" --no-symbols --no-merge-base 2>&1) || bad "app boundary check failed on the scaffold: $out"

cc=${CC:-cc}
# The generated app file compiles (syntax only) against the real lvgl headers and the gate's: a template that drifted
# from lichen_label_set, menu_*, lichen_app_t ... fails here. lvgl is a managed component, so a clone that has not
# built once has none; then this step is skipped with a note (the gap: the IDF build is what compiles it).
lvgl=$repo/firmware/managed_components/lvgl__lvgl
if [ -f "$lvgl/lvgl.h" ]; then
    out=$("$cc" -std=gnu11 -fsyntax-only -Wall -Wextra -Werror -DLV_CONF_SKIP=1 -I"$lvgl" \
        -I"$repo/firmware/components/lichen_api" -I"$t/firmware/components/lichen_demo" \
        "$t/firmware/components/lichen_demo/app_demo.c" 2>&1) || bad "the generated app_demo.c does not compile: $out"
else
    echo "note: firmware/managed_components/lvgl__lvgl missing, the generated app is not compiled (build once first)" >&2
fi

# The generated model test builds and passes the way host_tests builds it
comp=$t/firmware/components/lichen_demo
if out=$("$cc" -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$comp" "$t/host_tests/demo_model_test.c" "$comp/demo_model.c" -o "$tmp/demo_model_test" 2>&1); then
    out=$("$tmp/demo_model_test" 2>&1) || bad "the generated model test failed: $out"
else
    bad "the generated model test does not build: $out"
fi

# The real tree was not touched
[ "$real_before" = "$(real_snap)" ] || bad "the real tree changed"
[ ! -e "$repo/firmware/components/lichen_demo" ] || bad "a generated app is in the real tree"

# badge.sh new-app is host-only: no host, no ssh, and bad input is refused before anything else
out=$(env -u BADGE_HOST BADGE_HOST= XDG_CONFIG_HOME="$tmp/none" PATH="$tmp/nobin:/usr/bin:/bin" bash "$repo/tools/badge.sh" new-app 'Bad Id' x 2>&1); rc=$?
[ "$rc" = 1 ] && grep -q '^new-app: ' <<<"$out" || bad "badge.sh new-app with a bad id: exit $rc: $out"
out=$(bash "$repo/tools/badge.sh" new-app onlyone 2>&1); rc=$?
[ "$rc" = 1 ] && grep -q '^Usage:' <<<"$out" || bad "badge.sh new-app with one argument: exit $rc"

[ "$fail" = 0 ] && echo "new_app_test: ok"
exit $fail
