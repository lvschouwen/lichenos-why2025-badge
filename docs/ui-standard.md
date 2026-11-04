# UI standard

Every screen (OS and apps) is checked against these rules. Panel 720x720, keyboard only, THEME_PAD 36, status bar 56, LARGE 40 px (~27 chars/line), SMALL 24 px (~14 px/char), menu row content ~600 px. Origin: a repeater settings heading that clipped a 32-char node name to "Settings: NL-HHW-..." (#131), hiding the name behind "..." is a safety net, not a design.

1. **Headings name the subject.** A page about one thing: small dim kind line ("Repeater settings") + the subject's
   name LARGE on its own line, wrapping to 2 lines, "..." only past that. Never "Kind: name" on one line. Fixed pages
   ("Display") = one LARGE line.
2. **Never lose what identifies or matters.** Variable text goes through `lichen_label_line`/`lichen_label_wrap`
   (lichen_ui.h), never a bare label (hard clip). Names, values, commands the user confirms: wrap or give them a line;
   "..." is for secondary text (list previews, long paths) only.
3. **Rows: what left, state right.** Label = what (short, sentence case), value = current state, dim, right; the value
   always stays visible (label capped ~68%). A row that opens a page and a row that acts at once must look different;
   anything destructive, on air or hard to undo asks first (careful dialog).
4. **Values are human.** Units and sensible precision ("869.618 MHz, 62.5 kHz, SF7, CR5", "22 dBm", "3 min ago"),
   on/off for booleans, local time; never raw floats, protocol strings, enum numbers or hex unless it is a key/id the
   user copies.
5. **Waits show what and how far, and end.** "Reading 3 of 17", a bar or a count; it ends in a final state (done,
   failed + why); no "..." left standing after the work is done. Errors in plain words + what to do next.
6. **Keys are predictable.** Arrows choose, Enter opens/acts, Esc goes back one level (clears a search/draft first).
   Focus always visible (amber border), lands on the first useful item, survives a refresh. Non-obvious keys get a dim
   hint line at the bottom.
7. **Layout is calm.** Edges THEME_PAD, nothing under the status bar, one heading size (LARGE) and one body size
   (SMALL), dim for secondary, scroll only when it does not fit, no jumps when values update.
8. **Colour has one meaning each.** Amber = focus/attention, teal = OK/live, red = failed/down. Never colour alone
   (add a word or icon); screenshots cannot show panel colour bugs (#106), so colour is checked by Lucas on the glass.
9. **One word per thing.** Same action, same word everywhere (Restart, not Reboot here and Restart there); same unit
   format; MeshCore terms as the MeshCore apps use them.
10. **Every key and transition shows something.** A visible response at once; no blank or garbage screen longer than
    ~0.5 s (the "Restarting..." frame, #134); long work runs off the LVGL task.
11. **Empty states explain.** An empty list says what would be there and how to get it ("No contacts yet: they appear
    when a node adverts. Send advert: Settings > ...").
12. **Verified with long data.** `fw ui overflow` = 0 on each screen with max-length names (32-char node, 31-char
    channel, 127-char file); one screenshot per changed screen; `tools/ui_overflow_sweep.py` for the sweep.

**Conventions (R9; #136, OKd by Lucas 2026-11-02 "wording is fine"):** Restart (never Reboot in UI; repeater CLI token
stays "reboot"), "Restarting...", On/Off, hints "Enter: open • Del: delete" (dim, pinned, no "Esc: back" lines),
confirms "Esc: cancel" + "(cancels in N s)", "3 min ago"/"3 h" (space before unit), 45%, °C, radio "869.618 MHz,
62.5 kHz, SF7, CR5", SD card, sentence case everywhere ("Node ID", "Keyboard test"), "..." only for ongoing work or a
row that asks for input ("Command..."), Wi-Fi radio / LoRa radio.

**Shared widgets that enforce it (#135):** `lichen_heading(kind, name)` / `menu_create_named`, row kinds
(OPEN "›", CYCLE, TOGGLE, DANGER -> careful dialog, INFO not focusable), `shell_page_hint`, `menu_status`
(begin/progress/done/fail), `menu_empty`, focus keys. Prefer these over local code; a screen that needs something they
cannot do -> extend the widget.

**Audit 2026-11-02 baseline:** 3 Sonnet Explore agents (OS screens / MeshCore / widgets+wording) found the gaps filed
as #135 (widgets), #136 (wording), #137 (MeshCore), #138 (OS). Repeat that split for later audits.

**How to apply:** design new screens against 1-12; reviews and audit agents cite rule numbers; a violated rule is fixed in the shared widget (menu.c, text_entry.c, dialog.c) first, local code second. Audits: 3 read-only Sonnet agents (OS screens / MeshCore / widgets+wording); the 2026-11-02 baseline produced #135-#138.
