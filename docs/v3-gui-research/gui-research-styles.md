# GUI style directions for a 720x720 hacker-camp badge (revision 2, web-checked 2026-11-27)

Tags: **[S]** sourced, with the URL it came from. **[I]** inferred or my own judgement, no source found. **[S-snip]** only seen in a search-result snippet, the page itself was not opened.
Cost notes assume ESP32-P4 + LVGL 9.6 with a PSRAM framebuffer, all [I].
Method: 14 web searches, 13 pages fetched and read (listed at the end).

## 0. Biggest finding: muOS already has 720x720 themes
[S] The muOS theme docs list `720x720` as a supported resolution folder. Themes are folders of TrueType fonts, SVG glyphs, wallpapers or looping mp4 backgrounds, sounds, and layered `.ini` scheme files (global, then default, then per-screen, then selected alternative, then user override). Per-screen names include `muxlaunch` (main menu). https://muos.dev/themes/structure
[S-snip] The "Aesthetic" theme maker added a grid home-screen next to list view. https://community.muos.dev/t/aesthetic-create-themes-directly-on-your-handheld/195
[I] muOS's 720x720 target is probably the square handhelds; I did not confirm which device. Either way, community 720x720 launcher themes are the closest existing visual references to this badge, plus the layered scheme-file idea (a theme = data, not code) is worth copying. Browse https://github.com/GrumpyGopher/theme and https://github.com/VagueParade/theme (from search, not opened) for screenshots.

## 1. TUI / ncurses with colour (btop, lazygit)
- Refs: https://github.com/aristocratos/btop (loaded; has a `themes/` dir and a Themes section [S]), https://raw.githubusercontent.com/jesseduffield/lazygit/master/docs/Config.md (loaded).
- [S] lazygit's theme is a small set of semantic slots: `activeBorderColor`, `inactiveBorderColor`, `searchingActiveBorderColor`, `selectedLineBgColor`, with separate dark/light overrides. Default `scrollOffMargin` is 2 (keeps 2 rows visible past the cursor). Focus = coloured border on the active pane + background bar on the selected line.
- [S-snip] gitui uses a `theme.ron` with named colours; a Rust TUI lib offers "16 ANSI names" vs fixed dark palette modes.
- Type [I]: one monospace on a cell grid, e.g. 12x24 px cells = 60x30 grid. Palette [I]: Tokyo Night / Gruvbox / Catppuccin style, one reverse-video focus bar.
- Home [I]: titled panes, status line, app list with hotkey letters, preview pane, key-hint footer.
- Cost [I]: lowest (solid fills, bitmap mono, no alpha).
- Risks [I]: reads as "terminal"; 24 px rows are small touch targets. btop/lazygit are mouse-light, so touch needs bigger rows (use 48+ px) in touch mode.

## 2. Dot-matrix (Nothing OS, NDot, Doto)
- Refs: https://www.fontsinuse.com/typefaces/233367/ndot (loaded), https://nothing.community/d/104-ndot57-the-nothing-typeface (search), https://fonts.google.com/specimen/Doto (loaded but JS-only, no content readable).
- [S] NDot: "Designed in 2021 for exclusive use by tech company Nothing" (fontsinuse). Proprietary: no official download; community found it as "ndot57" by reading the nothing.tech source [S-snip, nothing.community].
- [S-snip] Nothing OS 3.0 replaced NDot with Ntype 82 in many places after user complaints; NDot returned only as an experimental option. So even the source brand has moved away from dot type for body UI, which supports "headings and numbers only".
- CORRECTION: I could NOT confirm "Doto" as a real font from the search or the page (page body empty). Treat Doto as unverified; verify licence (OFL?) before using. Alternative open dot fonts: not checked.
- Cost/risks [I]: a dot font is just a font; low cost. Do not copy NDot or Nothing layouts (trademark). Dot fonts illegible below about 28-32 px.

## 3. XMB (PSP/PS3)
- Refs: https://en.wikipedia.org/wiki/XrossMediaBar (loaded), https://docs.libretro.com/guides/xmb/ (search; has a background-animation option [S-snip]).
- [S] Horizontal row of category icons; selecting one shows items above and below; "navigation moves the icons, instead of a cursor". Won a 2006 Technology and Engineering Emmy.
- [S-snip] PSP background "waves" effect was changed in system software 4.20.
- NOT FOUND: any published easing or timing for XMB. My earlier timings were guesses. [I] Use 200-300 ms ease-out.
- Cost/risks [I]: full-screen animated gradient is the expensive part; the crosspoint layout itself is cheap and d-pad/keyboard native. Risk: reads as Sony; strongly "dated" per forum critique [S-snip].

