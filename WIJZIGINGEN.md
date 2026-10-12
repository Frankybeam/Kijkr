# Kijkr: wat er veranderd is ten opzichte van EVO Player 0.12.0

## 1.3.0: tv-gids als raster, terugkijken, meerdere accounts, eigen zendervolgorde
- **Tv-gids als raster**: touchpad op een zender (of Opties > TV guide grid) toont alle
  zenders van de lijst onder elkaar, met hun programma's langs een klok van 2 uur.
  Omhoog/omlaag kiest een zender, links/rechts een programma, L1/R1 bladert 8 zenders,
  L2/R2 schuift een uur terug of vooruit. Een rode lijn geeft "nu" aan.
  X op wat nu bezig is kijkt live, X op een eerder programma met archief kijkt terug,
  X op iets dat nog komt zegt hoe laat het begint. O sluit het raster.
- **Terugkijken (catch-up) voor Xtream**: in de tv-gids van een zender (Opties > TV guide)
  staan eerdere programma's met archief als "Catch-up - press to watch". Kijkr leest de
  tijdzone van het panel uit de gids zelf.
- **Terugkijken voor M3U**: zenders met `catchup`, `catchup-source`, `catchup-days`,
  `tvg-rec` of `timeshift` (ook als standaard in de `#EXTM3U`-regel). Ondersteund:
  default, append, shift, flussonic en xc, met de gangbare `{utc}`, `{start}`,
  `{duration}`, `${start}`-achtige plaatshouders. Werkt via het raster.
- **Meerdere Xtream-accounts**: een nieuw account toevoegen (Opties > Account settings)
  vervangt het oude niet meer. Met meer dan één account toont Xtream eerst de lijst van
  accounts; Opties op een account > "Remove this account". Het eerste account houdt
  zijn favorieten en Continue Watching; die van andere accounts openen op hun eigen account.
- **Meerdere M3U-afspeellijsten**: elke toegevoegde afspeellijst blijft bewaard (max. 8)
  en staat als lijst bovenaan M3U. Opties op een afspeellijst > "Remove this playlist".
  Opties bij M3U opent nu het menu; "Playlist & guide" brengt je naar het oude paneel.
- **Instellingen per zender in M3U**: `#EXTVLCOPT:http-user-agent`, `http-referrer`,
  `http-origin`, `http-cookie`, `#EXTHTTP:{...}`, `#KODIPROP` (stream_headers) en
  `url|User-Agent=...&Referer=...` worden meegestuurd, ook bij zappen.
- **Zenders ordenen**: Opties op een zender > "Hide this channel", "Move up/down",
  "Move to position..." en "Original order". Bij M3U en Xtream verplaatsen L2/R2 de
  zender ook. "Show hidden channels" haalt alles terug. Bewaard per lijst, op zendernaam.
- **Favoriete zenders**: Xtream en Stalker krijgen bovenaan een map "Favorite channels"
  met je favoriete zenders (driehoek op een zender), ook die weer te ordenen.
  M3U-favorieten worden nu op zendernaam bewaard, dus ze blijven als de lijst verandert.
- De tv-gids houdt 48 uur terug vast (was 6), voor het raster en terugkijken.
- Vertalingen (NL/DE/FR/ES) voor alle nieuwe teksten. 17/17 tests slagen, met nieuwe
  tests voor zendervolgorde en voor Xtream-accounts plus catch-up-adres.

## 1.2.0: verbeteringen uit evo-wijzigingen-v30
- **Zoekgeschiedenis**: Zoeken opent met je laatste 8 zoekopdrachten. Eén druk zoekt
  opnieuw; ook "New search...", "Remove one search..." en "Clear search history".
- **Nieuw instellingenblok "Home & Content"**: TMDB-sleutel en -taal, ouderlijk slot,
  **CLEAR CACHE** en **CLEAR SEARCH HISTORY**. Interface & Storage is daardoor korter
  (de tv-gid-instellingen blijven daar staan).
