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
