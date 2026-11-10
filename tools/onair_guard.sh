# On-air guard (#287): one rule set for every console line that can transmit, sourced by tools/badge.sh (fw and
# console, before anything is sent) and by .claude/hooks/guard-badge.sh (the command line). Bench rule: on air only
# the channel #badgetest and the test node. Allowed contact targets (names or key digits, one per line, # comments)
# live in the gitignored .claude/onair-allow.local, so no device id is committed (the repo is public); without that
# file only #badgetest sends pass. The badge-side check that also covers other routes is #289.
# shellcheck shell=bash

# Allow file: the .claude/onair-allow.local next to this file's tools/. Only the guard hook may point elsewhere
# (onair_allow_override, from ONAIR_ALLOW_FILE in Claude Code's own environment, for its test); badge.sh clears the
# override before sourcing this, so a caller's environment never changes the list badge.sh checks.
onair_allow_file() {
    local repo
    repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
    printf '%s\n' "${onair_allow_override:-$repo/.claude/onair-allow.local}"
}

# True when target matches a line of the allow file, ignoring case
onair_allowed() {
    local file
    file=$(onair_allow_file)
    [ -n "$1" ] && [ -r "$file" ] || return 1
    grep -v '^[[:space:]]*#' "$file" | sed 's/[[:space:]]*$//' | grep -qixF -- "$1"
}

# Verdict for one console line ("[fw ]mesh <sub> [target] ..."): prints "pass", "deny <reason>" or "ask <reason>".
# Quotes and backslashes are dropped first, as the shell would before the line reaches the badge.
onair_verdict() {
    local w1 w2 w3 _rest
    read -r w1 w2 w3 _rest <<<"$(printf '%s' "$1" | tr -d "'\"\\\\")"
    if [ "$w1" = fw ]; then
        read -r w1 w2 w3 _rest <<<"$w2 $w3 $_rest"
    fi
    [ "$w1" = mesh ] || { echo pass; return; }
    case "$w2" in
        send)
            if [ "$w3" = "#badgetest" ]; then echo pass; else
                echo "deny on air only #badgetest: 'mesh send ${w3:-?}' would transmit to another channel (#287, bench rules)"
            fi ;;
        dm | telemetry | trace | discover)
            if [ -z "$w3" ] || onair_allowed "$w3"; then echo pass; else
                echo "deny on air only the test node: 'mesh $w2 $w3' targets a contact not in .claude/onair-allow.local (#287)"
            fi ;;
        remote)
            if [ -z "$w3" ] || [ "$w3" = settings ] || onair_allowed "$w3"; then echo pass; else
                echo "deny on air only the test node: 'mesh remote $w3' targets a node not in .claude/onair-allow.local (#287)"
            fi ;;
        advert)
            case "$w3" in
                interval | manual) echo "ask mesh advert $w3 changes what the badge floods on air: needs Lucas (#287)" ;;
                *) echo pass ;;
            esac ;;
        *) echo pass ;;
    esac
}
