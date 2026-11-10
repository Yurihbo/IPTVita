# IPTVita

A new, independent IPTV player for PlayStation Vita and PlayStation TV.

**Author:** Yurihbo  
**Status:** Stage 2 — UI preview (`v0.2.0`)

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
