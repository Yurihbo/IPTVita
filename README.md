# IPTVita

A new, independent IPTV player for PlayStation Vita and PlayStation TV.

**Author:** Yurihbo  
**Status:** Stage 3 — local source manager (`v0.3.0`)

IPTVita is being developed from new source code in C with VitaSDK. It does not include the source code or assets of Vita IPTV. The other application is used as a functional and technical reference only.

## Stage 1

- Original dark-blue user interface, optimized for the Vita's 960 × 544 display; home, sources, settings and about screens with three themes.
- D-pad navigation, Cross/Circle, L/R page switching, START exit confirmation.
- Placeholder entries for M3U/M3U8 URLs, Xtream Codes, local M3U files and direct stream links.
- Independent application identifier `IPTVI0001` and data directory `ux0:data/IPTVita/` (reserved for later stages).
- Automated GitHub Actions build publishing `IPTVita.vpk` as an artifact.

**Important:** Stage 2 adds navigable menus and themes, but it is **not a media player yet**. Playback, HTTPS/HLS AES-128, Pluto TV playlists, buffering and caching will be developed and tested separately in later stages. No playlist URLs or user credentials are bundled.

## Build

Requires [VitaSDK](https://vitasdk.org/) with the Vita 2D library (`vita2d`).

```sh
mkdir build && cd build
cmake ..
make -j4
```

Output: `build/IPTVita.vpk`.

On GitHub, open **Actions → Build IPTVita VPK → latest successful run → Artifacts → IPTVita-vpk**, extract the ZIP and install the VPK through VitaShell.

## Roadmap

1. Foundation and first executable VPK.
2. Complete menus and visual styling.
3. Playlist sources and local storage.
4. Networking, HLS, TLS, AES-128.
5. Hardware playback and audio.
6. Buffering and performance.
7. Favorites, EPG, media-library features.
8. Stability tests and release.

## Copyright

Copyright (c) 2026 Yurihbo. Licensed under the MIT License — see [LICENSE](LICENSE).

Third-party components added in future stages will keep their own notices and licenses.

## Stage 3 source manager (v0.3)

The **Sources** screen offers four source types: M3U/M3U8 URL, Xtream Codes, direct stream URL and local M3U path. Use **X** on a type to create an entry, or **SELECT** to toggle the saved-source list. In the saved list, **X** edits and **Triangle** deletes an entry. The application supports up to 16 entries and URLs up to 4095 characters.

The stage-3 editor is a lightweight, controller-operated character grid:
- D-pad: choose character; X: append; Triangle: backspace
- L/R: switch fields (Name, URL/Path, and Xtream username/password)
- Square: save; Circle: cancel

Entries are stored locally in `ux0:data/IPTVita/sources.bin`. No passwords are uploaded by this stage, but Xtream passwords are stored **unencrypted on the memory card**; do not use sensitive passwords. Local file entries are stored as paths, not opened yet. No playlists are fetched or parsed, and **no video playback is included** in stage 3.

### Planned favorites behavior
When video playback is implemented, **START while watching a channel** will toggle its favorite status. Each playlist will have its own **Favorites** tab, with favorites stored by source identity and channel identity. START in menus retains the exit confirmation; playback will have its own controller context. This is planned, not yet implemented.

## Stage 3.1 usability adjustments

- Triangle saves the source editor; Square erases a character. In the saved-source list, Square deletes the selected entry.
- The controller character grid now supports touch selection, left analog stick and auto-repeat while holding the D-pad or analog direction.
- Smaller, lighter pixel text in headings, menus and source cards.
- The Vita's native on-screen IME keyboard is planned as a further improvement; the current build retains the touch/controller grid. The source list is not yet a channel browser: playback and playlist parsing belong to Stage 4.
