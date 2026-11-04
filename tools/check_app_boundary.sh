#!/usr/bin/env bash
# App boundary check (#178, docs/app-api.md "Boundary check"). Fails when an app component:
#   A. REQUIRES / PRIV_REQUIRES a component that is not on the allow-list,
#   B. #includes a header of a repo component that is not allowed (or an unlisted system header),
#   C. leaves a forbidden OS-only symbol undefined in its object files (nm -u on the built archive).
# Config: tools/app_boundary.conf. Run after the IDF build (rule C needs firmware/build); --no-symbols skips C.
set -u
repo="$(cd "$(dirname "$0")/.." && pwd)"
fw="$repo/firmware"
. "$repo/tools/app_boundary.conf"
check_syms=1
[ "${1:-}" = "--no-symbols" ] && check_syms=0
fails=0
fail() { echo "app boundary: $1 [$2]" >&2; fails=$((fails + 1)); }
in_list() { local x="$1"; shift; local i; for i in "$@"; do [ "$i" = "$x" ] && return 0; done; return 1; }
excepted() { in_list "$1:$2" $3; }   # app, item, list

# Header basename -> repo component (components other than the app itself)
declare -A comp_of
for d in "$fw"/components/*/; do
    c="$(basename "$d")"
    while IFS= read -r h; do comp_of["$(basename "$h")"]="$c"; done < <(find "$d" -name '*.h' -not -path '*/build/*')
done

# Rule A: tokens after REQUIRES / PRIV_REQUIRES, with ${VAR} expanded from set(VAR ...), as "line token"
requires_of() {
    awk '
    { sub(/#.*/, ""); gsub(/\(/, " ( "); gsub(/\)/, " ) ");
      n = split($0, t, /[ \t]+/);
      for (i = 1; i <= n; i++) {
        w = t[i]; if (w == "") continue;
        if (w == "set") { st = "set-open"; continue }
        if (st == "set-open" && w == "(") { st = "set-name"; continue }
        if (st == "set-name") { var = w; st = "set"; continue }
        if (st == "set" && w == ")") { st = ""; continue }
        if (st == "set") { setv[var] = setv[var] " " NR ":" w; continue }
        if (w == "REQUIRES" || w == "PRIV_REQUIRES") { st = "req"; continue }
        if (st == "req" && w == ")") { st = ""; continue }
        if (st == "req" && w ~ /^[A-Z_]+$/) { st = ""; continue }
        if (st == "req") { out[++m] = NR ":" w }
      } }
    END { for (j = 1; j <= m; j++) {
            split(out[j], p, ":"); w = p[2];
            if (w ~ /^\$\{.*\}$/) { v = substr(w, 3, length(w) - 3); k = split(setv[v], s, " ");
                for (q = 1; q <= k; q++) { split(s[q], r, ":"); print r[1], r[2] } }
            else print p[1], w } }' "$1"
}

for app in $APP_COMPONENTS; do
    dir="$fw/components/$app"
    cm="components/$app/CMakeLists.txt"
    while read -r line comp; do
        in_list "$comp" $ALLOWED_COMPONENTS && continue
        excepted "$app" "$comp" "$EXCEPT_COMPONENTS" && continue
        fail "component '$comp' is not on the app allow-list (rule A; see tools/app_boundary.conf)" "firmware/$cm:$line"
    done < <(requires_of "$dir/CMakeLists.txt")

    # Rule B
    own_dirs=""; for e in $APP_OWN_DIRS; do [ "${e%%:*}" = "$app" ] && own_dirs="$own_dirs ${e#*:}"; done
    while IFS=: read -r file line text; do
        inc="$(printf '%s' "$text" | sed -E 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^>"]+)[>"].*/\1/')"
        rel="${file#"$repo/"}"
        # own file, or under the app's own / bundled third-party roots
        found=0
        for o in $own_dirs; do [ -e "$fw/$o/$inc" ] && found=1; done
        [ -e "$(dirname "$file")/$inc" ] && found=1
        [ $found = 1 ] && continue
        excepted "$app" "$inc" "$EXCEPT_HEADERS" && continue
        owner="${comp_of[$(basename "$inc")]:-}"
        if [ -n "$owner" ]; then
            [ "$owner" = "$app" ] && continue
            in_list "$owner" $ALLOWED_COMPONENTS && continue
            excepted "$app" "$owner" "$EXCEPT_COMPONENTS" && continue
            fail "#include \"$inc\" belongs to component '$owner', which apps may not use (rule B)" "$rel:$line"
            continue
        fi
        ok=0; for pat in $SYSTEM_HEADERS; do case "$inc" in $pat) ok=1 ;; esac; done
        [ $ok = 1 ] && continue
        fail "#include \"$inc\" is not an allowed header (rule B; add it to SYSTEM_HEADERS only if it is libc or a plain library)" "$rel:$line"
    done < <(grep -rnE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]' --include='*.c' --include='*.cpp' --include='*.h' "$dir" | grep -v '/build/')
done

# Rule C
if [ $check_syms = 1 ]; then
    nm="$(command -v riscv32-esp-elf-nm || true)"
    for app in $APP_COMPONENTS; do
        lib="$fw/build/esp-idf/$app/lib$app.a"
        if [ ! -f "$lib" ]; then
            echo "app boundary: no $lib yet, symbol rule (C) skipped" >&2
            continue
        fi
        if [ -z "$nm" ]; then
            fail "cannot check symbols: riscv32-esp-elf-nm is not on PATH (IDF export.sh)" "tools/check_app_boundary.sh"
            continue
        fi
        while read -r obj sym; do
            for pat in $FORBIDDEN_SYMBOLS; do
                case "$sym" in $pat)
                    excepted "$app" "$sym" "$EXCEPT_SYMBOLS" || fail "object references OS-only symbol '$sym' (rule C)" "$obj" ;;
                esac
            done
        done < <("$nm" -A -u "$lib" 2>/dev/null | awk '$2 == "U" { sub(/^[^:]*:/, "", $1); sub(/:$/, "", $1); print $1, $3 }')
    done
fi

if [ $fails -gt 0 ]; then
    echo "app boundary: $fails violation(s). Apps reach the OS through lichen_api only (docs/app-api.md)." >&2
    exit 1
fi
echo "app boundary: ok ($APP_COMPONENTS)"
