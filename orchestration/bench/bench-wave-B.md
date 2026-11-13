# Bench script: wave B on fw-f537a44a

Wave B = #404 (remote request argument bound), #389 heard time for channel messages, #400 (an injected key never starts a stopped MeshCore). The badge was just updated to `fw-f537a44a` with the first compressed update (#298). Goal: a brief proof on real hardware that the wave boots, runs and does what it should. Remote only. File prefix for your output: `wb-`.

## Tools (use exactly these)
- `S=<scratchpad>`
- Console line over Wi-Fi: `"$S/fw.sh" "<line>" ["<line>" ...]` (lines without the `fw ` prefix; it prints `>> line` and the redacted reply).
- Other badge.sh commands need the address: `IP=$(cat "$S/badge-ip")`, then from <repo>: `tools/badge.sh ota-info "$IP"`, `tools/badge.sh log "$IP" > "$S/wb-log-N.txt"`, `tools/badge.sh screenshot "$IP" "$S/wb-N.png"`, `tools/badge.sh ota-reboot "$IP"`.
- NEVER print `$IP`, never `cat` the badge-ip file, never paste an address, network name, host name, MAC, coordinates, contact names or message text into your report. Save raw output to `$S/wb-*.txt` (chmod 600) and quote only the lines this script asks for. Pipe anything you print through `sed -E 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ip>/g'`.
- After ANY reboot wait at least 75 s before the next console line (poll `ota-info` until it answers, then wait out the 75 s).
- Before any key: `idle test 0`, then `ui lock on 600`. When done: `ui open home`, `ui lock off`.

## Hard limits
- Allowed keys: `up down left right esc wait`, and `enter` ONLY where this script says so, always behind a focus guard (`keys send focus:<Row>| enter`). If a guard fails or a reply says a key is hand-only, stop that step and report it; do not find another way.
- On air: exactly TWO sends, both in step C. No other `mesh send`, no `dm`, `advert`, `trace`, `discover`, `telemetry`, `remote`, `history <target>`, no Enter on Advertise, Region & radio, TX power, Custom radio, Airplane, Wi-Fi off. If badge.sh or the badge refuses the send, report the refusal text; do not retry another way.
- No flashing, no `ota-firmware`, `ota-assets`, `ota-c6`, `ota-file`, no `fw sd format`, no factory reset, no `notify clear`, no power off, no `fw dev off`, no `fw bl`, no `fw psram speed`, no serial or USB console, no ssh, nothing else on the flash host.
- No edits in the repository, no git, no gh. Your only writes are files under `$S/`.
- If the badge stops answering for more than 4 minutes, or `crash` reports a new crash for this image, STOP the script and report at once with what you have.

Extra allowance for this script only: step E pushes ONE small test file with `tools/badge.sh ota-file` and removes it with `tools/badge.sh ota-rm`; nothing else may be pushed.

## A. Health
1. `tools/badge.sh ota-info "$IP"` -> starts with `badge_fw fw-f537a44a`.
2. `slot` -> running `(valid)`, other slot `fw-0746e922`. `c6` -> `IDENT match`, `up | restarts 0`. `crash` -> `this boot is clean`, no record naming `fw-f537a44a`. `airplane` -> `bluetooth off, controller idle`. `dev` -> `developer mode on`. `battery` -> absent.
3. `mem` -> `mem dma free` >= 89981, `min` >= 80650; record dma free, dma min, internal free. (Before: free 155939, min 148319, internal 182151.)
4. `mesh` -> `identity: ok`, radio line ends `(Netherlands)`; record `rx` and `tx`.

