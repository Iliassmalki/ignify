#include "views.h"
#include "ui.h"
#include "util.h"
#include <QCloseEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>

namespace {
void transportButtons(QPainter &p, bool playing, QPointF centre, qreal big, QRectF &prev, QRectF &play, QRectF &next) {
    qreal small = big * .55, gap = big * .55;
    play = QRectF(centre.x() - big / 2, centre.y() - big / 2, big, big);
    prev = QRectF(play.left() - gap - small, centre.y() - small / 2, small, small);
    next = QRectF(play.right() + gap, centre.y() - small / 2, small, small);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::white);
    p.drawEllipse(play);
    paintGlyph(p, playing ? Glyph::Pause : Glyph::Play, theme::base, play.adjusted(big * .22, big * .22, -big * .22, -big * .22));
    paintGlyph(p, Glyph::Prev, theme::white, prev);
    paintGlyph(p, Glyph::Next, theme::white, next);
}

void elidedText(QPainter &p, const QRectF &r, const QString &text, const QColor &c) {
    p.setPen(c);
    p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(p.font()).elidedText(text, Qt::ElideRight, int(r.width())));
}
} // namespace

// ---- mini player -------------------------------------------------------------

MiniPlayer::MiniPlayer(QWidget *owner)
    : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setAttribute(Qt::WA_TranslucentBackground);  // rounded corners
    setWindowTitle("ignify mini player");
    resize(300, 300);
    setMouseTracking(true);
}

void MiniPlayer::setState(const NowPlaying &s) {
    state = s;
    update();
}

void MiniPlayer::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QRectF r = rect();
    QPainterPath clip;
    clip.addRoundedRect(r, 10, 10);
    p.setClipPath(clip);
    p.fillRect(r, theme::base);
    if (!state.cover.isNull()) p.drawPixmap(r, state.cover, QRectF(state.cover.rect()));

    // Scrim so the title reads on any cover.
    QLinearGradient scrim(0, r.height() * .5, 0, r.height());
    scrim.setColorAt(0, QColor(0, 0, 0, 0));
    scrim.setColorAt(1, QColor(0, 0, 0, 215));
    p.fillRect(r, scrim);
    QFont f = font();
    f.setWeight(QFont::Bold);
    f.setPixelSize(16);
    p.setFont(f);
    elidedText(p, QRectF(16, r.height() - 62, r.width() - 32, 22), state.title.isEmpty() ? "Nothing playing" : state.title, theme::white);
    f.setWeight(QFont::Normal);
    f.setPixelSize(13);
    p.setFont(f);
    elidedText(p, QRectF(16, r.height() - 40, r.width() - 32, 18), state.artist, theme::subdued);
    if (state.duration > 0) {  // progress hairline along the bottom edge
        p.fillRect(QRectF(0, r.height() - 3, r.width(), 3), QColor(255, 255, 255, 50));
        p.fillRect(QRectF(0, r.height() - 3, r.width() * state.position / state.duration, 3), theme::green);
    }

    if (hovered) {  // controls appear on hover, as in Spotify's mini player
        p.fillRect(r, QColor(0, 0, 0, 110));
        transportButtons(p, state.playing, QPointF(r.center().x(), r.height() * .45), 52, prevRect, playRect, nextRect);
        closeRect = QRectF(10, 10, 26, 26);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 140));
        p.drawEllipse(closeRect);
        paintGlyph(p, Glyph::Close, theme::white, closeRect.adjusted(5, 5, -5, -5));
    }
}

void MiniPlayer::enterEvent(QEnterEvent *) {
    hovered = true;
    update();
}

void MiniPlayer::leaveEvent(QEvent *) {
    hovered = false;
    update();
}

void MiniPlayer::mousePressEvent(QMouseEvent *e) {
    QPointF at = e->position();
    auto call = [](const std::function<void()> &f) { if (f) f(); };
    if (hovered && closeRect.contains(at)) return void(close());
    if (hovered && playRect.contains(at)) return call(transport.playPause);
    if (hovered && prevRect.contains(at)) return call(transport.previous);
    if (hovered && nextRect.contains(at)) return call(transport.next);
    if (windowHandle()) windowHandle()->startSystemMove();  // drag anywhere else
}

