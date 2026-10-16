#pragma once
// Visual layer: theme tokens, vector glyphs, cover helpers, image cache, album card delegate.
#include <QCache>
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QNetworkAccessManager>
#include <QPixmap>
#include <QPointer>
#include <QStyledItemDelegate>
#include <functional>

class QApplication;

namespace theme {
// Spotify's dark theme.
inline const QColor black("#000000"), base("#121212"), elevated("#1F1F1F"), highlight("#2A2A2A"),
    white("#FFFFFF"), subdued("#B3B3B3"), green("#1DB954"), greenHover("#1ED760");
void apply(QApplication &app);  // style, palette, font, stylesheet
}

namespace Role {
enum { Kind = Qt::UserRole, Uri, Id, Subtitle, Json, Image, Loaded, Order };
}

enum class Glyph { Play, Pause, Prev, Next, Heart, HeartOutline, Plus, Check, Grid, Volume, Devices, Computer, Phone,
                   Speaker, Mic, Shuffle, Repeat, RepeatOne, Queue, MiniPlayer, FullScreen, Home, Back, Forward,
                   Search, Sort, Close, Clock, More };
QIcon glyphIcon(Glyph g, const QColor &color, int size = 24);
void paintGlyph(QPainter &p, Glyph g, const QColor &color, const QRectF &box);
// Spotify's on/off controls (shuffle, repeat, queue): green with a dot underneath when on.
// The icon is size x (size + 6); give the button that icon size.
QIcon toggleIcon(Glyph g, bool on, int size);
// Rounded square: a cover cropped to fill it, or `bg` with an optional glyph on top.
QPixmap rounded(const QPixmap &src, QSize size, qreal radius, qreal dpr);
inline QPixmap rounded(const QPixmap &src, int size, qreal radius, qreal dpr) {
    return rounded(src, QSize(size, size), radius, dpr);
}
QPixmap tile(int size, qreal radius, qreal dpr, const QColor &bg, const QColor &fg = {}, const Glyph *glyph = nullptr,
             const QColor &bg2 = {});  // bg2: diagonal gradient bg -> bg2
QPixmap spotifyLogo(int size, qreal dpr);  // attribution mark beside the name only
QPixmap appIcon(int size, qreal dpr);      // ignify's own mark: a green flame on a dark tile

// The cover's most vivid colour; tints the now-playing panel like Spotify's own view.
QColor moodColor(const QPixmap &cover);
QColor washFrom(const QColor &mood);  // now-playing panel tint
// Up to `count` of the cover's own colours, most present first, lifted to read on a dark
// background. Never empty: a grey or black-and-white cover gives a soft grey.
QVector<QColor> coverPalette(const QPixmap &cover, int count);
// A smooth Gaussian-quality blur (Qt's own, through QGraphicsBlurEffect). Edges stay solid.
QImage blurred(const QImage &src, qreal radius);

// Downloads each image once: in-flight requests are shared, results kept in RAM and on disk.
class ImageCache : public QObject {
public:
    using Done = std::function<void(const QPixmap &)>;
    explicit ImageCache(QObject *parent = nullptr);
    void load(const QString &url, QObject *context, Done done);  // done skipped if context died

private:
    QNetworkAccessManager net;
    QCache<QString, QPixmap> memory;  // cost in KB
    QHash<QString, QList<QPair<QPointer<QObject>, Done>>> pending;
};

// Album grid cell: cover, bold title, muted artist; no box around it.
// Items with Role::Kind == Round draw a round placeholder (artists, as in Spotify).
class CardDelegate : public QStyledItemDelegate {
public:
    static constexpr int Cover = 168, Pad = 12, Round = 1;
    static QSize cell() { return {Cover + 2 * Pad, Cover + 2 * Pad + 46}; }
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return cell(); }
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
};

// Spotify's title cell: 40px album thumbnail (when `thumbs`), the title (its ForegroundRole, so
// the playing song is green) over the artists in grey (Role::Subtitle).
class TrackTitleDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    bool thumbs = true;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
};

// Queue / recently-played row: 40px cover, white title over grey artist. Role::Kind marks
// section headers ("Now playing", "Next from: ...") and the playing row (green title).
class RowDelegate : public QStyledItemDelegate {
public:
    enum { Header = 1, Current = 2 };
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &idx) const override;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
};
