# On-air guard (#287): one rule set for every console line that can transmit, sourced by tools/badge.sh (fw and
# console, before anything is sent) and by .claude/hooks/guard-badge.sh (the command line). Bench rule: on air only
# the channel #badgetest and the test node. Allowed contact targets (names or key digits, one per line, # comments)
# live in the gitignored .claude/onair-allow.local, so no device id is committed (the repo is public); without that
# file only #badgetest sends pass. The badge-side check that also covers other routes is #289.
# The same file names the console scope (#290): one "scope <region>" line. Every flood a console line starts goes out
# under the badge's console scope, so badge.sh checks before each on-air line with a target that the badge's console
# scope equals that line, and "mesh scope console <x>" passes only for it. Without the line nothing with a target goes
# on air through badge.sh. A "scope" line is never a contact target.
# shellcheck shell=bash

# Allow file: the .claude/onair-allow.local next to this file's tools/. Only the guard hook may point elsewhere
# (onair_allow_override, from ONAIR_ALLOW_FILE in Claude Code's own environment, for its test); badge.sh clears the
# override before sourcing this, so a caller's environment never changes the list badge.sh checks.
onair_allow_file() {
    local repo
    repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
    printf '%s\n' "${onair_allow_override:-$repo/.claude/onair-allow.local}"
}

# True when target matches a contact line of the allow file (not a comment, not a "scope" line), ignoring case
onair_allowed() {
    local file
    file=$(onair_allow_file)
    [ -n "$1" ] && [ -r "$file" ] || return 1
    grep -v '^[[:space:]]*#' "$file" | grep -viE '^[[:space:]]*scope([[:space:]]|$)' | sed 's/[[:space:]]*$//' |
        grep -qixF -- "$1"
}

# A region name as the badge keeps it (mesh_scope_parse): lowercase, no leading '#'
onair_scope_norm() {
    local name=${1,,}
    printf '%s' "${name#\#}"
}

# The console scopes the allow file names (#303: every exact "scope <region>" line), normalised, one per line; empty
# when there is none. Only names of letters, digits and '-' count: a wildcard ("nl-hhw-*") or "*" (unscoped on the
# badge) is never a scope the console may use
onair_allowed_scopes() {
    local file line name
    file=$(onair_allow_file)
    [ -r "$file" ] || return 0
    while read -r line; do
        read -r _ name _ <<<"$line"
        name=$(onair_scope_norm "$name")
        if [[ $name =~ ^[a-z0-9][a-z0-9-]*$ ]]; then printf '%s\n' "$name"; fi
    done < <(grep -iE '^[[:space:]]*scope[[:space:]]+[^[:space:]]' "$file" || true)
}

# Whether <name> is one of the allow file's scope lines
onair_scope_allowed() {
    local want list
    want=$(onair_scope_norm "$1")
    # Not "onair_allowed_scopes | grep -q": under pipefail grep's early exit makes a match on any but the last line fail
    list=$(onair_allowed_scopes)
    [ -n "$want" ] && grep -qxF -- "$want" <<<"$list"
}

# The words of a console line as the badge's parser sees them: quotes and backslashes dropped (as the shell would
# before the line reaches the badge), a leading "fw" gone. An app's prefix (#423: "fw meshcore mesh send") is not
# stripped by name: every rule below judges the words as they are AND without the first word (onair_candidates), so
# any app id in front of a gated command is refused without a list of apps. A harmless "help mesh" is judged as
# "mesh" and passes (no subcommand); "help mesh send x" or "help advert" would be judged as the on-air line they
# resemble: the guard fails closed there.
onair_words() {
    local line w1 rest
    line=$(printf '%s' "$1" | tr -d "'\"\\\\")
    read -r w1 rest <<<"$line"
    if [ "$w1" = fw ]; then
        line=$rest
        read -r w1 rest <<<"$line"
    fi
    printf '%s' "$line"
}

# The line's words, then the same without the first word: one per output line
onair_candidates() {
    local words rest
    words=$(onair_words "$1")
    read -r _ rest <<<"$words"
    printf '%s\n%s\n' "$words" "$rest"
}

# True when the line can start a flood (#290): badge.sh checks the console scope before those. With a target: send,
# dm, telemetry, trace, discover, remote, and history (switching a repeater's polls on or changing them); without one:
# "advert" (a flood advert from the console, or the Advertise setting's, which may be flood).
onair_needs_scope() {
    local c
    while IFS= read -r c; do
        onair_needs_scope_words "$c" && return 0
    done < <(onair_candidates "$1")
    return 1
}

onair_needs_scope_words() {
    local w1 w2 w3 w4 _rest
    read -r w1 w2 w3 w4 _rest <<<"$1"
    [ "$w1" = advert ] && return 0
    [ "$w1" = mesh ] && [ -n "$w3" ] || return 1
    case "$w2" in
        send | dm | telemetry | trace | discover | history) return 0 ;;
        remote) ! onair_remote_show "$w3" "$w4" ;; # "<who> settings" logs in and reads: console floods
        *) return 1 ;;
    esac
}