void MiniPlayer::closeEvent(QCloseEvent *e) {
    if (transport.closed) transport.closed();
    QWidget::closeEvent(e);
}

// ---- full screen -------------------------------------------------------------

FullScreenView::FullScreenView(QWidget *owner) : QWidget(owner, Qt::Window) {
    setWindowTitle("ignify");
    setCursor(Qt::ArrowCursor);
}

void FullScreenView::setState(const NowPlaying &s) {
    if (s.cover.cacheKey() != state.cover.cacheKey()) backdrop = {};
    state = s;
    update();
}

void FullScreenView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QRectF r = rect();
    p.fillRect(r, theme::black);

    // A cheap blur: the cover shrunk to 24 px, then scaled back up smoothly. Built once per cover.
    if (backdrop.isNull() && !state.cover.isNull()) {
        QImage tiny = state.cover.toImage().scaled(24, 24, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        backdrop = QPixmap::fromImage(tiny.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    }
    if (!backdrop.isNull()) p.drawPixmap(0, 0, backdrop);
    p.fillRect(r, QColor(0, 0, 0, 150));

    const qreal margin = 64;
    qreal coverSize = std::min(r.height() * .45, 460.0);
    QRectF cover(margin, r.height() - margin - 150 - coverSize, coverSize, coverSize);
    if (!state.cover.isNull()) p.drawPixmap(cover.topLeft(), rounded(state.cover, int(coverSize), 10, devicePixelRatioF()));

    QFont f("Inter Display");
    f.setWeight(QFont::Bold);
    f.setPixelSize(56);
    p.setFont(f);
    qreal textX = cover.right() + 40, textW = r.width() - textX - margin;
    elidedText(p, QRectF(textX, cover.bottom() - 110, textW, 70), state.title.isEmpty() ? "Nothing playing" : state.title, theme::white);
    f.setWeight(QFont::Normal);
    f.setPixelSize(22);
    p.setFont(f);
    elidedText(p, QRectF(textX, cover.bottom() - 38, textW, 32), state.artist, QColor(255, 255, 255, 190));

    // Progress, then transport, along the bottom.
    qreal barY = r.height() - margin - 92;
    QRectF track(margin + 56, barY, r.width() - 2 * margin - 112, 4);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 70));
    p.drawRoundedRect(track, 2, 2);
    if (state.duration > 0) {
        p.setBrush(theme::white);
        p.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * state.position / state.duration, 4), 2, 2);
    }
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 200));
    p.drawText(QRectF(margin, barY - 8, 48, 20), Qt::AlignRight | Qt::AlignVCenter, formatTime(state.position));
    p.drawText(QRectF(r.width() - margin - 48, barY - 8, 48, 20), Qt::AlignLeft | Qt::AlignVCenter, formatTime(state.duration));
    transportButtons(p, state.playing, QPointF(r.center().x(), r.height() - margin - 36), 60, prevRect, playRect, nextRect);

    closeRect = QRectF(r.width() - margin - 40, margin - 24, 40, 40);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 120));
    p.drawEllipse(closeRect);
    paintGlyph(p, Glyph::Close, theme::white, closeRect.adjusted(10, 10, -10, -10));
}

void FullScreenView::mousePressEvent(QMouseEvent *e) {
    QPointF at = e->position();
    auto call = [](const std::function<void()> &f) { if (f) f(); };
    if (closeRect.contains(at)) return void(close());
    if (playRect.contains(at)) return call(transport.playPause);
    if (prevRect.contains(at)) return call(transport.previous);
    if (nextRect.contains(at)) return call(transport.next);
}

void FullScreenView::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_F11) return void(close());
    if (e->key() == Qt::Key_Space && transport.playPause) return transport.playPause();
    QWidget::keyPressEvent(e);
}

void FullScreenView::closeEvent(QCloseEvent *e) {
    if (transport.closed) transport.closed();
    QWidget::closeEvent(e);
}