- **CLEAR CACHE** (twee keer drukken): portallijsten, Xtream-lijsten en TMDB-resultaten
  worden opnieuw geladen. In Kijkr wordt ook de **tv-gids** opnieuw gedownload.
- **QUIT KIJKR sluit de app echt af** en de PS5 gaat terug naar het startscherm.
  **O twee seconden ingedrukt houden op Home** doet hetzelfde. Lukt het niet, dan
  blijft het scherm donker en sluit je via de appwisselaar.
- **Stalker**: de zenderlijst wordt per portal op schijf bewaard, dus Live TV en de
  eerste zoekopdracht hoeven niet meer te wachten (16000 zenders duurden ~17 s).
- Vertalingen (NL/DE/FR/ES) voor alle nieuwe teksten, ook voor de tv-gids-instellingen
  uit 1.1.0. "EVO" in de nieuwe teksten is "Kijkr" geworden.
- De testsuite (`tests/run_tests.sh`) draait weer: de Emby-tests zijn eruit en de
  tv-gids is erbij. 15/15 tests slagen, inclusief de nieuwe zoekgeschiedenistest.

## 1.1.0: tv-gids (EPG)
- De gids van de bron zelf gaat eerst: `url-tvg` uit de M3U, `xmltv.php` van het
  Xtream-account (nieuw, Xtream had bijna geen nu/straks) en de gids van het Stalker-portal.
- Eigen gids in **Instellingen > Interface & Storage > TV GUIDE (EPG)**: een XMLTV-URL
  (`.xml` of `.xml.gz`) of een bestand op USB (`/mnt/usb0/guide.xml.gz`).
  Vult de zenders aan die de provider leeg laat.
- **TV GUIDE ORDER**: "Provider first" (standaard) of "My guide first".
- **REFRESH TV GUIDE**: alles opnieuw downloaden. Verder elke 12 uur vanzelf.
- Gzip-gidsen werken nu overal, ook bij M3U en op USB (`.xml.gz`, `.gz`).
- Zenders worden gekoppeld op gids-id (tvg-id, epg_channel_id, xmltv_id) en anders
  op naam, zonder landcode (`NL:`) en kwaliteit (`HD`, `FHD`).
- Grote gidsen worden op de achtergrond ingelezen; het menu blijft gewoon werken.

## 1.0.0: alleen IPTV, nieuwe naam

### Naam en logo
- Nieuwe naam: **Kijkr**. Nieuw logo (tv met play-knop), PS5-icoon en achtergrond.
- Eigen title-ID **PPSA99077** (EVO was PPSA99039), dus beide kunnen naast elkaar staan.
- Instellingen en portals staan nu in `/data/kijkr` op de console.
- Logo-bestanden en uitleg om naam/logo zelf te veranderen: `branding/`.

### Alleen IPTV
- In het menu: **Home, Live TV, Instellingen, Over**. Live TV opent de bronnen:
  - **M3U Playlist** (URL of .m3u op USB)
  - **Xtream Codes**
  - **Stalker / Ministra**
- Verwijderd: Emby, Jellyfin, Stremio-addons, Nuvio.
- Verwijderd: USB/interne bestandsbrowser, FTP-netwerkopslag, bestandsbeheer
  (kopiëren/verplaatsen/hernoemen/wissen), fotoviewer, tekstlezer.
- Instelling "Folders first" (sortering bestandsbrowser) weggehaald.
- Gebleven: de speler zelf (hardware-decode, HDR, ondertitels, audio,
  Continue Watching en Favorieten op Home), instellingen, back-up naar USB.

## Wat is gecontroleerd
- Alle bronbestanden compileren met de PS5 Payload SDK v0.42 (zelfde als in de
  Dockerfile) en er ontbreken geen symbolen na het verwijderen.
- Nog **niet** getest op een echte PS5: bouw met `scripts/package-app.sh --ffpfsc`
  en probeer het op de console.

## Weggelaten uit de zip
- Oude build-bestanden (`output/`, `.o`/`.d`, `EVOPlayer.elf`) en de
  achtergebleven `.rej`/`.orig` patch-restanten. Die worden opnieuw gemaakt bij het bouwen.
