#pragma once
// Pure helpers, kept apart so test.cpp can check them without a network or a window.
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

// RFC 7636 S256 code challenge for the PKCE sign-in.
inline QString pkceChallenge(const QString &verifier) {
    return QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

inline QString formatTime(qint64 ms) {
    qint64 s = ms / 1000;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

// Liked Songs wrap tracks as {"track": ...}, playlists (2026 API) as {"item": ...},
// search returns them bare.
inline QJsonObject unwrapTrack(const QJsonObject &entry) {
    if (entry.contains("uri")) return entry;
    auto t = entry["track"].toObject();
    return t.isEmpty() ? entry["item"].toObject() : t;
}

// Only what a song row needs, so a cached list stays small (no markets, links or ids of
// everything). Keeps the entry's shape: {added_at, track}.
inline QJsonObject compactEntry(const QJsonObject &entry) {
    auto t = unwrapTrack(entry);
    QJsonObject track;
    for (auto field : {"name", "uri", "duration_ms", "is_local"})
        if (t.contains(field)) track[field] = t[field];
    QJsonArray artists;
    for (const auto &v : t["artists"].toArray()) {
        auto a = v.toObject();
        artists << QJsonObject{{"id", a["id"]}, {"name", a["name"]}, {"uri", a["uri"]}};
    }
    track["artists"] = artists;
    if (t.contains("album")) {  // album tracks have none: the page knows the album
        auto album = t["album"].toObject();
        track["album"] = QJsonObject{{"name", album["name"]}, {"images", album["images"]}};
    }
    QJsonObject out{{"track", track}};
    if (entry.contains("added_at")) out["added_at"] = entry["added_at"];
    return out;
}

// In-place radix-2 FFT; a.size() must be a power of two.
inline void fft(std::vector<std::complex<float>> &a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {  // bit-reversal permutation
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float angle = -2 * float(M_PI) / float(len);
        const float sr = std::cos(angle), si = std::sin(angle);
        for (size_t i = 0; i < n; i += len) {
            float wr = 1, wi = 0;
            for (size_t k = 0; k < len / 2; ++k) {
                // Multiplied out by hand: std::complex's operator* calls a NaN-safe helper
                // (__mulsc3) per product, which made this several times slower.
                auto &x = a[i + k + len / 2];
                std::complex<float> v(x.real() * wr - x.imag() * wi, x.real() * wi + x.imag() * wr);
                auto u = a[i + k];
                a[i + k] = u + v;
                x = u - v;
                float t = wr * sr - wi * si;
                wi = wr * si + wi * sr;
                wr = t;
            }
        }
    }
}

// Spectrum levels 0..1 in `bands` log-spaced bands from lo to hi Hz, for mono samples in
// -1..1 (a power-of-two count). Tilted +3 dB per octave around 1 kHz, as music falls off by
// about that much, so the treble bars aren't always flat. -60..-10 dB maps to 0..1.
inline std::vector<float> spectrumBands(const std::vector<float> &samples, int rate, int bands,
                                        float lo = 40, float hi = 14000) {
    const size_t n = samples.size();
    std::vector<std::complex<float>> a(n);
    for (size_t i = 0; i < n; ++i)  // Hann window: no smearing from the cut edges
        a[i] = samples[i] * 0.5f * (1 - std::cos(2 * float(M_PI) * float(i) / float(n - 1)));
    fft(a);
    const float binHz = float(rate) / float(n);
    std::vector<float> out(bands);
    for (int b = 0; b < bands; ++b) {
        float f0 = lo * std::pow(hi / lo, float(b) / bands), f1 = lo * std::pow(hi / lo, float(b + 1) / bands);
        size_t i0 = std::max<size_t>(1, size_t(f0 / binHz)), i1 = std::min(n / 2 - 1, std::max(i0, size_t(f1 / binHz)));
        float peak = 0;
        for (size_t i = i0; i <= i1; ++i) peak = std::max(peak, std::abs(a[i]));
        float db = 20 * std::log10(peak / (float(n) / 4) + 1e-9f) + 3 * std::log2(std::sqrt(f0 * f1) / 1000);
        out[b] = std::clamp((db + 60) / 50, 0.f, 1.f);
    }
    return out;
}

inline QString joinNames(const QJsonArray &artists) {
    QStringList names;
    for (const auto &a : artists) names << a.toObject()["name"].toString();
    return names.join(", ");
}

// Spotify lists images largest first; take the smallest that still covers `px`
// (width is null on some playlist mosaics, so unknown counts as big enough).
inline QString imageUrl(const QJsonArray &images, int px) {
    QString best;
    for (const auto &v : images) {
        auto o = v.toObject();
        int w = o["width"].toInt();
        if (!best.isEmpty() && w != 0 && w < px) break;
        best = o["url"].toString();
    }
    return best;
}

// Body for PUT /me/player/play. Playlists and albums play as a context, so playback
// carries on past what's listed; anything else (Liked Songs, search) gets a uri window.
inline QJsonObject playBody(const QString &context, const QStringList &uris, int row) {
    if (context.startsWith("spotify:playlist:") || context.startsWith("spotify:album:"))
        return {{"context_uri", context}, {"offset", QJsonObject{{"uri", uris[row]}}}};
    // ponytail: 100-track window, Liked Songs stops after it; queue more via /me/player/queue if that bites
    return {{"uris", QJsonArray::fromStringList(uris.mid(row, 100))}};
}

struct LyricLine {
    qint64 ms;
    QString text;
};

// LRC ("[01:02.34] words") → timed lines; untimed lines are dropped.
// ponytail: one timestamp per line; "[00:12][00:45] chorus" repeats keep only the first
inline QList<LyricLine> parseLrc(const QString &lrc) {
    static const QRegularExpression stamp(R"(^\[(\d+):(\d+(?:\.\d+)?)\]\s?(.*)$)");
    QList<LyricLine> out;
    for (const auto &raw : lrc.split('\n')) {
        auto m = stamp.match(raw.trimmed());
        if (m.hasMatch())
            out.append({qRound64((m.captured(1).toInt() * 60 + m.captured(2).toDouble()) * 1000), m.captured(3)});
    }
    return out;
}

// The line being sung at `ms`; -1 before the first one.
inline int lineAt(const QList<LyricLine> &lines, qint64 ms) {
    auto it = std::upper_bound(lines.begin(), lines.end(), ms,
                               [](qint64 t, const LyricLine &l) { return t < l.ms; });
    return int(it - lines.begin()) - 1;
}

// Wikipedia's short description ("Moroccan singer-songwriter") confirms we found the musician,
// not a namesake.
inline bool looksLikeMusician(const QString &description) {
    static const QRegularExpression music(
        R"(\b(singer|songwriter|band|rapper|musician|DJ|producer|composer|group|duo|trio|vocalist|guitarist|pianist|orchestra)\b)",
        QRegularExpression::CaseInsensitiveOption);
    return music.match(description).hasMatch();
}

// Cut at a word boundary so a bio preview never ends mid-word.
inline QString clip(const QString &text, int max) {
    if (text.size() <= max) return text;
    int cut = text.lastIndexOf(' ', max);
    return text.left(cut > 0 ? cut : max) + "\u2026";
}

// Spotify's repeat button cycles off -> all (context) -> one (track) -> off.
inline QString nextRepeat(const QString &state) {
    return state == "off" ? "context" : state == "context" ? "track" : "off";
}

// "A, B" as rich-text links ("artist:<id>") for labels that open artist pages. Names are escaped;
// an id that isn't plain base62 is shown as text rather than trusted inside an href.
inline QString artistLinks(const QJsonArray &artists, const QString &color) {
    static const QRegularExpression safeId("^[A-Za-z0-9]+$");
    QStringList out;
    for (const auto &v : artists) {
        auto a = v.toObject();
        QString name = a["name"].toString().toHtmlEscaped(), id = a["id"].toString();
        out << (safeId.match(id).hasMatch()
                    ? QString("<a href=\"artist:%1\" style=\"color:%2;text-decoration:none\">%3</a>").arg(id, color, name)
                    : name);
    }
    return out.join(", ");
}

// Spotify's total-length wording: "3 hr 39 min" for long lists, "38 min 12 sec" for short ones.
inline QString formatLength(qint64 ms) {
    qint64 s = ms / 1000, h = s / 3600, m = s % 3600 / 60;
    return h ? QString("%1 hr %2 min").arg(h).arg(m) : QString("%1 min %2 sec").arg(m).arg(s % 60);
}

// "2024-05-25T10:00:00Z" -> "May 25, 2024" (English, like the rest of the UI); empty if unknown.
inline QString formatAdded(const QString &iso) {
    QDateTime t = QDateTime::fromString(iso, Qt::ISODate);
    return t.isValid() ? QLocale(QLocale::English).toString(t.date(), "MMM d, yyyy") : QString();
}

// "1,599" style counts.
inline QString formatCount(qint64 n) { return QLocale(QLocale::English).toString(n); }