# "mesh remote settings" alone shows the last read and sends nothing; "settings" with more after it is a target (a
# node whose name starts with "settings"), so it is checked as one
onair_remote_show() {
    [ "$1" = settings ] && [ -z "$2" ]
}

# The console scope query for a line about to be sent, in the line's own form ("fw mesh scope" when it starts with fw)
onair_scope_query() {
    case $1 in
        fw | fw\ *) printf 'fw mesh scope' ;;
        *) printf 'mesh scope' ;;
    esac
}

# Verdict on the badge's answer to the scope query ($1): prints "pass" or "deny <reason>". The badge answers with one
# whole line "default <name|None> | console <name|not set>" (mesh_scope_status_text). Only an exact line counts, and
# exactly one: a message logged around the reply ("x: default a | console b", or a second status line) can never
# speak for the badge. No such line, or two, is a deny.
onair_scope_verdict() {
    local want have lines
    if [ -z "$(onair_allowed_scopes)" ]; then
        echo "deny no 'scope <region>' line in .claude/onair-allow.local: no console line with a target goes on air without one (#290)"
        return
    fi
    # The USB console prints every reply line behind "fw: " (#301): exactly that one prefix, never any other text
    lines=$(printf '%s\n' "$1" | tr -d '\r' | grep -E '^(fw: )?default [^ |]+ \| console ([^ |]+|not set)$' || true)
    if [ "$(printf '%s' "$lines" | grep -c '^')" != 1 ]; then
        echo "deny no single status line in the badge's answer to the scope query: not sent (#290)"
        return
    fi
    have=${lines##* | console }
    if [ "$have" != "not set" ] && onair_scope_allowed "$have"; then echo pass; else
        echo "deny the badge's console scope is '${have:-no answer}', not a scope line of .claude/onair-allow.local: fw mesh scope console <region> YES first (#290)"
    fi
}

# Verdict for one console line ("[fw ]mesh <sub> [target] ..."): prints "pass", "deny <reason>" or "ask <reason>".
onair_verdict() {
    local c v
    while IFS= read -r c; do
        v=$(onair_verdict_words "$c")
        if [ "$v" != pass ]; then
            printf '%s\n' "$v"
            return
        fi
    done < <(onair_candidates "$1")
    echo pass
}

onair_verdict_words() {
    local w1 w2 w3 w4 _rest want
    read -r w1 w2 w3 w4 _rest <<<"$1"
    if [ "$w1" = onair ]; then # #289: the badge's own allow list; only narrowing it passes
        case "$w2" in
            "" | on) echo pass ;;
            *) echo "deny 'onair ${w2}' weakens or edits the badge's on-air allow list: Lucas does that himself at the USB console (#289)" ;;
        esac
        return
    fi
    [ "$w1" = mesh ] || { echo pass; return; }
    case "$w2" in
        send)
            if [ "$w3" = "#badgetest" ]; then echo pass; else
                echo "deny on air only #badgetest: 'mesh send ${w3:-?}' would transmit to another channel (#287, bench rules)"
            fi ;;
        dm | telemetry | trace | discover | history)
            if [ -z "$w3" ] || onair_allowed "$w3"; then echo pass; else
                echo "deny on air only the test node: 'mesh $w2 $w3' targets a contact not in .claude/onair-allow.local (#287)"
            fi ;;
        chat) # #438: an open chat is one Enter from a message, so opening one is judged like a send
            case "${w3,,}" in
                open)
                    if [ "$w4" = "#badgetest" ] || onair_allowed "$w4"; then echo pass; else
                        echo "deny on air only #badgetest: 'mesh chat open ${w4:-?}' would open a chat that is not the test channel's (#438)"
                    fi ;;
                *) echo pass ;;
            esac ;;
        remote)
            if [ -z "$w3" ] || onair_remote_show "$w3" "$w4" || onair_allowed "$w3"; then echo pass; else
                echo "deny on air only the test node: 'mesh remote $w3' targets a node not in .claude/onair-allow.local (#287)"
            fi ;;
        scope)
            if [ -z "$w3" ] || { [ "$w3" = console ] && onair_scope_allowed "$w4"; }; then
                echo pass
            else
                echo "deny 'mesh scope $w3 ${w4:-?}' is not a scope line of .claude/onair-allow.local: the console scope is Lucas's to choose (#290)"
            fi ;;
        advert)
            case "$w3" in
                interval | manual) echo "ask mesh advert $w3 changes what the badge floods on air: needs Lucas (#287)" ;;
                *) echo pass ;;
            esac ;;
        *) echo pass ;;
    esac
}
