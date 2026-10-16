#!/usr/bin/env python3
"""Local stand-in for the Spotify Web API, token endpoint, LRCLIB and Wikipedia, for previewing
and measuring ignify without an account.

    python3 tools/mock_spotify.py            # serves http://127.0.0.1:8899
    IGNIFY_API=http://127.0.0.1:8899/v1 IGNIFY_ACCOUNTS=http://127.0.0.1:8899 \\
    IGNIFY_LYRICS=http://127.0.0.1:8899/lrclib IGNIFY_WIKI=http://127.0.0.1:8899/wiki ./build/ignify

Needs a refreshToken in ignify's config so it refreshes against the mock instead of opening a
browser. All data is invented, lyrics and bios included. MOCK_ALBUMS=300 grows the library.
Prints READY on the first player poll and logs every write, so tests can follow along.

Security test switches (off by default):
  MOCK_EVIL=1           HTML in playlist/track names (incl. a local <img>), and an albums "next"
                        link outside /v1 that logs any Authorization header it receives
  MOCK_FOLLOWED_ONLY=1  the user owns no playlists
  MOCK_DELAY=3          playlist and Liked Songs pages take 3 s, to see what the list cache hides
"""
import json, os, re, struct, sys, time, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

PORT = int(os.environ.get("MOCK_PORT", "8899"))
BASE = f"http://127.0.0.1:{PORT}"
N_ALBUMS = int(os.environ.get("MOCK_ALBUMS", "12"))
EVIL = os.environ.get("MOCK_EVIL") == "1"
LOCAL_IMG = "/usr/share/icons/hicolor/48x48/apps/" + (sorted(os.listdir("/usr/share/icons/hicolor/48x48/apps"))[0]
                                                      if os.path.isdir("/usr/share/icons/hicolor/48x48/apps") else "x.png")
COLORS = ["e8115b", "1db954", "509bf5", "ff6437", "8d67ab", "f59b23", "27856a", "e91429",
          "477d95", "dc148c", "b49bc8", "006450"]
ALBUMS = ["Midnight Drive", "Green Room", "Blue Hour", "Ember", "Lavender Haze", "Golden Mile",
          "Evergreen", "Red Line", "Harbour Lights", "Neon Bloom", "Soft Focus", "Deep Forest"]
ARTISTS = ["Nadia K", "The Tidelines", "Oumaima", "Static Hearts", "Lune", "Marrakech Express"]
SONGS = ["Intro", "Waves", "Afterglow", "Signals", "Paper Moon", "Run", "Closer", "Echoes", "Daylight", "Outro"]
SCOPES = ("user-read-playback-state user-modify-playback-state playlist-read-private "
          "playlist-read-collaborative user-library-read user-library-modify "
          "playlist-modify-public playlist-modify-private user-read-currently-playing "
          "user-read-recently-played user-top-read")
# Invented for the mock: (Wikipedia title → short description, extract).
WIKI = {
    "Nadia_K": ("Moroccan singer-songwriter",
                "Nadia K is a Moroccan singer-songwriter from Casablanca whose late-night electronic pop "
                "blends Darija lyrics with analog synths. Her debut album Midnight Drive was recorded in a "
                "converted garage over one summer and became a word-of-mouth favourite on streaming."),
    "The_Tidelines": ("Topics referred to by the same term", "The Tidelines may refer to several things."),
    "The_Tidelines_(band)": ("British indie rock band",
                             "The Tidelines are a British indie rock band formed in Brighton. Known for "
                             "chiming guitars and harmonies recorded live to tape."),
}
LYRICS = ["Headlights on the empty road", "Counting exits one by one", "Radio hums a song we know",
          "Chasing down the setting sun", "", "Hold the wheel and let it go", "Every mile a little lighter",
          "City fading in the rearview", "Night is long but we burn brighter", "",
          "Waves of neon, waves of rain", "Wash the day out of my head", "Turn it up and drive again",
          "Say the things we never said"]


def png(hex_a, hex_b, size=300):
    """Diagonal two-colour gradient PNG, stdlib only."""
    a, b = bytes.fromhex(hex_a), bytes.fromhex(hex_b)
    rows = b"".join(
        b"\x00" + bytes(round(a[c] + (b[c] - a[c]) * (x + y) / (2 * size)) for x in range(size) for c in range(3))
        for y in range(size))
    chunk = lambda tag, data: struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def images(i):  # distinct URL per item, so the client can't dedupe what it shouldn't
    url = f"{BASE}/img/{i}.png"
    return [{"url": url, "width": 640, "height": 640}, {"url": url, "width": 300, "height": 300},
            {"url": url, "width": 64, "height": 64}]


