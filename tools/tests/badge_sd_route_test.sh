#!/usr/bin/env bash
# Host test (#595): ota-firmware goes through the SD card's update folder when /info says sd0-stream and sd0-card (no
# /prepare, the image then its .sha256 to SD0:[lichen.update]), and the old way (/prepare, /firmware) without a card.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/badge-sd-route-test.XXXXXX")
srv=
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -rf -- "$work"' EXIT
cat > "$work/stub.py" <<'P'
import http.server, sys
card, log = sys.argv[1] == "card", sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def reply(self, text):
        body = (text + "\n").encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
    def note(self, data=b""):
        with open(log, "a") as f:
            f.write("%s %s %s %s\n" % (self.command, self.path, self.headers.get("X-Path", "-"), data.decode(errors="replace").strip()))
    def do_GET(self):
        self.reply("badge_fw fw-test; receiver: deflate chunks zpieces sd0-stream%s prepare; hw {}" % (" sd0-card" if card else ""))
    def do_POST(self):
        self.note(); self.reply("ready")
    def do_PUT(self):
        data = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.note(data if self.headers.get("X-Path", "").endswith(".sha256") else b""); self.reply("ok")
    def log_message(self, *a): pass
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
P
head -c 4000 /dev/urandom > "$work/badge_fw.bin"
sha=$(sha256sum "$work/badge_fw.bin" | cut -d' ' -f1)
run() { # run <card|none>: the requests ota-firmware --no-reboot made, one per line
    : > "$work/log"
    python3 -I "$work/stub.py" "$1" "$work/log" > "$work/port" &
    srv=$!
    for _ in $(seq 50); do [ -s "$work/port" ] && break; sleep 0.1; done
    # Never the bench's config: no host or token file is read (XDG_CONFIG_HOME is the empty work dir)
    XDG_CONFIG_HOME=$work BADGE_HOST=127.0.0.1 BADGE_OTA_PORT=$(cat "$work/port") BADGE_OTA_TOKEN=$(printf 'ab%.0s' $(seq 16)) BADGE_OTA_COMPRESS=0 \
        BADGE_FIRMWARE_BIN="$work/badge_fw.bin" timeout 30 bash "$here/../badge.sh" ota-firmware 127.0.0.1 --no-reboot \
        > "$work/out" 2>&1 || { echo "FAIL: ota-firmware ($1) exited $?: $(cat "$work/out")" >&2; exit 1; }
    kill $srv; wait $srv 2>/dev/null; srv=
    rm -f -- "$work/port"
}
run card
want="PUT /file SD0:[lichen.update]badge_fw.bin 
PUT /file SD0:[lichen.update]badge_fw.bin.sha256 $sha  badge_fw.bin"
[ "$(cat "$work/log")" = "$want" ] || { echo "FAIL: with a card, got: $(cat "$work/log")" >&2; exit 1; }
run none
if ! grep -q '^POST /prepare' "$work/log" || ! grep -q '^PUT /firmware' "$work/log" || grep -q 'SD0:' "$work/log"; then
    echo "FAIL: without a card, got: $(cat "$work/log")" >&2
    exit 1
fi
echo "badge_sd_route_test: ok"
