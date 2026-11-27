# GUI inspiration: small, playful hacker devices and badges

Method note: a handful of web searches (Flipper, Playdate, EMF Tildagon) confirmed URLs and some facts. The rest is from background knowledge and is marked "(unverified)" where I am less sure. Examples are ranked by usefulness to us, not by fame.

## Examples

### 1. Flipper Zero
https://docs.flipperzero.one/basics/dolphin , https://domusweb.it/en/news/gallery/2024/01/22/flipper-zero-is-a-celebration-of-the-hacker-visual-culture.html
A pixel-art dolphin pet lives in the idle screen and levels up through three stages as you use the device. The art director describes it as playful and Tamagotchi-like. Animations are plain files on the SD card, and a large community ships replacement "asset packs" (https://github.com/ClaraCrazy/Flipper-Xtreme/wiki/Asset-Packs). It is 128x64 monochrome, so the character comes from art, not colour.
Works badly: a HackerNoon critique (https://hackernoon.com/lite/my-ux-improvements-for-flipper-zero) says the UI has no consistent design system and you re-learn each screen. There is also no way to hide rarely used items.
Borrow: a companion mascot on the idle/home screen whose state reflects real use, with swappable art packs from the SD card.

### 2. Playdate
https://sdk.play.date/designing-for-playdate , https://play.date/dev/catalog/checklist/
The launcher is a grid of big, hand-made animated cards, one per app. Panic asks that each card be distinctive enough to identify the app. The system menu swipes in from the side with a fast angled animation. The crank is used for scrubbing and scrolling, and docs insist on a button fallback. Typography is chunky and friendly, with a pixel font used consistently (Whodunit/Asheville, unverified).
Works badly: the crank is a charming novelty but slow for long lists.
Borrow: large animated app cards (the launcher as a "poster wall") plus one fast, signature transition used everywhere.

### 3. Teenage Engineering (OP-1, EP-133, TP-7)
https://teenage.engineering/products/op-1 (unverified path; the site is the reference)
The OP-1 screen uses tiny line-art illustrations: each mode (synth, drum, tape) is a small animated icon-diagram, not a text menu. Colour is used as a code (four flat hues, one per function). The EP-133 uses a very small set of glyphs, a monospace-ish font and high-contrast orange on white or black. The TP-7 turns a tape-reel image into the UI itself.
Works badly: icon-only navigation has a learning cost, and TE gets away with it because there are few functions.
Borrow: one illustrated "diagram" icon per app, and a strict colour code: one hue means one thing (radio, files, settings).

### 4. Pwnagotchi
https://pwnagotchi.ai/
An e-ink face (ASCII/emoticon-style: `(⌐■_■)`, `(☉_☉ )`) reacts to what the device is doing, such as bored, excited, lonely or sad, with a short text line ("Hello, I'm Pwnagotchi"). The personality is in the *voice*: short, jokey status strings tied to real events. Cheap to build.
Works badly: it is the whole product, so it would nag if shown constantly. e-ink refresh limits motion (not our issue).
Borrow: a face plus one-line quips driven by real events (mesh peer found, low battery, SD mounted). Keep it in one corner, never in modals.

### 5. M5Stack Cardputer firmwares (Launcher, Bruce)
https://github.com/bmorcelli/Launcher , https://github.com/pr3y/Bruce
Launcher shows a big centred icon carousel with the app name underneath and a pixel/bitmap font. Bruce uses a tile menu with one coloured icon per tool and a mascot-style boot splash. The keyboard-first flow (letters jump to items) feels fast and "hacker".
Works badly: both have inconsistent screen layouts and very small text on 240x135 (their constraint, not ours). Many themes are just recolours.
Borrow: carousel launcher with big icons, and type-to-jump from the physical keyboard.

### 6. LilyGo T-Deck / Meshtastic UI
https://github.com/meshtastic/firmware (device-ui, LVGL) , https://github.com/Xinyuan-LilyGO/T-Deck
Meshtastic's LVGL "device-ui" has a bottom tab bar, message bubbles with node colours and a map tab. It is the closest relative to our badge (LVGL, touch plus keyboard, mesh). It is usable, but also fairly "app-like and officy", which is the same problem the owner has.
Works badly: dense settings lists, small tap targets, themes that are just light and dark.
Borrow: what *not* to copy. Do take the per-node colour/avatar idea for chat identity.

### 7. EMF Tildagon and card10 (camp badges)
https://tildagon.badge.emfcamp.org/ , https://github.com/emfcamp/badge-2024-software , https://card10.badge.events.ccc.de/ (unverified)
Tildagon is a round display driven by six outer buttons. The menu is a radial ring that matches the hardware, with an app store. card10 has a small coloured OLED, rainbow LEDs and a "personal state" feature showing your status (a physical LED colour). Both lean on LEDs and the physical form factor, not on screen chrome.
Works badly: Tildagon's early firmware had missing settings and a fiddly Wi-Fi flow, a reminder that charm does not excuse a clumsy basics path.
Borrow: use the badge LEDs and a "status colour" to extend the UI beyond the panel, and shape the menu to the hardware (our square panel: a grid, not a list).

### 8. DEF CON and Supercon badges
https://hackaday.com/tag/supercon/ , https://defcon.org/html/links/dc-badges.html (unverified)
These badges carry personality through puzzles, hidden menus and easter eggs: a Konami-style code, a secret mode, a unique-per-badge name or avatar. Supercon badges have often been a playable gadget with a game or synth first. The UI is often crude, but the *discovery* is the feature.
Works badly: puzzle gating on core functions frustrates people who just want to use it.
Borrow: easter eggs only in non-critical places (about screen, long-press the logo, key sequences), plus a per-badge avatar.

### 9. Cyberdecks, CRT and terminal UIs
https://github.com/Swordfish90/cool-retro-term , https://www.reddit.com/r/cyberDeck/
Phosphor-green or amber on black, scanlines, slight glow, monospace type, boot text that scrolls, and ASCII borders. This signals "hacker" instantly and is cheap on a 720x720 panel. cool-retro-term shows the full effect set (curvature, flicker, burn-in).
Works badly: flicker, blur and curvature hurt legibility and cost frames. Full terminal chrome gets tiring in long reading (messages).
Borrow: a boot sequence, a monospace display face for headings and numbers, and a subtle scanline/glow option, not a whole-screen shader.

### 10. Tamagotchi-style companions and Pico-8/Pixel UI themes
https://en.wikipedia.org/wiki/Tamagotchi , https://www.lexaloffle.com/pico-8.php
A small creature with needs, feeding, an idle loop of 2-4 frames, a limited palette (Pico-8's fixed 16 colours) and chunky pixels. The constraint produces a recognisable look more than detail does. Sound blips on every action (Playdate and Tamagotchi both do this) reinforce it.
Works badly: needs-based pets create guilt and chores; people turn them off.
Borrow: a fixed, small palette used consistently, and a pet with no needs, only moods.

## What tends to go wrong (summary)
- Motion on every transition: tiring after the hundredth time. Keep animation under about 150 ms for repeated actions and save the long ones (boot, level-up) for rare events.
- Mascot as obstacle: anything modal or blocking gets disabled by users.
- Skin-only change: a recolour of the same list rows still looks office-like. The layout, the type and the icons must change, not just the palette.
- Gating basics behind puzzles or novel input.
- Full-screen effects (CRT shaders) costing frames and legibility.

## Top 5 ideas for our badge, ranked
1. **Poster-wall launcher.** A grid of large illustrated app cards (or a carousel) in place of list rows, one flat hue per app family. Biggest visible change, uses the square panel, and keeps navigation clear. (Playdate, TE, Cardputer Launcher)
2. **A companion mascot with moods and one-line quips tied to real events** (mesh peer seen, SD mounted, battery low). Lives in a corner or on the home screen, never blocks. Art swappable from the SD card. (Flipper, Pwnagotchi)
3. **Typography with character.** Pair a pixel or monospace display face for headings, numbers and status with a plain readable body face. Cheap, and the largest single step away from "office". (Playdate, TE, CRT terminals)
4. **One signature transition plus tiny sound blips.** A single fast slide or "wipe" used everywhere, short confirm/error blips, and a boot sequence shown only on cold start. Everything else stays instant. (Playdate system menu, Tamagotchi)
5. **Hidden depth.** Easter eggs in the about screen and logo long-press, a per-badge avatar and name, and use of the LEDs as a status colour. Never in core flows. (DEF CON, card10, Tildagon)

Also useful, as a guard rail: type-to-jump in lists from the physical keyboard (Cardputer) keeps the playful UI fast.

## Sources
- https://docs.flipperzero.one/basics/dolphin
- https://domusweb.it/en/news/gallery/2024/01/22/flipper-zero-is-a-celebration-of-the-hacker-visual-culture.html
- https://hackernoon.com/lite/my-ux-improvements-for-flipper-zero
- https://sdk.play.date/designing-for-playdate
- https://play.date/dev/catalog/checklist/
- https://tildagon.badge.emfcamp.org/
- https://github.com/emfcamp/badge-2024-software
- Others listed inline are from background knowledge and were not fetched.