def artist_ref(i):
    return {"id": f"ar{i % len(ARTISTS)}", "name": ARTISTS[i % len(ARTISTS)], "uri": f"spotify:artist:ar{i % len(ARTISTS)}"}


def album(i):
    name = ALBUMS[i % 12] + (f" {i // 12 + 1}" if i >= 12 else "")
    return {"id": f"al{i}", "uri": f"spotify:album:al{i}", "name": name, "images": images(i),
            "artists": [artist_ref(i)], "release_date": f"{2014 + i % 12}-05-01", "total_tracks": 10}


def track(a, n):
    name = f"{album(a)['name']} {SONGS[n]}"
    if EVIL and (a, n) == (0, 1):
        name = '<h1 style="color:red">BIG</h1> Waves'
    return {"id": f"{a}-{n}", "uri": f"spotify:track:{a}-{n}", "name": name,
            "duration_ms": 150_000 + n * 13_000, "artists": album(a)["artists"], "album": album(a)}


def synced():
    return "\n".join(f"[{(5 + 5 * i) // 60:02d}:{(5 + 5 * i) % 60:02d}.00] {line}" for i, line in enumerate(LYRICS * 2))


COVERS = [png(COLORS[i], COLORS[(i + 5) % 12]) for i in range(12)]
DEVICES = [
    {"id": "dev-ignify", "name": "ignify", "type": "Computer", "is_active": False, "is_restricted": False,
     "supports_volume": True, "volume_percent": 60},
    {"id": "dev-phone", "name": "Iliass's iPhone", "type": "Smartphone", "is_active": True, "is_restricted": False,
     "supports_volume": False, "volume_percent": 100},
    {"id": "dev-speaker", "name": "Living Room", "type": "Speaker", "is_active": False, "is_restricted": True,
     "supports_volume": True, "volume_percent": 30},
]
PLAYLISTS = [(n, "spotify" if os.environ.get("MOCK_FOLLOWED_ONLY") == "1" else "me") for n in ["Late Night Code", "Gym 🔥", "Darija Bangers", "Focus", "Road Trip", "Chill Sunday"]]
PLAYLISTS.append(("Today's Top Hits", "spotify"))  # followed, not editable
PLAYLISTS.append(("Panic Attack 🧜‍♀️", "Zineb"))  # followed, emoji in the name
if EVIL:  # a followed playlist's name is written by someone else
    PLAYLISTS.append((f'<img src="{LOCAL_IMG}"><span style="font-size:60px;color:red">PWNED</span>', "stranger"))
LIKED = {"spotify:track:0-3"}
ADDED = {}  # playlist index -> uris added through POST, so a re-opened playlist has changed
PLAYER = {"shuffle": False, "repeat": "off", "context": "spotify:album:al0", "queue": [track(0, n) for n in range(2, 8)]}
STARTED, READY = time.time(), False


