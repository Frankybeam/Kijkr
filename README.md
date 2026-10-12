# Kijkr

**An IPTV player for jailbroken PlayStation 5: M3U playlists, Xtream Codes and Stalker portals.**

![Kijkr](branding/kijkr-icon-512.png)

Kijkr is an IPTV-only fork of [EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5)
0.12.0 by sainsaji. It keeps EVO's hardware video decode (H.264, HEVC, VP9 up
to 4K HDR), the GPU-drawn interface and the player, and drops everything that
is not live TV.

## What it plays

- **M3U / M3U8 playlists**: by URL, or an `.m3u` file on a USB stick
  (`iptv.m3u`, `playlist.m3u` or `channels.m3u` in the root is found
  automatically). XMLTV guide (NOW / NEXT).
- **Xtream Codes**: server, user name and password. Live, films and series.
- **Stalker / Ministra portals**: portal URL and MAC address.

Open **Live TV** from the side menu, pick the source type and fill it in.

## TV guide (EPG)

Each source's own guide is used first: the playlist's `url-tvg` (M3U), the
account's `xmltv.php` (Xtream Codes) or the portal's guide (Stalker).

Your own guide goes in **Settings -> Interface & Storage -> TV GUIDE (EPG)**:
an XMLTV address (`http://.../guide.xml` or `.xml.gz`) or a file on USB
(`/mnt/usb0/guide.xml.gz`). It fills the channels the provider's guide leaves
empty; **TV GUIDE ORDER** can put it first instead. Guides refresh every 12
hours, or straight away with **REFRESH TV GUIDE**. Channels are matched by
guide id (tvg-id, epg_channel_id, xmltv_id) and otherwise by name, ignoring
country prefixes like `NL:` and quality tags like `HD`.

## Search, cache and quitting

- **Search** opens with your last 8 searches: one press searches again, or
  pick "New search...". Remove one or clear them all from the same list, or
  from **Settings -> Home & Content**.
- **Settings -> Home & Content** holds the TMDB key and language, the
  parental PIN and **CLEAR CACHE** (press twice): portal lists, the TV guide
  and Home's TMDB results load fresh.
- **QUIT KIJKR** (Settings -> System) closes the app and the PS5 goes back to
  its home screen. **Holding O for two seconds on Home** does the same. Where
  the system refuses, close Kijkr from the switcher.
- Stalker portals keep their channel list on disk, so Live TV and search have
  it at once on the next start.

## TV guide grid, catch-up and arranging channels (1.3.0)

- **Touchpad on a channel** (or Options -> TV guide grid) opens the grid: the
  list's channels against a two-hour clock. Up/Down picks a channel,
  Left/Right a programme, L1/R1 pages, L2/R2 moves an hour. X watches what is
  on now, or plays a past programme back when the channel has catch-up.
- **Catch-up** for Xtream (channels with an archive; also in a channel's own
  TV guide) and for M3U channels with `catchup` / `catchup-source` /
  `catchup-days` / `tvg-rec` / `timeshift` (default, append, shift,
  flussonic and xc).
- **Several Xtream accounts and M3U playlists**: adding one keeps the others;
  with more than one the provider opens on the list. Options on an entry
  removes it.
- **Per-channel M3U headers**: `#EXTVLCOPT` user-agent / referrer / origin /
  cookie, `#EXTHTTP`, `#KODIPROP` stream headers and `url|User-Agent=...`.
- **Your own channel order**: Options on a channel hides it or moves it (L2 /
  R2 on M3U and Xtream); Show hidden channels brings them back. Xtream and
  Stalker get a Favorite channels folder at the top.

## What is different from EVO Player

Kijkr starts from EVO Player 0.12.0. In short: everything that is not live TV
or IPTV films and series went out, and a lot was added around IPTV.

### Added

**Sources**
- **Stalker / Ministra portals** (portal URL + MAC), with the channel list
  kept on disk so Live TV opens at once. EVO had no Stalker support.
- **Several Xtream Codes accounts and several M3U playlists** side by side
  (EVO kept one of each).
- **Per-channel M3U headers**: `#EXTVLCOPT` user-agent / referrer / origin /
  cookie, `#EXTHTTP`, `#KODIPROP` and `url|User-Agent=...`.

**TV guide**
- The account's own guide for Xtream (`xmltv.php`) and the portal's guide for
  Stalker, next to the M3U `url-tvg` EVO already read.
