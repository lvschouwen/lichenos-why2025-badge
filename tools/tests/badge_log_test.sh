#!/usr/bin/env bash
# Host test (#151): badge.sh log without --follow drains every /log page, not just the first.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/badge-log-test.XXXXXX")
trap 'kill $srv 2>/dev/null; rm -f "$work"/*; rmdir "$work"' EXIT
cat > "$work/stub.py" <<'P'
import http.server, sys
LINES = ["line %04d %s\n" % (i, "x" * 40) for i in range(1500)]  # ~74 KB: several 24 KB pages
CAP = 24000
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.headers.get("Authorization") != "Bearer " + "ab" * 16:
            self.send_response(401); self.end_headers(); return
        since = int(self.path.split("since=")[1])
        out, n = b"", since
        while n < len(LINES) and len(out) + len(LINES[n]) <= CAP:
            out += LINES[n].encode(); n += 1
        self.send_response(200)
        self.send_header("X-Next-Seq", str(n))
        self.send_header("Content-Length", str(len(out)))
        self.end_headers(); self.wfile.write(out)
    def log_message(self, *a): pass
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
P
python3 -I "$work/stub.py" > "$work/port" &
srv=$!
for _ in $(seq 50); do [ -s "$work/port" ] && break; sleep 0.1; done
port=$(cat "$work/port")
got=$(BADGE_OTA_PORT=$port BADGE_OTA_TOKEN=$(printf 'ab%.0s' $(seq 16)) timeout 30 bash "$here/../badge.sh" log 127.0.0.1 2>&1); rc=$?
want=$(for i in $(seq 0 1499); do printf 'line %04d %s\n' "$i" "$(printf 'x%.0s' $(seq 40))"; done)
[ "$rc" = 0 ] || { echo "FAIL: exit $rc: ${got:0:200}" >&2; exit 1; }
[ "$got" = "$want" ] || { echo "FAIL: got $(wc -l <<<"$got") lines, want 1500 in order" >&2; exit 1; }
echo "badge_log_test: ok"
