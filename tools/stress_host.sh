#!/usr/bin/env bash
# The network half of the Stress app (#278): while `fw stress <minutes>` runs on the badge, pull GET /log and
# GET /screenshot over Wi-Fi in a loop, so the C6 link's frame pools (#260), the OTA server task and lwIP carry load
# at the same time as the badge's own workers. Read-only on the badge: no pushes, no console commands.
#
# Usage: tools/stress_host.sh <host> <minutes>
#   host     the badge's address (an argument, never kept in this repo)
#   minutes  1-30, as long as the badge's run
# Environment, as tools/badge.sh:
#   BADGE_OTA_TOKEN  the update token ("fw token"; default: ${XDG_CONFIG_HOME:-~/.config}/why2025-badge/ota-token)
#   BADGE_OTA_PORT   the update server's port (default 8080)
# Exit: 0 when every request answered 200, 1 on the first that did not (its status is printed), 2 on bad usage.
set -euo pipefail

usage() {
    echo "usage: $0 <host> <minutes 1-30>" >&2
    exit 2
}

[ $# -eq 2 ] || usage
host=$1
minutes=$2
[[ $minutes =~ ^[0-9]+$ ]] && [ "$minutes" -ge 1 ] && [ "$minutes" -le 30 ] || usage
port=${BADGE_OTA_PORT:-8080}

token=${BADGE_OTA_TOKEN:-}
token_file="${XDG_CONFIG_HOME:-$HOME/.config}/why2025-badge/ota-token"
if [ -z "$token" ] && [ -r "$token_file" ]; then
    token=$(tr -d '[:space:]' < "$token_file")
fi
[[ $token =~ ^[0-9a-fA-F]{32}$ ]] || { echo "no OTA token: set BADGE_OTA_TOKEN or put it in $token_file" >&2; exit 2; }

work=$(mktemp -d)
trap 'rm -f "$work/body" "$work/hdr"; rmdir "$work" 2>/dev/null || true' EXIT

# One GET with the token on stdin (never on the command line, so it stays out of ps); prints the HTTP status
get() {
    local code
    code=$(printf 'header = "Authorization: Bearer %s"\n' "$token" |
        curl --config - -sS --connect-timeout 5 --max-time 60 -D "$work/hdr" -o "$work/body" -w '%{http_code}' \
            "http://$host:$port/$1") || true
    echo "${code:-000}"
}

since=0
logs=0
shots=0
lines=0
end=$((SECONDS + minutes * 60))
while [ $SECONDS -lt $end ]; do
    code=$(get "log?since=$since")
    [ "$code" = 200 ] || { echo "stress_host: GET /log answered $code after $logs logs, $shots screenshots" >&2; exit 1; }
    logs=$((logs + 1))
    lines=$((lines + $(grep -c 'stress: ' "$work/body" || true)))
    next=$(tr -d '\r' < "$work/hdr" | sed -n 's/^[Xx]-[Nn]ext-[Ss]eq: *\([0-9]*\).*/\1/p')
    [ -n "$next" ] && since=$next
    code=$(get screenshot)
    [ "$code" = 200 ] || { echo "stress_host: GET /screenshot answered $code after $logs logs, $shots screenshots" >&2; exit 1; }
    shots=$((shots + 1))
done
echo "stress_host: $logs logs, $shots screenshots, all 200; $lines stress lines seen"
