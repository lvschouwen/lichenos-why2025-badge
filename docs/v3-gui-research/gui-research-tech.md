# GUI tech research: what makes a characterful UI feasible on ESP32-P4

Legend: [S] = sourced from a search result this session. [I] = inferred or from my background knowledge, not verified this session. Search tooling returned few hard numbers; gaps are stated.

## 1. Toolkits and renderers

| Option | no_std / port effort | Licence | Evidence on ESP32 |
|---|---|---|---|
| **Slint** (software renderer) | Bare-metal no_std with a global allocator; renders whole frame or line-by-line [S] https://docs.slint.dev/latest/docs/slint/guide/platforms/embedded/ , https://slint.dev/blog/porting-slint-to-microcontrollers | **GPL-3.0 OR Slint Royalty-free OR Slint commercial; royalty-free tier excludes embedded, which needs a commercial licence** [S] https://components.espressif.com/components/slint/slint/versions/1.16.1/license . Not Apache-compatible for shipping. | Tested on ESP32-S3 boards (480x480 board example) [S] https://docs.slint.dev/latest/docs/cpp/mcu/esp-idf/ ; no P4 or fps numbers found. |
| **embedded-graphics** | Native no_std, draw-target model [S] https://github.com/embedded-graphics/embedded-graphics | MIT OR Apache-2.0 [I] | Primitive-level only: no compositing, scaling or AA. Fine as the base for a bitmap-font/sprite layer, a poor fit for an effects-heavy UI. |
| **embedded-gui / guillotine / mirui** (small no_std widget crates on e-g) | no_std, no-alloc [S] https://docs.rs/crate/embedded-gui/latest | Check each [I] | Young, little adoption; no fps evidence. embedded-gui has fixed-capacity animation primitives [S]. |
| **tiny-skia** | `std` feature can be disabled, `no-std-float` feature exists [S] https://docs.rs/crate/tiny-skia/latest | BSD-3-Clause [S] | AA vector paths, gradients, pixmap blends, RGBA8 only [I]. No embedded fps data. CPU cost on a 360 MHz core at 720x720 is the concern [I]. |
| **vello_cpu** | needs `libm` feature, `std` off; alpha [S] https://docs.rs/crate/vello_cpu/0.1.0/source/README.md | Apache-2.0 OR MIT [S] | Aimed at devices with no or weak GPU [S]; sparse-strip design, likely SIMD-heavy and alloc-heavy, unproven on RISC-V MCU [I]. |
| **egui** | Not run no_std in anything I found [S: no result]. Needs std/alloc and a float-heavy mesh tessellator [I]. | MIT OR Apache-2.0 [I] | Not a fit. |
| **LVGL via bindings** | C library; Rust bindings (lvgl-rs) exist [I] | MIT [I] | Espressif reports 55 fps RGB565 / 50 fps RGB888 on P4 at 1024x600 with LVGL 9.4 benchmark [S] https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/lcd-application-note/overview.html . Our own measurement: 15 fps on a canvas vs 53 fps direct video path (task brief). Runs as C inside an isolated app is awkward. |

Immediate versus retained: for a compositor with a framebuffer per app, immediate-mode drawing into the app's own buffer with app-side dirty tracking is the simplest; a retained scene graph (Slint, LVGL) buys damage tracking automatically but brings an allocator, a layout engine and (for Slint) a licence problem [I].

## 2. Fonts

