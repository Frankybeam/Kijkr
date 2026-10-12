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

## What was removed from EVO Player

- Emby, Jellyfin, Stremio addons and Nuvio.
- The USB / internal-storage file browser, FTP network storage and file
  operations (copy, move, rename, delete).
- The image viewer and the text reader.

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

## Name and logo

The default name and logo live in [branding/](branding/README.md); replace them
there to rebrand.

## License

GPL-3.0-or-later, like EVO Player and ProsperoPlayer before it. See
[LICENSE](LICENSE) and [COPYRIGHT.md](COPYRIGHT.md). EVO Player's original
README is kept at [docs/EVO_PLAYER_README.md](docs/EVO_PLAYER_README.md).