## 4. Retro desktops (Mac, Haiku, Amiga, SerenityOS)
- Refs: https://haiku-os.org/blog/pulkomandy/2010-06-20_evolution_user_interfaces_haiku_and_other_operating_systems (loaded), https://liliputing.com/serenityos-offers-a-unix-like-experience-with-90s-computing-vibes/ (search).
- [S] Haiku blog: Amiga Workbench and GEM borrowed the top-of-screen menu bar from the Mac; Amiga used per-application "screens"; GEM ran apps full screen. Haiku "still really near to the old BeOS".
- [S-snip] SerenityOS mixes classic Windows, Mac and NeXT elements and makes no attempt at touchscreens, "classic mouse and keyboard by design".
- [I] Everything else (1-bit art, zoom-rect animation, yellow Haiku tabs) is my recollection; only "subtle yellow gradients" appears in a forum snippet.
- Cost [I]: very low. Risk [I]: window metaphor fits badly on 3.5-4"; draw originals, do not reuse Apple/Commodore assets. Amiga "screens" (one full-screen app each) is the useful idea.

## 5. Handheld CFW themes (muOS, Onion, MinUI) + Playdate/Pebble small-screen lessons
- Refs: muOS (see section 0), https://github.com/OnionUI/Themes (search), https://sdk.play.date/designing-for-playdate (loaded), https://developer.repebble.com/guides/design-and-interaction/recommended (loaded).
- [S] Pebble guidance: "larger fonts for the most important data, consider size 28 for larger items and a minimum of 18 for smaller", use colour for meaning (green = done), "use animations to embody the layout with character and flourish", and "ensure the UI gives direct feedback to input". Those sizes are for a 144x168 screen, so scale them by pixel density, not literally [I].
- [S] Playdate: 400x240 at 173 ppi, pixels "quite small for the physical size", so size graphics and fonts for a pocket screen; launcher card art should uniquely identify each app [S-snip].
- [S-snip] Playdate's launcher later added a compact list mode beside the big banner view. Aesthetic added grid beside list for muOS. Both support offering a list and a grid mode.
- Cost [I]: low; sliding pill and carousel are translate-only.
- Risk [I]: list rows are what the badge already has; the character comes from big tiles, per-app colour, and sound.

## 6. Synthwave / y2k / Frutiger Aero
- Refs: https://en.wikipedia.org/wiki/Frutiger_Aero (loaded), https://trends.daisyui.com/trend/synthwave-outrun/ and https://chrislemke.github.io/website_designs/examples/Synthwave.html (search).
- [S] Frutiger Aero: mid-2000s to early 2010s, glossy and skeuomorphic with nature imagery, bright colours; Windows Aero was among the first; later replaced by flat design; the Wii is cited as an example.
- [S-snip] Synthwave: neon on dark, gradient skies, chrome type, "sincere 1980s nostalgia, no irony".
- Cost [I]: highest of the set (gradients, glow, alpha). Cheap versions: pre-rendered glow sprites and tiled gradient strips. Risk [I]: tacky; poor for dense text. Launcher-only.

## 7. Brutalist / neo-brutalist, Light Phone, Rabbit R1
- Refs: https://www.nngroup.com/articles/neobrutalism/ (loaded), https://trends.daisyui.com/trend/neubrutalism/ (loaded), https://www.rabbit.tech/newsroom/introducing-r1 (search), https://www.engadget.com/light-unveils-a-new-minimalist-phone-with-a-black-and-white-oled-screen-163225012.html (search).
- [S] NN/g: high contrast, blocky layouts, bold colours, thick borders; "solid single-colour shadows (e.g. a black drop shadow offset by 4px)"; limit palette to 2-3 bold colours; padding of "24-32px" around dense blocks; keep type hierarchy (headlines about 2x body); pair a chunky headline face with a neutral body face (Roboto or Inter); check contrast (avoid yellow on cyan); interactive state via underline or colour shift.
- [S] daisyUI: black and off-white plus one to three saturated accents, no gradients or blur; "lift elements by increasing their hard shadow, press controls by translating them into the shadow, abrupt slides, stamps or tilts with short direct timing"; radius 0 to 0.25rem; suggested faces Syne, Archivo Black, Space Grotesk. Borders 2-4 px [S-snip].
- NN/g warns it can overwhelm or hurt accessibility if not balanced.
- [S-snip] Light Phone III: 3.92" black-and-white OLED, matte glass, dedicated brightness wheel; typography not documented, so my earlier type claims for it are dropped. Light Phone is a minimalist, not brutalist, example; use only for "monochrome with huge type".
- [S-snip] Rabbit R1: orange, co-designed with Teenage Engineering, 2.88" touchscreen, scroll wheel, pixel-bunny avatar; reviewers called RabbitOS navigation "incredibly linear". Lesson [I]: character is in hardware and mascot, flat navigation hurts.
- Cost [I]: lowest. The "press translates into its shadow" interaction is two moves of a rectangle, ideal for LVGL.
- Risk [I]: whole UI in colour blocks may tire; keep 2-3 accents only.