## B. #400 hand-only start
Save the log before and after (`wb-log-1.txt`, `wb-log-2.txt`).
1. `mesh stop`; `mesh` -> stopped.
2. `idle test 0`, `ui lock on 600`, `ui open home`, then `keys send triangle` (the Messages shape key; if the reply says the key name is unknown, run `keys` with no argument to read the key names, use the name for the triangle or "messages" shape key, and say which you used).
3. `mesh` -> STILL stopped. Fetch the log -> a line `injected key ignored: starting MeshCore takes a hand (or fw mesh start)` (quote it).
4. `ui open home`; read `ui`; if the focus is not the MeshCore tile, do not navigate: skip to step 5. If `focus: MeshCore`: `keys send focus:MeshCore| enter`; `mesh` -> still stopped; quote the reply to the key line and any new log line.
5. `ui open meshcore` -> opens; wait 15 s; `mesh` -> running, identity ok (the explicit console command may start it).
6. `mesh stop`, then `mesh start`, wait 15 s, `mesh` -> running.
7. With MeshCore running: `ui open home`, `keys send triangle` -> `ui` shows a MeshCore screen on the stack (an injected key may open a running MeshCore). Then `ui open home`.
8. Spectrum resume: `ui open lora_spectrum`; wait 5 s; `mesh` -> record the state (expected: paused or stopped for the spectrum); `keys send esc`; wait 20 s; `mesh` -> running again; log has `MeshCore starting again after the LoRa spectrum` (quote it). If MeshCore does NOT come back, that is a FINDING: run `mesh start` to restore it and report.
9. `ui open home`, `ui lock off`.

## C. #389 heard time (two sends to the test channel)
1. `mesh scope` -> note the console scope line. `mesh` -> note `tx`.
2. `mesh send #badgetest wave-B bench 1 <HHMM UTC>`; record the UTC second. Poll `mesh out` every 5 s for 60 s and record the state sequence for that message.
3. Chat view: `idle test 0`, `ui lock on 600`, `ui open meshcore`; `ui` must show `focus: Chat` (if not, `ui open home`, `ui lock off`, skip the screenshot and say so); `keys send focus:Chat| enter`; read `ui`; step `keys send down` one at a time until the focus is the row for `#badgetest`, NEVER pressing enter on any other row; `keys send focus:#badgetest| enter`; screenshot `wb-chat.png`; look at it: under your message the status line should read `heard repeated, <time>` or `heard repeated x<N>, <time>` with a time such as `1.8 s` or `420 ms` (quote the status text only; do not quote other messages or names). Do NOT type or press enter inside the chat. `ui open home`, `ui lock off`.
4. `mesh send #badgetest wave-B bench 2 <HHMM UTC>`; poll `mesh out` 60 s as before.
5. Persistence: `tools/badge.sh ota-reboot "$IP"`; wait 75 s; repeat step 3's navigation and screenshot (`wb-chat-after-reboot.png`) -> both messages still show a `heard repeated ...` status with a time. `ui open home`, `ui lock off`.
6. `mesh` -> `failed 0`; record `tx`.

## D. #404 (latent bounds bug; no on-air test)
The fix is proven by its host tests; nothing reaches it from the console without a transmit. Only confirm no side effect: `crash` -> clean; `mesh out` answers.

## E. #298 compressed file push
1. Make a text file of about 200 KB of compressible text under `$S/` (for example `yes 'lichen wave-B bench line 0123456789' | head -c 204800 > "$S/wb-push.txt"`), and its `sha256sum`.
2. Read `tools/badge.sh` usage for `ota-file` (the badge path looks like `'FLASH0:[dir]name'`). Push it once: `tools/badge.sh ota-file "$IP" "$S/wb-push.txt" 'FLASH0:[bench]wb-push.txt'`. Record the reply lines that give bytes sent and bytes on the wire (expected: far fewer bytes on the wire than the file size). If the path form is refused, quote the refusal and stop this step; do not try other targets.
3. `storage` -> a `bench/` entry is listed. `sd cat /storage/bench/wb-push.txt 0 32` -> `sd cat: 32 bytes at 0 of 204800` and the first text line.
4. Remove it: `tools/badge.sh ota-rm "$IP" 'FLASH0:[bench]wb-push.txt'`; `storage` -> the file is gone (an empty `bench/` entry may remain: say so).

## F. Three reboots and close
1. Three times: `tools/badge.sh ota-reboot "$IP"`, wait 75 s, then `slot`, `c6`, `crash`, `mesh` -> valid, IDENT match, clean, identity ok.
2. `mem` -> record dma free / min and internal free (compare with A3). `dev` -> on. `ui` -> stack `0`, `ui lock: off`.

## Report (to the main session, under 400 words)
A table: step, pass / fail / not run, the one figure or line asked for. Then FINDINGS with the exact reply line (redacted) and the file under `$S/` holding the full output. Then the before/after memory figures. Say plainly if a step was skipped and why. Do not interpret a failure as acceptable; report it.