- **Your own XMLTV guide** (URL or file on USB), gzip guides everywhere, a
  guide order setting, refresh every 12 hours and 48 hours of history.
- **TV guide grid**: channels against a two-hour clock.
- **Catch-up**: watch past programmes back on Xtream channels with an archive
  and on M3U channels with `catchup` / `timeshift` tags.

**Channels and Home**
- **Your own channel order**: hide channels, move them, put them back.
- **Favorite channels** folder for Xtream and Stalker; M3U favourites kept by
  channel name.
- **TMDB rows on Home** (Trending, Popular films, Popular series), with your
  own free TMDB key. Only titles your IPTV provider actually has are shown, and
  a tile plays straight from the provider.
- **Continue Watching** knows series and lets you remove an entry.
- **Cleaner titles**: provider tags like `NL -`, `|NL| 4K`, `MULTI` and
  symbols the console cannot draw are taken off the displayed name.
- **Search history**: your last 8 searches.

**Subtitles**
- **OpenSubtitles.com**: search and download subtitles for what is playing
  (your own API key and account, in Settings -> Subtitles).

**Languages**
- The interface in **English, Nederlands, Deutsch, Français and Español**
  (EVO was English only). A `lang_<code>.txt` file in `/data/kijkr` adds or
  corrects translations without a new build.

**Settings and system**
- **Parental lock**: a PIN for chosen categories, or automatically for adult
  categories by name; locked content stays out of search and Home.
- **Home & Content** settings section with TMDB, parental lock, Clear cache
  and Clear search history.
- **Quit Kijkr** really closes the app (or hold O for two seconds on Home).
- Own name, logo, title ID (`PPSA99077`) and data folder (`/data/kijkr`), so
  Kijkr installs next to EVO Player.

### Removed

- **Emby, Jellyfin, Stremio addons and Nuvio**, with their screens and LAN
  discovery.
- **The file browser** for USB and internal storage, recent files and the
  file favourites screen.
- **FTP network storage** and **file operations** (copy, move, rename,
  delete, new folder).
- **The image viewer and the text reader.**
- The "Folders first" sorting setting that only the file browser used.

### Kept from EVO Player

The player itself is unchanged: hardware decode (H.264, HEVC, VP9, AV1 up to
4K 10-bit), HDR10 / HLG and Dolby Vision profile 5, audio passthrough and
5.1 / 7.1, the Surround Sound Studio, FSR and Anime4K upscaling, 120 Hz and
motion smoothing, subtitles (ASS / SRT / WebVTT, dual subtitles, auto-sync),
typing from your phone, Xtream films and series, and backup to USB.

The full list per version, in Dutch, is in [WIJZIGINGEN.md](WIJZIGINGEN.md).

## Installation

Kijkr is a game-category app module with title ID **`PPSA99077`**, so it
installs next to EVO Player (`PPSA99039`) without replacing it.

1. Build `PPSA99077.ffpfsc` (see below) or take it from a release.
2. Copy it to `/data/homebrew/` on the console over FTP.
3. Launch **Kijkr** from the Games row (ShadowMountPlus mounts it).

Settings and portals are kept in `/data/kijkr` on the console.

## Building

Same toolchain as EVO Player (PS5 Payload SDK in Docker). Clone with the
submodule:

```bash
git clone --recursive https://github.com/Frankybeam/Kijkr.git
cd Kijkr
docker compose run --rm ps5-dev ./scripts/package-app.sh --ffpfsc
docker compose run --rm ps5-dev ./scripts/deploy-app.sh --ffpfsc
```

The build applies Kijkr's small change to the `ps5-homebrew-ui` submodule
(`tools/patches/ps5-homebrew-ui-text-filter.patch`, needed for the
translations) by itself.

Full build docs: [docs/build/building.md](docs/build/building.md). The rest of
`docs/` is EVO Player's developer documentation and still uses its name.

## Changes per version

[WIJZIGINGEN.md](WIJZIGINGEN.md) lists what changed in each Kijkr version
(in Dutch). A release is made by pushing a tag that matches
`projects/evoplayer/VERSION`, e.g. `git tag v1.3.0 && git push origin v1.3.0`.

## License

GPL-3.0-or-later, like EVO Player and ProsperoPlayer before it. See
[LICENSE](LICENSE) and [COPYRIGHT.md](COPYRIGHT.md). EVO Player's original
README is kept at [docs/EVO_PLAYER_README.md](docs/EVO_PLAYER_README.md).