- Rasteriser: **fontdue** is a pure-Rust no_std rasteriser/layout crate [S] https://github.com/not-fl3/fontdue (MIT OR Apache-2.0 [I]). Caveat: it uses unsafe in the rasteriser and needs alloc [S/I]. ab_glyph no_std status unverified [S: no result]. Best pattern: rasterise glyph atlases at build time on the host and ship bitmaps, so no rasteriser runs on the badge [I].
- Bitmap fonts (zero runtime cost, strongest character):
  - **Cozette**, MIT, BDF/PSF/OTB [S] https://github.com/slavfox/Cozette
  - **Spleen**, BSD-2-Clause, many sizes [S] https://cgit-dev.freebsd.org/src/plain/contrib/spleen/README.md
  - **Terminus**, SIL OFL 1.1 [S, single aggregator listing: https://github.com/retro-vault/font-vault ; verify upstream]
  - GNU Unifont: licences conflict between sources (GPL with font exception vs OFL) [S]; avoid unless the exception is checked.
  - Public Pixel / itch.io pixel fonts: licences vary per font; check each [S] https://opengameart.org/content/public-pixel-font
- Distinctive typography on a 720x720 panel [I]: render at 2x or 3x integer scale of a small pixel font (crisp, chunky, cheap), or pre-render an anti-aliased display font to an 8-bit alpha atlas for headings. Alpha-only glyphs (A8) blend cheaply.

## 3. Cheap effects on this hardware

- **Bandwidth is the budget.** 720x720 RGB565 = 1.04 MB per frame; RGB888 = 1.55 MB. At ~96 MB/s PSRAM read (task brief) a full-frame read costs ~11 ms (RGB565) / ~16 ms (RGB888) [I, arithmetic]. DSI scan-out also reads PSRAM, and PPA is slowed by concurrent PSRAM traffic [S] https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32p4/api-reference/peripherals/ppa.html . So RGB565 and dirty rectangles are the main wins.
- **RGB565 vs RGB888:** Espressif's own numbers show 55 vs 50 fps and less RAM for RGB565 [S, LCD application note]. Dither (ordered Bayer, cheap) to hide banding in gradients [I].
- **Indexed/palette colour:** 8-bit indexed sprites and tiles quarter the read bandwidth versus RGB888 and enable palette cycling and fade effects with no pixel rewrite [I]. Expand to RGB565 through a 256-entry LUT during blit.
- **Dirty rectangles and tiles:** compositor tracks per-app damage and only re-composes those rects; apps with sprites/tiles update small areas [I]. Slint and LVGL both implement this internally [S for Slint line-by-line].
- **PPA:** does scale/rotate/mirror (SRM), alpha blend and fill; bilinear scaling can soften edges [S] https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32p4/api-reference/peripherals/ppa.html . LVGL 9.4 exposes it as a draw unit: Espressif claims up to 9x in fill-heavy cases, LVGL docs say ~30% average render-time saving [S] https://preview-developer.espressif.com/pr597/blog/2025/11/lvgl-plus-pixel-processing-accelerator , https://lvgl.io/docs/open/integration/chip_vendors/espressif/hardware_accelerator_ppa.mdx . Useful for the compositor: per-window alpha blend onto the scanout buffer, fades, window scale-in animations, rotate. Not benchmarked at 720x720 by anyone I found [S: no result]; measure on the badge.
- **Scrolling:** avoid moving pixels; scroll by changing a scanout/blit source offset or compose two strips per frame [I]. 2D-DMA memcpy strips are an option; unverified here.
- **CRT/scanline looks:** darken every other line during compose with a precomputed LUT or a PPA alpha overlay of a 1xN scanline mask; vignette as a pre-baked alpha image [I]. Cost is one more blend pass, so apply only when budget allows.
- **Animation:** easing (cubic, back, elastic) as fixed-point LUTs; sprite-sheet frame stepping; per-property tweens [I]. Animate transform/alpha at compositor level (PPA) so apps need not redraw.

## 4. Polished animated UIs on ESP32-class chips

- Espressif's LVGL benchmark/demo path on P4 hits 50-55 fps at 1024x600 [S, link above]. The Slint ESP32-S3 workshop and board examples show animated UIs on 480x480 [S] https://preview-developer.espressif.com/pr495/workshops/slint-esp32-workshop . I found no source detailing the techniques of specific third-party polished UIs; the commonly cited tricks are double-buffered full-frame DMA, partial refresh, RGB565 and pre-rendered assets [I].
- Our own data point: direct video path 53 fps vs LVGL canvas 15 fps (task brief), consistent with per-pixel toolkit overhead dominating, not bus bandwidth [I].

## 5. Recommendation: candidate stacks

All stacks assume the compositor gives each app an RGB565 (or indexed-8) framebuffer plus damage rects, and does alpha/scale/fade of whole windows with PPA.

**A. Own thin immediate-mode layer + pre-baked assets (recommended).** embedded-graphics (MIT/Apache) or a small custom blitter for sprites, tiles, 9-slice panels, A8 font atlases generated at build time from OFL/MIT pixel fonts (Cozette, Spleen, Terminus). Optional fontdue on host only. Evidence: bandwidth arithmetic and Espressif's RGB565 numbers; the 53 fps direct path shows this class of renderer reaches high fps [task brief]. Fit: excellent; each app draws into its own framebuffer with no allocator-heavy dependency, small U-mode footprint. Cost: we write widgets and animation helpers ourselves.

**B. tiny-skia (BSD-3) or vello_cpu (Apache/MIT) for vector content, on top of stack A.** Use only for a few hero elements (gradients, rounded shapes, smooth icons), rendered once into cached sprites rather than per frame. Evidence: no_std flags exist [S] but no embedded fps data; measure before committing. Fit: good as a build-time or occasional runtime asset renderer, risky as a per-frame renderer on 360 MHz.

**C. LVGL (MIT) kept as an optional app toolkit.** Proven 50-55 fps on P4 with PPA [S], MIT. Fit: weaker; canvas path gave 15 fps for us, C in isolated U-mode apps needs a port, and it limits distinctive pixel-art character. Keep as a fallback for form-style apps.

**Avoid:** Slint for shipped code unless a commercial licence is bought (royalty-free tier excludes embedded; GPL is incompatible with Apache-2.0 distribution) [S]. egui: no no_std evidence.

Open measurements to take on the badge before locking in: PPA blend/SRM throughput at 720x720 under DSI scan-out load; RGB565 vs indexed-8 blit rate from PSRAM; 2D-DMA strip copy rate.
