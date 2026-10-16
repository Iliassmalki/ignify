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

## Build and install

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local && cmake --build build -j && ctest --test-dir build
cmake --install build
```

This puts `ignify` in `~/.local/bin`, its launcher in your app menu, and the flame icon in
`~/.local/share/icons`, so it opens with a click like any other app. To also get a desktop
icon on Linux Mint (Nemo), copy the launcher and mark it trusted:

```bash
cp ~/.local/share/applications/ignify.desktop ~/Desktop/ && chmod +x ~/Desktop/ignify.desktop && gio set ~/Desktop/ignify.desktop metadata::trusted true
```

On first launch ignify asks for the Client ID and opens two browser sign-ins: one for
ignify itself and one for librespot. Both are remembered after that:

- ignify keeps its refresh token in `~/.config/ignify/ignify.conf` (file mode 0600).
- librespot keeps its credentials in `~/.cache/ignify/`.

## Using it

- **Home**: shortcuts to the albums and playlists you played last, your top artists and
  your top songs this month.
- **Artist pages**: click any artist name (now playing, the bar, a song row, an album
  header, Home) for their songs and discography (albums, singles and EPs).
- **Albums**: your saved albums, with filter and sort (recently added, alphabetical, artist).
- **Your Library**: search it, sort it (recents, alphabetical, creator), and create playlists.
- **Playlists and albums** are laid out like Spotify's: a header in the cover's colour,
  owner, song count and length, then play, shuffle and "…" (queue it all, add it all to a
  playlist), search in the list, and a sort (custom order, title, artist, album, date
  added, duration). Rows show the album thumbnail, artist, album, date added and length.
  Double-click a song to play; click the grey artist name to open the artist.
- **Playlists you follow**: Spotify only lets apps list the songs of playlists you own or
  collaborate on (2026 API rules), and answers anything else with 403. ignify says so on
  the page. Press play and the songs fill in from Spotify's queue as it plays.
- **Queue**: the queue button shows Now playing, "Next from" the album or playlist, and a
  Recently played tab. Right-click songs to add them to the queue.
- **Shuffle and repeat** (off, all, one) beside the transport, as in Spotify.
- **Back and forward** between pages: the arrows by the logo, or Alt+Left / Alt+Right.
- **Mini player** (Ctrl+M): a small always-on-top window with controls on hover.
- **Full screen** (F11): large cover on a blurred backdrop. Esc leaves.
- **Media keys and the desktop's sound menu** work through MPRIS (D-Bus).
- **F1** lists every keyboard shortcut. Double-click or press Enter on a song to play it;
  Space plays or pauses.
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
| binary 0.84 MB | 1 MB | 347 MB install |
| memory 47 MB headless, ~50 MB on screen (+23 MB librespot) | 100 MB | 929 MB across 10 processes, idle |
| CPU ~1% (+~2% librespot) while playing | 3% | 23% idle on the home screen |

Covers are only kept near the visible part of the grid. Loading every cover up front cost
125 MB on the same 300-album library.

## Testing without a Spotify account

`tools/mock_spotify.py` is a local stand-in for the Web API, LRCLIB and Wikipedia. It has
fake albums with generated covers, playlists, artists with discographies, a queue, listening
history, top items, and three devices, the active one being a phone. It logs every write, so
you can see play, queue, shuffle, repeat, like and playlist calls arrive.

```bash
python3 tools/mock_spotify.py &
mkdir -p /tmp/ignify-mock/ignify && printf '[General]\nclientId=0123456789abcdef0123456789abcdef\nrefreshToken=mock\n' > /tmp/ignify-mock/ignify/ignify.conf
B=http://127.0.0.1:8899; XDG_CONFIG_HOME=/tmp/ignify-mock IGNIFY_API=$B/v1 IGNIFY_ACCOUNTS=$B IGNIFY_LYRICS=$B/lrclib IGNIFY_WIKI=$B/wiki ./build/ignify
```

## Known limits

- **Liked Songs**: starting a song queues it and the next 99, because Spotify has no
  playable "liked songs" context with an offset.
- **Search**: shows the top 50 results (5 pages of 10, the API maximum per page).
- **Artist "Popular" songs**: Spotify removed the top-tracks endpoint in 2026, so artist
  pages list the artist's songs from search, labelled "Songs".
- **Queue**: songs in the queue can't be jumped to or removed; the public API only reads it
  and adds to it.
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

ignify uses Spotify's colours and Liked Songs gradient. The Spotify logo, drawn in vector by
`spotifyLogo()` in `src/ui.cpp`, appears only beside the wordmark as "powered by Spotify"
attribution, which Spotify's guidelines require. The window icon is ignify's own mark, a green
flame drawn by `appIcon()`.
