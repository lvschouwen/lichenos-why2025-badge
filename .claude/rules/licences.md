---
paths:
  - "c6/**"
  - "shared/**"
  - "**/sdkconfig.defaults"
  - "**/idf_component.yml"
  - "tools/third_party_licenses.py"
---

# Licences (moved from CLAUDE.md)

- `c6/` and `shared/lora/` are Apache-2.0 (the C6 image links Espressif blobs): no GPL or BadgeVMS code there.
- A new or newly enabled library gets an entry in `tools/third_party_licenses.py`; regenerate `THIRD_PARTY_LICENSES.txt`.
- Enable LVGL bundled extras (QR, PNG/GIF/JPEG decoders, TinyTTF, LZ4, ThorVG, FrogFS, ...) or other third-party libs only when a feature needs them; name the option and its licence (all LVGL extras are GPL-3-compatible) before adding.