class Handler(BaseHTTPRequestHandler):
    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def empty(self, status=200):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *_):  # quiet; writes are logged explicitly
        pass

    def uris(self):
        return [u for u in unquote(parse_qs(urlparse(self.path).query).get("uris", [""])[0]).split(",") if u]

    def do_GET(self):
        global READY
        url = urlparse(self.path)
        path, q = url.path, {k: v[0] for k, v in parse_qs(url.query).items()}
        if m := re.fullmatch(r"/img/(\d+)\.png", path):
            data = COVERS[int(m[1]) % 12]
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "max-age=86400")
            self.end_headers()
            return self.wfile.write(data)
        if path.startswith("/evil/"):
            print("LEAKED", self.headers.get("Authorization"), flush=True)
            return self.send_json({"items": [], "next": None})
        if path == "/v1/me":
            return self.send_json({"id": "me", "display_name": "Iliass"})
        if path == "/v1/me/albums":
            offset, limit = int(q.get("offset", 0)), int(q.get("limit", 50))
            nxt = f"{BASE}/v1/me/albums?offset={offset + limit}&limit={limit}" if offset + limit < N_ALBUMS else None
            if EVIL and offset == 0:
                nxt = f"{BASE}/evil/steal"  # a "next" outside the API
            return self.send_json({"total": N_ALBUMS, "next": nxt, "items": [
                {"album": album(i)} for i in range(offset, min(offset + limit, N_ALBUMS))]})
        if path == "/v1/me/playlists":
            return self.send_json({"next": None, "items": [
                {"id": f"pl{i}", "uri": f"spotify:playlist:pl{i}", "name": n, "collaborative": False,
                 "owner": {"id": owner, "display_name": "Iliass" if owner == "me" else owner.title()}, "public": i % 2 == 0,
                 "items": {"total": 5 if i == 3 else 10}, "images": images(i + 3)} for i, (n, owner) in enumerate(PLAYLISTS)]})
        if m := re.fullmatch(r"/v1/playlists/pl(\d+)", path):  # playlist metadata
            i = int(m[1])
            n, owner = PLAYLISTS[i] if i < len(PLAYLISTS) else (f"New playlist {i}", "me")
            return self.send_json({"id": f"pl{i}", "uri": f"spotify:playlist:pl{i}", "name": n, "images": images(i + 3),
                                   "owner": {"id": owner, "display_name": "Iliass"}, "items": {"total": 10}})
        if m := re.fullmatch(r"/v1/albums/al(\d+)", path):
            return self.send_json(album(int(m[1])))
        if m := re.fullmatch(r"/v1/artists/ar(\d+)/albums", path):
            a, offset, limit = int(m[1]), int(q.get("offset", 0)), min(int(q.get("limit", 5)), 10)  # 10 max, like Spotify
            mine = [album(i) | {"album_type": "single" if i % 4 == 3 else "album"} for i in range(min(N_ALBUMS, 40))
                    if i % len(ARTISTS) == a]
            nxt = f"{BASE}/v1/artists/ar{a}/albums?offset={offset + limit}&limit={limit}" if offset + limit < len(mine) else None
            return self.send_json({"total": len(mine), "next": nxt, "items": mine[offset:offset + limit]})
        if path == "/v1/me/player/queue":
            return self.send_json({"currently_playing": track(0, 1), "queue": PLAYER["queue"]})
        if path == "/v1/me/player/recently-played":
            return self.send_json({"items": [{"track": track(a, a % 10), "played_at": "2026-10-16T10:00:00Z",
                                              "context": {"type": "album", "uri": f"spotify:album:al{a}"} if a % 2 else
                                              {"type": "playlist", "uri": f"spotify:playlist:pl{a % 6}"}}
                                             for a in range(1, 11)]})
        if m := re.fullmatch(r"/v1/me/top/(artists|tracks)", path):
            if m[1] == "artists":
                return self.send_json({"items": [{"id": f"ar{i}", "name": ARTISTS[i], "uri": f"spotify:artist:ar{i}",
                                                  "images": images(40 + i)} for i in range(len(ARTISTS))]})
            return self.send_json({"items": [track(i % 12, (i * 3) % 10) for i in range(10)]})
        if m := re.fullmatch(r"/v1/playlists/pl(\d+)/items", path):  # 2026: owner/collaborator only
            i = int(m[1])
            if i < len(PLAYLISTS) and PLAYLISTS[i][1] != "me":
                return self.send_json({"error": {"status": 403, "message": "Forbidden"}}, 403)
        if path == "/v1/me/tracks" or path.startswith("/v1/playlists/"):
            time.sleep(float(os.environ.get("MOCK_DELAY", 0)))  # a slow network, to see what the cache hides
            m = re.fullmatch(r"/v1/playlists/pl(\d+)/items", path)
            i = int(m[1]) if m else -1
            size = 5 if i == 3 else 10  # "Focus" is short: no scrollbar, every row on screen at once
            items = [{"track": track(n, n), "added_at": f"2025-0{n % 9 + 1}-1{n}T10:00:00Z"} for n in range(size)]
            for uri in ADDED.get(i, []):
                a, n = (uri.rsplit(":", 1)[1].split("-") + ["0"])[:2]
                items.append({"track": track(int(a), int(n)), "added_at": "2026-10-16T12:00:00Z"})
            return self.send_json({"total": len(items), "next": None, "items": items})
        if m := re.fullmatch(r"/v1/albums/al(\d+)/tracks", path):
            a = int(m[1])
            return self.send_json({"next": None, "items": [{k: v for k, v in track(a, n).items() if k != "album"}
                                                            for n in range(10)]})
        if m := re.fullmatch(r"/v1/artists/ar(\d+)", path):
            i = int(m[1])
            return self.send_json({"id": f"ar{i}", "name": ARTISTS[i], "images": images(40 + i),
                                   "genres": [["moroccan pop", "electropop"], ["indie rock"], ["alt r&b"]][i % 3]})
        if path == "/v1/search":
            offset = int(q.get("offset", 0))
            if am := re.match(r'artist:"(.+)"', q.get("q", "")):  # an artist's songs
                a = ARTISTS.index(am[1]) if am[1] in ARTISTS else 0
                songs = [track(i, n) for i in range(12) if i % len(ARTISTS) == a for n in range(10)]
                return self.send_json({"tracks": {"items": songs[offset:offset + 10]}})
            return self.send_json({"tracks": {"items": [track((offset + n) % 12, n) for n in range(10)]}})
        if path == "/v1/me/library/contains":
            return self.send_json([u in LIKED for u in self.uris()])
        if path == "/v1/me/player/devices":
            return self.send_json({"devices": DEVICES})
        if path == "/v1/me/player":
            if not READY:
                READY = True
                print("READY", flush=True)
            active = next((d for d in DEVICES if d["is_active"]), None)
            if not active:
                return self.empty(204)
            item = track(0, 1)
            return self.send_json({"is_playing": True, "device": active, "item": item,
                                   "shuffle_state": PLAYER["shuffle"], "repeat_state": PLAYER["repeat"],
                                   "context": {"type": PLAYER["context"].split(":")[1], "uri": PLAYER["context"]},
                                   "progress_ms": int((time.time() - STARTED) * 1000) % item["duration_ms"]})
        if path == "/lrclib/api/get":
            if "Waves" in q.get("track_name", ""):
                return self.send_json({"instrumental": False, "plainLyrics": "\n".join(LYRICS), "syncedLyrics": synced()})
            return self.send_json({"code": 404, "name": "TrackNotFound"}, 404)
        if path == "/lrclib/api/search":
            return self.send_json([])
        if m := re.fullmatch(r"/wiki/api/rest_v1/page/summary/(.+)", path):
            title = unquote(m[1])
            if title in WIKI:
                desc, extract = WIKI[title]
                kind = "disambiguation" if desc.startswith("Topics") else "standard"
                return self.send_json({"type": kind, "title": title.replace("_", " "), "description": desc,
                                       "extract": extract,
                                       "content_urls": {"desktop": {"page": f"https://en.wikipedia.org/wiki/{title}"}}})
            return self.send_json({"type": "https://mediawiki.org/wiki/HyperSwitch/errors/not_found"}, 404)
        self.send_json({"error": {"status": 404, "message": f"mock has no {path}"}}, 404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        if self.path == "/api/token":
            print("TOKEN", parse_qs(body).get("grant_type"), flush=True)
            return self.send_json({"access_token": "mock", "expires_in": 3600, "refresh_token": "mock-refresh",
                                   "scope": SCOPES})
        print("POST", self.path, body, flush=True)
        path = urlparse(self.path).path
        if m := re.fullmatch(r"/v1/playlists/pl(\d+)/items", path):
            ADDED.setdefault(int(m[1]), []).extend(json.loads(body).get("uris", []))
            return self.send_json({"snapshot_id": "mock"}, 201)
        if path == "/v1/me/playlists":  # create
            name = json.loads(body).get("name", "New playlist")
            PLAYLISTS.append((name, "me"))
            i = len(PLAYLISTS) - 1
            return self.send_json({"id": f"pl{i}", "uri": f"spotify:playlist:pl{i}", "name": name, "images": [],
                                   "collaborative": False, "owner": {"id": "me", "display_name": "Iliass"},
                                   "items": {"total": 0}}, 201)
        if path == "/v1/me/player/queue":
            uri = unquote(parse_qs(urlparse(self.path).query).get("uri", [""])[0])
            a, n = (uri.rsplit(":", 1)[1].split("-") + ["0"])[:2]
            PLAYER["queue"].insert(0, track(int(a), int(n)))
        self.empty(204)

    def do_PUT(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        print("PUT", unquote(self.path), body, flush=True)
        path = urlparse(self.path).path
        if path == "/v1/me/player":  # transfer playback
            target = json.loads(body)["device_ids"][0]
            for d in DEVICES:
                d["is_active"] = d["id"] == target
        if path == "/v1/me/library":
            LIKED.update(self.uris())
            return self.empty(200)
        state = parse_qs(urlparse(self.path).query).get("state", [""])[0]
        if path == "/v1/me/player/play" and body and "context_uri" in json.loads(body):
            PLAYER["context"] = json.loads(body)["context_uri"]
        if path == "/v1/me/player/shuffle":
            PLAYER["shuffle"] = state == "true"
        if path == "/v1/me/player/repeat":
            PLAYER["repeat"] = state
        self.empty(204)

    def do_DELETE(self):
        print("DELETE", unquote(self.path), flush=True)
        if urlparse(self.path).path == "/v1/me/library":
            LIKED.difference_update(self.uris())
        self.empty(200)


if __name__ == "__main__":
    print(f"mock Spotify on {BASE} ({N_ALBUMS} albums)", file=sys.stderr, flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
