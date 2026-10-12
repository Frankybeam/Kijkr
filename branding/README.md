# Kijkr branding

Default name and logo for Kijkr. Replace any of these to rebrand.

| File | Used for |
|---|---|
| `kijkr-logo.svg` / `kijkr-logo-512.png` | The logo mark (TV with play button), transparent background |
| `kijkr-icon.svg` / `kijkr-icon-512.png` | PS5 home-screen icon (`projects/evoplayer/sce_sys/icon0.png`, 512x512, no alpha) |
| `kijkr-background-1920x1080.png` | PS5 background (`sce_sys/pic0.png` and `pic1.png`, 1920x1080, no alpha) |

In-app logo: `projects/evoplayer/assets/icons/icon_logo.png` (144x144, transparent).
It is embedded into the binary by `tools/bundle_rml_assets.py`, which
`scripts/package-app.sh` runs on every build.

## Changing the name

- Home-screen title: `titleName` in `projects/evoplayer/sce_sys/param.json`.
- Title ID: `PPSA99077` (`titleId`, `conceptId`, `contentId` in param.json,
  plus the scripts under `scripts/` and `tools/`). It differs from EVO Player's
  `PPSA99039`, so both can be installed side by side.
- In-app text: search the sources for `Kijkr` / `KIJKR`.
- Settings and portals are stored in `/data/kijkr` on the console.
