# Kijkr: what changed compared to EVO Player 0.12.0

What is new in each Kijkr version. The release workflow puts the matching
section in the GitHub release notes, so keep the headings in the form
`## 1.3.0: title`. The same list in Dutch is in [WIJZIGINGEN.md](WIJZIGINGEN.md).
EVO Player's own history is in [CHANGELOG.md](CHANGELOG.md).

## 1.3.0: TV guide grid, catch-up, several accounts, your own channel order
- **TV guide grid**: the touchpad on a channel (or Options > TV guide grid) shows
  every channel of the list below each other, with their programmes along a
  two-hour clock. Up/Down picks a channel, Left/Right a programme, L1/R1 pages
  through 8 channels, L2/R2 moves an hour back or forward. A red line marks "now".
  X on what is on now watches live, X on an earlier programme with an archive
  plays it back, X on something still to come says when it starts. O closes the grid.
- **Catch-up for Xtream**: in a channel's TV guide (Options > TV guide), earlier
  programmes with an archive show as "Catch-up - press to watch". Kijkr reads the
  panel's time zone from the guide itself.
- **Catch-up for M3U**: channels with `catchup`, `catchup-source`, `catchup-days`,
  `tvg-rec` or `timeshift` (also as a default on the `#EXTM3U` line). Supported:
  default, append, shift, flussonic and xc, with the usual `{utc}`, `{start}`,
  `{duration}` and `${start}`-style placeholders. Works from the grid.
- **Several Xtream accounts**: adding an account (Options > Account settings) no
  longer replaces the old one. With more than one account, Xtream first shows the
  list of accounts; Options on an account > "Remove this account". The first
  account keeps its favourites and Continue Watching; those of other accounts
  open on their own account.
- **Several M3U playlists**: every playlist you add is kept (up to 8) and listed at
  the top of M3U. Options on a playlist > "Remove this playlist". Options in M3U
  now opens the menu; "Playlist & guide" takes you to the old panel.
- **Per-channel M3U settings**: `#EXTVLCOPT:http-user-agent`, `http-referrer`,
  `http-origin`, `http-cookie`, `#EXTHTTP:{...}`, `#KODIPROP` (stream_headers) and
  `url|User-Agent=...&Referer=...` are sent along with the stream.
- **Arranging channels**: Options on a channel > "Hide this channel", "Move up/down",
  "Move to position..." and "Original order". In M3U and Xtream, L2/R2 also move
  the channel. "Show hidden channels" brings everything back. Kept per list, by
  channel name.
- **Favourite channels**: Xtream and Stalker get a "Favorite channels" folder at
  the top with your favourite channels (Triangle on a channel), which you can
  arrange as well. M3U favourites are now kept by channel name, so they stay when
  the list changes.
- The TV guide keeps 48 hours of the past (was 6), for the grid and catch-up.
- Translations (NL/DE/FR/ES) for all new texts. 17/17 tests pass, with new tests
  for channel order, Xtream accounts and the catch-up address.

## 1.2.0: search history, Home & Content, clear cache, quit
- **Search history**: Search opens with your last 8 searches. One press searches
  again; also "New search...", "Remove one search..." and "Clear search history".
- **New "Home & Content" settings section**: TMDB key and language, parental lock,
  **CLEAR CACHE** and **CLEAR SEARCH HISTORY**. Interface & Storage is shorter as a
  result (the TV guide settings stay there).
- **CLEAR CACHE** (press twice): portal lists, Xtream lists and TMDB results load
  fresh. In Kijkr the **TV guide** is downloaded again too.
- **QUIT KIJKR really closes the app** and the PS5 goes back to its home screen.
  **Holding O for two seconds on Home** does the same. If the system refuses, the
  screen stays dark and you close Kijkr from the app switcher.
- **Stalker**: the channel list is kept on disk per portal, so Live TV and the
  first search no longer wait (16000 channels took about 17 s).
- Translations (NL/DE/FR/ES) for all new texts, and for the TV guide settings
  from 1.1.0. "EVO" in the new texts became "Kijkr".
- The test suite (`tests/run_tests.sh`) runs again: the Emby tests are out and
  the TV guide is in. 15/15 tests pass, including the new search history test.

## 1.1.0: TV guide (EPG)
- The source's own guide comes first: `url-tvg` from the M3U, `xmltv.php` from the
  Xtream account (new; Xtream had almost no now/next) and the Stalker portal's guide.
- Your own guide in **Settings > Interface & Storage > TV GUIDE (EPG)**: an XMLTV
  URL (`.xml` or `.xml.gz`) or a file on USB (`/mnt/usb0/guide.xml.gz`). It fills
  the channels the provider leaves empty.
- **TV GUIDE ORDER**: "Provider first" (default) or "My guide first".
- **REFRESH TV GUIDE**: download everything again. Otherwise every 12 hours.
- Gzip guides now work everywhere, M3U and USB included (`.xml.gz`, `.gz`).
- Channels are matched by guide id (tvg-id, epg_channel_id, xmltv_id) and
  otherwise by name, ignoring the country code (`NL:`) and quality (`HD`, `FHD`).
- Big guides are read in the background; the menus keep working.

## 1.0.0: IPTV only, new name

### Name and logo
- New name: **Kijkr**. New logo (a TV with a play button), PS5 icon and background.
- Own title ID **PPSA99077** (EVO was PPSA99039), so both can be installed side by side.
- Settings and portals are now kept in `/data/kijkr` on the console.
- Logo files and how to change the name and logo yourself: `branding/`.

### IPTV only
- In the menu: **Home, Live TV, Settings, About**. Live TV opens the sources:
  - **M3U Playlist** (URL or .m3u on USB)
  - **Xtream Codes**
  - **Stalker / Ministra**
- Removed: Emby, Jellyfin, Stremio addons, Nuvio.
- Removed: USB / internal file browser, FTP network storage, file operations
  (copy / move / rename / delete), image viewer, text reader.
- Removed the "Folders first" setting (file browser sorting).
- Kept: the player itself (hardware decode, HDR, subtitles, audio, Continue
  Watching and Favourites on Home), settings, backup to USB.
