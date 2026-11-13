# Plan: epic #392 MeshCore app

Read-only planner, main @47dc849e, 2026-11-13. Sources: the epic body (no later comments), all 19 listed issues with comments, code under `firmware/components/mesh/`.

**Headline:** 17 of 19 issues already have their code on main and wait only for bench proof. Most of it can be proofed remotely (console, `fw keys send`, screenshots, Heltec driven through the meshcore peer session). Only #389 and #368 need new code. One small UI bug (#52's off-screen latitude error) has no issue of its own yet. This epic is mostly proofing work for main, not agent work.

## 1. Issue inventory

| # | Short title | State | Labels of note | Real status (code checked) |
|---|---|---|---|---|
| 126 | Wrong-target reply, two-chat crash | open | score 9, now, needs-proofing | On main (per-screen state, `mesh_chat_target.c` + test). Remote pass of steps 1-7, on-air proof via CoreScope. Left: steps 8-11 with a real Heltec DM. |
| 50 | Notifications for new messages | open | needs-proofing | On main (`mesh_notify*.c`). Left: real Heltec DMs, #badgetest toast, mention from the peer, chat-open skip. |
| 153 | QR screen (contact, Wi-Fi, text) | open | needs-lucas | On main (`app_qr.c`, Show as QR rows). Left: a phone camera scan and the phone-app import. |
| 52 | Advert, location, auto-add settings | open | needs-proofing | On main. Left: steps 3-5, 7-10, 12 (12 is Esc only). Finding not fixed yet: the latitude error note shows off screen (UI rule "feedback where you look"). |
| 111 | Select mode + `@[name]` mention | open | needs-lucas, needs-proofing | On main. Steps 1-3 and 6-9 passed. Left: 4, 5 and 10 (need a peer message), 11 optional. Lucas: does a phone show it as a mention? |
| 127 | List scroll jumps, reading slide | open | needs-proofing | On main. Steps 1-3 and 5 passed. Left: 4 (Heltec advert) and 6-11 (peer messages, 50+ in #badgetest). |
| 137 | MeshCore screens vs the UI standard | open | needs-proofing | On main. Most steps passed. Left: 8 (Status transmits a request), 9 (channel page), and the #117 shared steps 3-8 and 13. |
| 162 | Telemetry request | open | needs-lucas | On main (console path proven, wait/timeout OK). Left: an answer with values; the contact-page row behind a careful dialog. |
| 163 | Default flood scope | open | needs-lucas | On main, shows "None (everywhere)". Left: set and clear a scope on air. Open decision: should the Settings factory reset clear the scope? |
| 53 | Trace / discover path | open | needs-proofing | On main. Left: discover with the Heltec on, the result screens. |
| 166 | Signal history per contact | open | needs-proofing | On main. Step 1 passed. Left: step 2 (Heltec adverts fill the graph). |
| 167 | Discover nearby | open | needs-lucas | On main, already moved to the grid tile "Nearby" (`mesh_grid.c`). Left: confirming the dialog, then a live answer. |
| 168 | Repeater status history | open | (none) | On main (c5ae6dd3, 64865a51). Polling of the agreed repeater was left **on** on 2026-11-07. Left: check the spacing, the UI/R12 steps, then switch it off. |
| 171 | Contacts radar | open | needs-lucas | On main, tile "Radar". Remote pass on 11-13. Left: the no-position note (needs the position cleared, which changes adverts), dot colours on the glass, Heltec distance. |
| 291 | Per-channel flood scope | open | needs-lucas | On main (96bf709f..e9cf045d). Left: the full on-air checklist (confirm dialog, CoreScope check that the send is scoped). |
| 389 | Delivery state gaps after #47 | open | (none) | **Not started.** `mesh_chat.c:62-64` still shows "queued" for both QUEUED and SENT. No heard time or repeats in the detail. The journal already stores state, repeats and trip_ms (`mesh_hist.h:19,188-193`), so item 4 is probably covered: audit only. |
| 117 | Repeater settings page | open | needs-lucas, needs-proofing | On main. Read-only login OK since 11-13 (Lucas). Left: read steps 1-9, 11-16 by screen (remote). Lucas at the badge: the one reversible non-radio write. |
| 132 | One-off label overflow | open | needs-proofing | On main. Left: steps 1-4 and 6 (repeater login, now allowed), the rest of step 8. |
| 368 | SD format leaves the journal dead | open | bug | **Not started.** `mesh_journal.c` appends to `/sd/lichen/messages/log.bin`. After a format the directory is gone (errno 2) and `turn_off()` disables history. No reopen path exists. `lichen_sd_info_t.generation` grows on every mount, so the fix can stay inside mesh. |

## 2. Dependencies inside the epic

- #117 before #137 and #132: they share the repeater Settings steps and the login. Do all three in one read-only login session.
- #126 before the rest of the chat proofing (#50, #111, #127, #389): it is the safety bug (wrong recipient). Its steps 8-11 also exercise the notification centre that #50 needs.
- #163 before #291: a channel scope falls back to the default scope; prove the default first.
- #52 (own position set) before #171 (the no-position note and the distance check).
- #53 and #166 share Heltec adverts and discover; batch them in one Heltec session.
- #389's bench (queued, then heard repeated, then delivered) runs after #126 is closed, so a mis-target cannot spoil it.
- #368 is independent.

## 3. Cross-epic dependencies

**This epic needs:**
- **#381 tooling:** #377 `fw ui answer` (open, `now`; dialog policy #376 is merged and needs-proofing). Without it, every careful-confirm step stays manual: #162, #163, #167, #291, the #53 rows, and the #111/#117/#137 steps that confirm. #377 also widened scope to the radio "Switch radio" page under `ALLOW_ONAIR_SCOPED`. It must land and be proofed before W3.
- **#395 security:** #289, the badge-side on-air allow list for console sends. It is labelled area:meshcore but is not in this epic; confirm it is in #395. Once it is on, console bench sends go only to listed targets, so the #badgetest channel and the Heltec must be on the badge list before any remote proofing here. The hand-only rule (#374, dev taps refuse injected keys) decides which dialogs can be proofed remotely at all.
- **#216 platform:** #234, the P4 <-> C6 contract and the two-chip gate. LoRa sits behind the C6, so a contract change there resets proofing confidence for every on-air step. Do not proof MeshCore on air while a #234 wave is half-merged. Any `lichen_sys_storage` change (generation semantics) collides with #368.
- **#393 UI:** any change to the shared menu, heading or hint helpers re-touches every MeshCore screen. Run W1's #52 note fix after #393's menu changes, or on the same helper. #132 also covers non-MeshCore files (notify_centre.c, update_screen.c, time_set_screen.c, boot_screen.c, home.c); #393 should not reopen those labels without telling this epic.
- **#394 hardware/power:** sleep or screen-off work changes the #168 polling and the #50 "chat open with the screen off" rule (decided: still skipped).
- **#262 memory:** none blocking. #389 and #368 add only small RAM; both stay within the +8 KB rule.

**Others need from this epic:**
- #381 and #395 use MeshCore screens as targets for proving their policies (Discover nearby, telemetry, Reset path, Remove contact).
- #368's fix pattern (reopen on a new SD generation) is reusable by any other SD writer (#216/#262 storage work).

## 4. Decisions for Lucas and missing prerequisites

1. **#163:** should the Settings-level factory reset also clear the flood scope? (Recommended: yes, since a forgotten scope makes the badge silently unreachable.) A yes adds a small code task.
2. **#163/#291:** OK to set the agreed test region on air through `fw ui answer` under the bench scope, and which region (keep it out of the public issue)?
3. **#168:** is polling of the agreed repeater still on? Keep it or switch it off after the spacing check?
4. **#162:** set the Heltec to share telemetry with the badge (a setting on the peer), or name another node that does.
5. **#117:** a session with Lucas at the badge for the one reversible non-radio write.
6. **Hands-only checks (no remote path):** #111 (does a phone show it as a mention), #153 (phone camera scans and the import; use a dummy Wi-Fi network, never a real password), #171 (dot colours on the glass, clearing the own position).
7. **Optional tooling, instead of needs-lucas:** decode the QR from a `fw ui` screenshot on the host (a zbar-type decoder) to prove the #153 payloads remotely. That leaves only the phone-app import for Lucas.
8. **#52's latitude-error finding:** main opens it as its own issue (the rule for bugs found on the way). A planner cannot open issues.

No solder work or new hardware is needed. Every check needs the Heltec on and the repeater in range.

## 5. Waves

### E392-W0: remote proofing sweep (main only, no code)
- **Issues:** #126 (8-11), #50, #127 (4, 6-11), #111 (4, 5, 10), #53, #166 (2), #117 (read steps), #137 (8, 9, shared steps), #132 (1-4, 6, 8), #168 (state check and switch off).
- **Prerequisites:** none for the non-dialog steps. Before any console send, check that #289 (if merged) lists #badgetest and the Heltec.
- **Tasks, in sequence on the one badge:**
  - (a) chat block, Heltec driven via the meshcore session: #126, #50, #111, #127 (fill #badgetest to 50+ messages first);
  - (b) Heltec advert/discover block: #53, #166, #127 step 4;
  - (c) one read-only repeater login: #117, #137, #132;
  - (d) #168 history read, then off.
- **Acceptance:** every checklist step ticked or moved to a named later wave; issues close with evidence; `needs-lucas` comes off where nothing is left for Lucas.
- **Bench:** all remote. On air only #badgetest, the Heltec and read-only `get` to the agreed repeater. `fw crash` clean after each block.
- **Checks:** no writes to the repeater; CoreScope confirms no stray sends.
- **Files:** none. **Parallel:** none (one badge). **Model:** main.

### E392-W1: code for the two unstarted issues (+ #52 note)
- **Issues:** #389, #368, and the new #52-note issue.
- **Prerequisites:** none. It can start now, in parallel with W0, since agents do not touch the badge.
- **Tasks (3 agents, disjoint files):**
  - **T1 #389 (Sonnet):**
    - audit the labels;
    - give SENT its own label, distinct from QUEUED, in channel and DM status (`mesh_chat.c`);
    - show the heard time and repeat count in the message detail (`meshcore_chat.c`);
    - audit what the journal keeps (`mesh_hist`). If a heard time needs a new record field, stop and report: a format change becomes its own Opus issue.
    - Tests: extend `host_tests/mesh_chat_test.c` with a status-table case for every state (fails before on SENT).
  - **T2 #368 (Sonnet):**
    - the journal remembers the SD generation it opened on;
    - on a new generation (format, unmount and remount) it recreates `SD_MESSAGES_DIR` and starts a fresh `log.bin` instead of `turn_off`;
    - the decision is a pure helper with a new host test (generation same → append; new and mounted → reopen; unmounted → keep in RAM, retry).
    - Files: `mesh_journal.c` and a small pure `.c/.h`. No change to `sdcard/`.
  - **T3 #52 note (Sonnet):** show the latitude/longitude parse error next to the row, or as a toast. File: the Advert & contacts page in `meshcore_settings.c` (or its own file).
- **Conflict files:** `host_tests/CMakeLists.txt` (T2 only; T1 reuses the existing test), `firmware/components/mesh/CMakeLists.txt` (T2 only, if it adds a .c), `tools/standard-baseline.txt` (all three: merge one at a time and commit the size lines after each).
- **Acceptance:** full `firmware/build.sh build` green, new tests fail before and pass after, size within +8 KB per archive.
- **Bench (main, one OTA for the batch, remote):**
  - #389: a #badgetest send shows `queued`, then `sent`/`on air`, then `heard repeated`; a DM to the Heltec shows `delivered`; the detail shows the heard time and repeats;
  - #368: `fw sd format ERASE` with MeshCore running, then one #badgetest message → `fw mesh hist` shows "history on", 1 record, no journal write error;
  - #52 note: type `abc` into Latitude → the error is visible from the row.
- **Reviewer (one lichen-reviewer, Sonnet):**
  - #389: no new `mesh_send_*` path, no new frames;
  - #368: journal integrity (crypto header and HEAD record on the fresh file), SD begin/end batching, no file work under the mesh lock beyond today's pattern, ENOENT handling;
  - T3: UI rules R5 and R12, `fw ui overflow` 0.

### E392-W2: decision follow-ups (small, conditional)
- **Issues:** #163 (if Lucas says the factory reset clears the scope).
- **Prerequisites:** decision 1; after #393's settings or reset changes, if any, since the reset path may be in shell or settings code that #393 owns.
- **Task:** one Sonnet agent clears the default scope on the Settings reset, plus a host test of the reset item list, if one is pure.
- **Conflict files:** `tools/standard-baseline.txt`.
- **Bench:** remote. Set a scope (needs W3's dialog path), reset, then "None (everywhere)".

### E392-W3: dialog-confirm proofing (main only)
- **Issues:** #167, #162 (row and dialog), #163 (set and clear), #291 (full list), the #53 UI rows, and any confirm steps left from W0.
- **Prerequisites:** #377 merged and proofed (epic #381); decisions 2 and 4; #289 lists the targets.
- **Acceptance:** each checklist ticked. CoreScope shows the #291/#163 sends scoped and nothing further. The scope is cleared back to the agreed value and survives `fw reboot` (wait 75 s).
- **Bench:** remote via `fw ui answer`. On-air scope sets only with Lucas's ok (they change who receives the badge).
- **Files:** none. **Model:** main.

### E392-W4: hands session with Lucas (needs-lucas)
- **Issues:** #111 (phone mention render), #153 (camera scans and import; or only the import if decision 7 is built), #171 (glass colours, the no-position note), #117 (the reversible write), #126 step 5 on the phone (optional: CoreScope already proved it).
- **Prerequisites:** W0 done, so the session is short. One bench checklist comment per issue, posted by main.

## 6. Risks and what to leave out

**Risks:**
- Proofing load on one badge and one Heltec. W0 is long and serial. A dead Heltec or a weak link stalls many issues at once, so batch by peer state.
- On-air mistakes: scope sets (#163/#291) can cut the badge off its own channel. Always end on the agreed scope and check with `fw mesh scope`.
- #368 writes a fresh journal on SD. A wrong reopen could overwrite `log.bad`/`log.old` evidence, and the reviewer must check this.
- #389 creeping into a journal format change (#120/#98 history). Hard stop and a separate Opus issue.
- #289 landing mid-sweep refuses bench sends until the allow list is set.
- Half-merged #234 waves: hold on-air proofing.

**Leave out of this program:**
- A "heard by a repeater" state for DMs: the protocol offers none (#389 says so).
- #162 "the badge answers with its own sensors": not asked yet.
- #168 extensions: more nodes, longer rings.
- Wi-Fi QR scans with a real password.
- The empty-state texts of #137, which need an empty list or a forbidden chat: write "not checked".
- Location sharing in adverts (#52): it publishes a position.

Each of these would transmit something new, or needs a decision nobody has asked for.
