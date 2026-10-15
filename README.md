# ignify

A native Qt6/C++ Spotify player. The official Linux client puts a whole Chromium
(`libcef.so`, ~240 MB) behind a React UI. ignify uses ordinary Qt widgets instead,
gets its data from the official Web API, and plays audio through
[librespot](https://github.com/librespot-org/librespot).

```
ignify (Qt widgets)  ──HTTPS──▶  Spotify Web API   (library, search, playback control)
      │
      └─ spawns ─▶ librespot  ──▶ Spotify Connect device "ignify" ──▶ ALSA/PipeWire
```

Requires Spotify Premium.

## One-time setup

1. Install the build dependencies:
   `sudo apt install cmake qt6-base-dev libasound2-dev libssl-dev`
2. Install the audio engine: `cargo install librespot --locked`
3. At <https://developer.spotify.com/dashboard>, create an app:
   - Redirect URI: `http://127.0.0.1:8888/callback`
   - API: Web API
   - Copy its **Client ID**.

## Build and run

```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
./build/ignify
```

On first launch ignify asks for the Client ID and opens two browser sign-ins: one for
ignify itself and one for librespot. Both are remembered after that:

- ignify keeps its refresh token in `~/.config/ignify/ignify.conf` (file mode 0600).
- librespot keeps its credentials in `~/.cache/ignify/`.

## Using it

- **Albums**: your saved albums. Click one to see its songs.
- **Playlists**: double-click to play. Spotify only lets apps list the songs of
  playlists you own or collaborate on (2026 API rules). Playlists you follow still play.
- Double-click or press Enter on a song to play it. **Ctrl+Space** plays or pauses.
  **Ctrl+L** jumps to search.
- Without librespot, ignify works as a remote for whatever device is active.

## Lyrics, artist info, likes and playlists

- **Lyrics** (microphone button): time-synced lyrics from [LRCLIB](https://lrclib.net). The
  page takes the song's colour, the sung line turns white and scrolls into place, and
  clicking a line jumps there. Spotify's own lyrics are private API.
- **About the artist** (now-playing panel): photo and genres from the Web API, with a bio
  from Wikipedia credited on the card. ignify tries the artist's name, then "(musician)",
  "(band)" and similar titles, and only accepts a page whose description says musician.
- **Like**: the heart next to the song. Uses `PUT`/`DELETE /me/library` and
  `/me/library/contains`.
- **Add to playlists**: the `+` button, or right-click one or several songs (shift/ctrl to
  multi-select). Tick any number of your playlists, then Done. Followed playlists aren't
  offered, because Spotify only allows adding to playlists you own or collaborate on.
- First launch after updating: ignify asks once in the browser for the new permissions
  (liking, editing playlists).

## Connect (phone ↔ computer)

- **Phone → ignify:** librespot signs in as a Spotify Connect device named "ignify"
  (`--device-type computer`). It shows up in the phone app's device list, and the phone can
  play on it, pause, skip and change its volume.
- **ignify → phone:** the Connect button next to the volume slider opens the device picker.
  It shows the current device in green, then "Select another device". Picking one moves
  playback there and keeps its play/pause state.
- When you're listening on another device, a green **"Playing on …"** strip appears under
  the bar. Clicking a song plays it *on that device*, as in the official app. The volume
  slider greys out for devices that refuse remote volume.

How this was rebuilt: the official client's picker (`dwp-panel-section.js` in
`/usr/share/spotify/Apps/xpui.spa`) was analysed statically with REA. Its rows call an
internal `transferPlayback(deviceId)`, keep play state, and are disabled for restricted
devices. ignify does the same with the public Web API: `GET /me/player/devices`,
`PUT /me/player {device_ids}` (no `play` field), and `is_restricted`.

## Footprint

`python3 tools/footprint.py build/ignify 300` runs ignify headless against a 300-album mock
library and checks it against a budget. It exits 1 if anything is over, so it can gate a
change.

| measured | budget | official Spotify (same laptop) |
|---|---|---|
| binary 0.43 MB | 1 MB | 347 MB install |
| memory 38 MB headless, ~49 MB on screen (+23 MB librespot) | 100 MB | 929 MB across 10 processes, idle |
| CPU ~1% (+~2% librespot) while playing | 3% | 23% idle on the home screen |

Covers are only kept near the visible part of the grid. Loading every cover up front cost
125 MB on the same 300-album library.

## Testing without a Spotify account

`tools/mock_spotify.py` is a local stand-in for the Web API. It has fake albums with
generated covers, playlists, and three devices, the active one being a phone. It logs every
PUT/POST, so you can see the transfer and play calls arrive.

```bash
python3 tools/mock_spotify.py &
mkdir -p /tmp/ignify-mock/ignify && printf '[General]\nclientId=0123456789abcdef0123456789abcdef\nrefreshToken=mock\n' > /tmp/ignify-mock/ignify/ignify.conf
B=http://127.0.0.1:8899; XDG_CONFIG_HOME=/tmp/ignify-mock IGNIFY_API=$B/v1 IGNIFY_ACCOUNTS=$B IGNIFY_LYRICS=$B/lrclib IGNIFY_WIKI=$B/wiki ./build/ignify
```

## Known limits

- **Liked Songs**: starting a song queues it and the next 99, because Spotify has no
  playable "liked songs" context with an offset.
- **Search**: shows the top 50 results (5 pages of 10, the API maximum per page).
- **Media keys**: there is no MPRIS support yet, so media keys don't work.
- **Lyrics** depend on LRCLIB's coverage, and the **artist bio** on finding a confident Wikipedia match. Without one, the card shows photo, name and genres.
- **Phones in the picker**: a phone only appears once its Spotify app is open, same as in the official client.
- **Jam and Google Cast**: the official picker's Jam (group listening) and Cast rows are not
  rebuilt, because they use private APIs.

## Security and memory safety

Checked with an AddressSanitizer + UBSan + LeakSanitizer build (`build-asan/`) driven
through every feature against the mock, including quitting with requests in flight: no
errors and no leaks. Hostile data was replayed with `MOCK_EVIL=1` and `MOCK_FOLLOWED_ONLY=1`.

- **Ownership.** Every widget has a Qt parent or is owned by a layout or menu; popups use
  `WA_DeleteOnClose`. Async callbacks are bound to a context object (or a `QPointer` /
  `QPersistentModelIndex`), so a reply that lands after its view is gone is dropped.
- **Untrusted text** (track and playlist names, lyrics, bios, device names) is shown as plain
  text. Rich text is used only where ignify builds it, with the data escaped.
- **The access token** is only sent to the Spotify API. Pagination links pointing anywhere
  else are refused.
- **Secrets**: the refresh token lives in `~/.config/ignify/` (folder 0700, file 0600), and
  librespot's saved login in `~/.cache/ignify/` (folder 0700).
- **Sign-in**: PKCE with a `state` check, and a loopback server on 127.0.0.1 only.
- **Shutdown**: librespot gets SIGINT so it leaves Spotify Connect cleanly, and is only
  killed after 3 s.

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" && cmake --build build-asan -j
```

## Branding

ignify uses Spotify's colours, its Liked Songs gradient and the Spotify logo. The logo is
drawn in vector by `spotifyLogo()` in `src/ui.cpp`. It appears beside the wordmark as
"powered by Spotify" attribution, which Spotify's guidelines require, and it is also the
window icon. That's fine for personal use. Before publishing, replace the window icon in
`main()`: Spotify's developer terms don't allow its logo as a third-party app's own icon.
