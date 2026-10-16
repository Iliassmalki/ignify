#undef NDEBUG  // keep asserts in Release builds
#include "util.h"
#include <cassert>
#include <cstdio>

int main() {
    // RFC 7636 appendix B test vector
    assert(pkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
           "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");

    assert(formatTime(0) == "0:00");
    assert(formatTime(61'999) == "1:01");
    assert(formatTime(600'000) == "10:00");

    QJsonObject bare{{"uri", "spotify:track:a"}};
    assert(unwrapTrack(bare)["uri"] == "spotify:track:a");
    assert(unwrapTrack({{"track", bare}})["uri"] == "spotify:track:a");
    assert(unwrapTrack({{"item", bare}})["uri"] == "spotify:track:a");
    assert(unwrapTrack({{"track", QJsonValue::Null}}).isEmpty());  // removed track

    // compactEntry keeps what a row shows and drops the rest
    QJsonObject full{{"added_at", "2024-05-25T10:00:00Z"},
                     {"track", QJsonObject{{"name", "Song"}, {"uri", "spotify:track:a"}, {"duration_ms", 1000},
                                           {"available_markets", QJsonArray{"MA", "FR"}},
                                           {"artists", QJsonArray{QJsonObject{{"id", "x"}, {"name", "X"}, {"href", "h"}}}},
                                           {"album", QJsonObject{{"name", "LP"}, {"images", QJsonArray{"i"}}, {"href", "h"}}}}}};
    auto small = compactEntry(full);
    auto t = unwrapTrack(small);
    assert(t["uri"] == "spotify:track:a" && t["name"] == "Song" && t["duration_ms"] == 1000);
    assert(small["added_at"] == "2024-05-25T10:00:00Z" && !t.contains("available_markets"));
    assert(joinNames(t["artists"].toArray()) == "X" && !t["artists"][0].toObject().contains("href"));
    auto album = t["album"].toObject();
    assert(album["name"] == "LP" && album["images"].toArray().size() == 1 && !album.contains("href"));
    assert(!unwrapTrack(compactEntry(QJsonObject{{"uri", "spotify:track:b"}})).contains("album"));  // album page tracks
    assert(compactEntry(full) == compactEntry(small));  // compacting twice changes nothing

    assert(joinNames(QJsonArray{QJsonObject{{"name", "A"}}, QJsonObject{{"name", "B"}}}) == "A, B");

    auto img = [](int w, const char *url) { return QJsonObject{{"width", w}, {"url", url}}; };
    QJsonArray sizes{img(640, "L"), img(300, "M"), img(64, "S")};
    assert(imageUrl(sizes, 600) == "L");
    assert(imageUrl(sizes, 168) == "M");
    assert(imageUrl(sizes, 40) == "S");
    assert(imageUrl(sizes, 1000) == "L");
    assert(imageUrl(QJsonArray{QJsonObject{{"width", QJsonValue::Null}, {"url", "X"}}}, 40) == "X");
    assert(imageUrl({}, 40).isEmpty());

    QStringList uris;
    for (int i = 0; i < 150; ++i) uris << QString("spotify:track:%1").arg(i);
    auto ctx = playBody("spotify:playlist:x", uris, 3);
    assert(ctx["context_uri"] == "spotify:playlist:x");
    assert(ctx["offset"].toObject()["uri"] == "spotify:track:3");
    auto win = playBody("", uris, 120);
    assert(!win.contains("context_uri"));
    assert(win["uris"].toArray().size() == 30);
    assert(win["uris"].toArray().first() == "spotify:track:120");
    assert(playBody("", uris, 0)["uris"].toArray().size() == 100);

    auto lrc = parseLrc("[ar: someone]\n[00:05.00] first\n\n[00:09.50]second\n[01:02.34] third\nno stamp");
    assert(lrc.size() == 3);
    assert(lrc[0].ms == 5000 && lrc[0].text == "first");
    assert(lrc[1].ms == 9500 && lrc[1].text == "second");
    assert(lrc[2].ms == 62340);  // rounds, doesn't truncate 62339.99
    assert(lineAt(lrc, 0) == -1);
    assert(lineAt(lrc, 5000) == 0);
    assert(lineAt(lrc, 9499) == 0);
    assert(lineAt(lrc, 9500) == 1);
    assert(lineAt(lrc, 999999) == 2);
    assert(lineAt({}, 1000) == -1);

    assert(looksLikeMusician("Moroccan singer-songwriter"));
    assert(looksLikeMusician("British rock band"));
    assert(looksLikeMusician("American DJ and record producer"));
    assert(!looksLikeMusician("City in Morocco"));
    assert(!looksLikeMusician("Topics referred to by the same term"));

    assert(clip("short", 10) == "short");
    assert(clip("one two three four", 9) == "one two\u2026");
    assert(clip("abcdefghij", 4) == "abcd\u2026");

    assert(nextRepeat("off") == "context" && nextRepeat("context") == "track" && nextRepeat("track") == "off");

    QJsonArray two{QJsonObject{{"id", "ar0"}, {"name", "Nadia K"}}, QJsonObject{{"id", "ar1"}, {"name", "A&B <x>"}}};
    QString links = artistLinks(two, "#B3B3B3");
    assert(links.contains("href=\"artist:ar0\"") && links.contains(">Nadia K</a>"));
    assert(links.contains("A&amp;B &lt;x&gt;"));  // names escaped
    QJsonArray hostile{QJsonObject{{"id", "x\" onclick=\"y"}, {"name", "Evil"}}};
    assert(artistLinks(hostile, "#fff") == "Evil");  // a non-base62 id never reaches an href

    assert(formatLength((3 * 3600 + 39 * 60 + 5) * 1000LL) == "3 hr 39 min");
    assert(formatLength((38 * 60 + 12) * 1000LL) == "38 min 12 sec");
    assert(formatAdded("2024-05-25T10:00:00Z") == "May 25, 2024");
    assert(formatAdded("").isEmpty() && formatAdded("not a date").isEmpty());
    assert(formatCount(1599) == "1,599");

    puts("ignify_test: ok");
}