## 8. Game-UI juice: what was actually found
- Refs: https://web.dev/articles/choosing-the-right-easing (loaded), Juice it or Lose It GDC 2012, https://gdcvault.com/play/1016789/Juice-It-or-Lose (search; video youtu.be/Fy0aCDmgnxg is cited in a paper, not opened), https://learn.microsoft.com/en-us/windows/apps/design/motion/timing-and-easing (search), https://api.arcade.academy/en/latest/programming_guide/gui/animations.html (HTTP 429, only seen as snippet).
- [S] web.dev: use ease-out for UI; Quintic ease-out is a good fast default; **ease-out and ease-in 200-500 ms; bounce/elastic 800-1200 ms**, use bounces "sparingly".
- [S-snip] Microsoft Fluent: 83 ms (faster), 167 ms (fast), 250 ms (normal) control durations. Toyota design system: smaller elements animate faster, larger or farther need more time. Arcade docs: hover pop of 0.15 s back-out to 108 % scale. Animatix docs: menu slide-in 0.25 s ease-out-back, items staggered 0.1 s apart. A macOS-menu preset opens in about 80 ms. One guide groups 100-150 ms feedback, 200-300 ms state change, 300-500 ms layout.
- [S-snip] Juice it or Lose It: layer flash, shake, particles, floating text and sound on a plain Breakout clone. A counter-talk "Don't Juice It or Lose It" (GDC Europe 2014) warns about losing immersion.
- CONTRADICTED: my earlier "10 % overshoot in 120-220 ms" has no source and conflicts with web.dev (overshoot/bounce needs longer to settle). Use: ease-out-quint 150-250 ms for moves, a small 105-108 % scale pop over about 150 ms [S-snip arcade], bounce only for rare celebratory moments at 800 ms+.
- CONTRADICTED: my "20-30 ms stagger" is unsourced; the one source found uses 100 ms. [I] On a 720 px list, 40-60 ms may be enough; tune on device.
- NOT FOUND: concrete menu sound design numbers (tick lengths, pitch rules). Those 20-60 ms ticks are [I].
- Practical [S-snip, gamineai guide]: keep a small set of duration/easing tokens (fast, medium, slow) and a debug slow-motion switch while tuning.

## Cross-cutting cost notes (all [I])
- 720x720 at 16 bpp is about 1 MB per frame; full-screen alpha blends are the expensive part. Prefer opaque sprites, pre-rendered glow, and moving small areas.
- Two font sizes is the main reason it feels flat. Give 4-5 sizes of one display face plus one body face; Pebble's guidance (28 big / 18 min) shows the hierarchy idea [S].
- One accent colour per app, used for header and focus, is nearly free.

## Recommended directions (updated)
1. **Neo-brutalist tiles/bars with game-style focus (7 + 8).** Now better backed: NN/g and daisyUI give concrete rules (4 px hard shadow, 2-3 colours, 24-32 px padding, press-into-shadow). Per-app accent, big type, ease-out 150-250 ms, small scale pop, tick sound. Keep contrast checked.
2. **Colour TUI for dense screens (1).** lazygit's slot model (active/inactive border, selected-line background) is a ready recipe; use a shared theme-file approach like muOS schemes so palettes are swappable data.
3. **Dot-matrix as accent only (2), with a licence check.** Nothing itself demoted NDot in OS 3.0, which supports limiting it to clock/numbers/headings. Doto not confirmed; pick a font whose licence is verified.
Also take from muOS: a list and a grid launcher mode, theme as data (scheme files + fonts + SVG glyphs + sounds), and study existing 720x720 muOS themes.
Skip: full XMB wave background and Frutiger/synthwave glow (cost, tackiness, no timing data), retro window metaphors.

## Pages actually fetched and read (13)
muos.dev/themes/structure; nngroup.com/articles/neobrutalism; trends.daisyui.com/trend/neubrutalism; web.dev choosing-the-right-easing (md); en.wikipedia XrossMediaBar; en.wikipedia Frutiger_Aero; github.com/aristocratos/btop; lazygit docs/Config.md (raw); haiku-os.org evolution-of-user-interfaces post; developer.repebble.com recommended guidelines; sdk.play.date designing-for-playdate; fontsinuse.com NDot; fonts.google.com Doto (loaded, empty). Failed: api.arcade.academy (429). Three web-fetch subagent runs returned nothing readable to me, so their pages are not counted.
