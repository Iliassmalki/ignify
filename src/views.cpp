#include "views.h"
#include "ui.h"
#include "util.h"
#include <QCloseEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>
#include <cstring>

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

constexpr int kBands = 56, kWindow = 4096, kRate = 44100;  // 4096 samples = 93 ms: calm, fine bass

QColor along(const QVector<QColor> &stops, qreal t) {  // t 0..1 across the palette
    if (stops.size() == 1) return stops[0];
    qreal at = t * (stops.size() - 1);
    int i = std::min(int(at), int(stops.size()) - 2);
    qreal f = at - i;
    const QColor &a = stops[i], &b = stops[i + 1];
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * f, a.greenF() + (b.greenF() - a.greenF()) * f,
                            a.blueF() + (b.blueF() - a.blueF()) * f);
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

FullScreenView::FullScreenView(QWidget *owner)
    : QWidget(owner, Qt::Window), frames(new QTimer(this)), levels(kBands, 0.f) {
    setWindowTitle("ignify");
    setCursor(Qt::ArrowCursor);
    frames->setInterval(42);  // 24 fps is plenty for bars that move this slowly
    connect(frames, &QTimer::timeout, this, [this] { tick(); });
    clock.start();
}

void FullScreenView::setState(const NowPlaying &s) {
    if (s.cover.cacheKey() != state.cover.cacheKey()) {
        backdrop = {};
        colors = coverPalette(s.cover, 3);
    }
    state = s;
    bool playing = isVisible() && s.playing;
    listen(playing && s.local);
    // No sound to read here (it plays on the phone, or parec is missing): the bars still move,
    // gently and on their own, in the cover's colours.
    ambient = playing && !capture;
    if (ambient) frames->start();
    update();
}

QRectF FullScreenView::coverRect() const {
    const qreal margin = 64, size = std::min(height() * .45, 460.0);
    return QRectF(margin, height() - margin - 150 - size, size, size);
}

// The floor of the screen, below the progress bar's line, so the bars never crowd the text.
QRectF FullScreenView::spectrumRect() const { return QRectF(0, height() * .8, width(), height() * .2); }

QRectF FullScreenView::barRect(int i, qreal h) const {
    QRectF area = spectrumRect();
    qreal slot = area.width() / kBands, w = slot * .56;
    return QRectF(area.left() + i * slot + (slot - w) / 2, area.bottom() - h, w, h + w);
}

// The 640 px cover, centre-cropped to the window and blurred just enough to sit behind text,
// a scrim that darkens towards the controls, the cover with a soft shadow, and a fine grain
// so the large dark gradients don't band. Built once per cover and window size.
void FullScreenView::buildBackdrop() {
    QRectF r = rect();
    backdrop = QPixmap((r.size() * devicePixelRatioF()).toSize());
    backdrop.setDevicePixelRatio(devicePixelRatioF());
    backdrop.fill(theme::black);
    QPainter p(&backdrop);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (!state.cover.isNull()) {
        // Blurred at 640 px wide, then scaled up: the blur hides what the smaller size loses.
        QSize work(640, std::max(1, int(640 * r.height() / std::max(1.0, r.width()))));
        QImage src = state.cover.toImage().scaled(work, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        src = src.copy((src.width() - work.width()) / 2, (src.height() - work.height()) / 2, work.width(), work.height());
        p.drawImage(r, blurred(src, 12));
    }
    QLinearGradient scrim(0, 0, 0, r.height());
    scrim.setColorAt(0, QColor(0, 0, 0, 70));
    scrim.setColorAt(.55, QColor(0, 0, 0, 120));
    scrim.setColorAt(1, QColor(0, 0, 0, 215));
    p.fillRect(r, scrim);

    QRectF cover = coverRect();
    const int spread = 60;
    QImage shadow((cover.size() + QSizeF(2 * spread, 2 * spread)).toSize(), QImage::Format_ARGB32_Premultiplied);
    shadow.fill(Qt::transparent);
    {
        QPainter s(&shadow);
        s.setRenderHint(QPainter::Antialiasing);
        s.setPen(Qt::NoPen);
        s.setBrush(QColor(0, 0, 0, 170));
        s.drawRoundedRect(QRectF(QPointF(spread, spread), cover.size()), 10, 10);
    }
    p.drawImage(cover.topLeft() + QPointF(-spread, -spread + 18), blurred(shadow, 28));

    static const QPixmap grain = [] {
        QImage g(128, 128, QImage::Format_ARGB32_Premultiplied);
        QRandomGenerator rng(7);  // the same grain every time
        for (int y = 0; y < g.height(); ++y)
            for (int x = 0; x < g.width(); ++x) {
                int alpha = 9, v = int(rng.bounded(256)) * alpha / 255;  // premultiplied
                g.setPixel(x, y, qRgba(v, v, v, alpha));
            }
        return QPixmap::fromImage(g);
    }();
    p.drawTiledPixmap(r, grain);
    if (!state.cover.isNull())  // the cover never changes between frames, so it lives here too (over the grain)
        p.drawPixmap(cover.topLeft(), rounded(state.cover, int(cover.width()), 10, devicePixelRatioF()));
}

// What the speakers play, read from the default output's monitor while the music plays here.
// ponytail: the whole output, so another app's sound shows too; parec --monitor-stream on
// librespot's own sink input would isolate it.
void FullScreenView::listen(bool on) {
    if (on == (capture != nullptr)) return;
    if (!on) {
        disconnect(capture, nullptr, this, nullptr);
        capture->kill();
        capture->waitForFinished(300);
        capture->deleteLater();
        capture = nullptr;
        pcm.clear();
        return;  // the bars ease down, then the timer stops
    }
    QString parec = QStandardPaths::findExecutable("parec");
    if (parec.isEmpty()) return;  // no PulseAudio/PipeWire tools: no spectrum, nothing breaks
    capture = new QProcess(this);
    capture->setStandardErrorFile(QProcess::nullDevice());
    connect(capture, &QProcess::readyReadStandardOutput, this, [this] {
        pcm += capture->readAllStandardOutput();
        qsizetype excess = pcm.size() - qsizetype(kWindow) * 2;
        if (excess > 0) pcm.remove(0, excess & ~qsizetype(1));  // drop whole samples only
    });
    capture->start(parec, {"--device=@DEFAULT_MONITOR@", "--format=s16le", QString("--rate=%1").arg(kRate),
                           "--channels=1", "--latency-msec=30", "--raw"});
    frames->start();
}

void FullScreenView::tick() {
    std::vector<float> target(kBands, 0.f);
    qsizetype end = pcm.size() & ~qsizetype(1);  // a trailing half sample waits for the next read
    if (capture && end >= qsizetype(kWindow) * 2) {
        std::vector<qint16> raw(kWindow);
        std::memcpy(raw.data(), pcm.constData() + end - qsizetype(kWindow) * 2, size_t(kWindow) * 2);
        std::vector<float> samples(kWindow);
        for (int i = 0; i < kWindow; ++i) samples[i] = raw[i] / 32768.f;
        target = spectrumBands(samples, kRate, kBands);
    } else if (ambient) {
        target = ambientBands(clock.elapsed() / 1000.f, kBands);
    }
    float peak = 0;
    qreal height = spectrumRect().height();
    QRegion dirty;  // only the strip each bar's top moved through: a fraction of the floor
    for (int i = 0; i < kBands; ++i) {
        // Each bar leans on its neighbours, then eases: up gently, down slower still.
        float t = .5f * target[i] + .25f * target[std::max(i - 1, 0)] + .25f * target[std::min(i + 1, kBands - 1)];
        float before = levels[i];
        levels[i] += (t - levels[i]) * (t > levels[i] ? .26f : .075f);
        if (!capture && !ambient && levels[i] < .004f) levels[i] = 0;
        peak = std::max(peak, levels[i]);
        qreal a = before * height, b = levels[i] * height;
        if (std::abs(a - b) < .25) continue;
        QRectF hi = barRect(i, std::max(a, b)), lo = barRect(i, std::min(a, b));
        dirty += QRectF(hi.left() - 1, hi.top() - 1, hi.width() + 2, lo.top() - hi.top() + hi.width() / 2 + 2).toAlignedRect();
    }
    if (!capture && !ambient && peak == 0) frames->stop();
    if (!dirty.isEmpty()) update(dirty);
}

void FullScreenView::paintSpectrum(QPainter &p, const QRegion &dirty) {
    QRectF area = spectrumRect();
    p.setPen(Qt::NoPen);
    for (int i = 0; i < kBands; ++i) {
        qreal h = levels[i] * area.height();
        QRectF bar = barRect(i, h);
        if (h < 1.5 || !dirty.intersects(bar.toAlignedRect())) continue;
        QColor c = along(colors, i / qreal(kBands - 1));
        // Brighter the higher it reaches: louder bars glow, quiet ones stay in the floor. Fixed
        // to the screen rather than the bar, so a moving bar only changes along its tip.
        QLinearGradient fade(0, area.top(), 0, area.bottom());
        c.setAlphaF(.75f);
        fade.setColorAt(0, c);
        c.setAlphaF(.08f);
        fade.setColorAt(1, c);
        p.setBrush(fade);
        p.drawRoundedRect(bar, bar.width() / 2, bar.width() / 2);
    }
}

void FullScreenView::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QRectF r = rect();
    if (backdrop.isNull()) buildBackdrop();
    p.drawPixmap(0, 0, backdrop);
    if (!colors.isEmpty()) paintSpectrum(p, e->region());

    const qreal margin = 64;
    QRectF cover = coverRect();

    QFont f("Inter Display");
    f.setWeight(QFont::Bold);
    f.setPixelSize(56);
    p.setFont(f);
    qreal textX = cover.right() + 40, textW = r.width() - textX - margin;
    if (e->rect().intersects(QRectF(textX, cover.bottom() - 110, textW, 104).toAlignedRect())) {  // not on bar frames
        elidedText(p, QRectF(textX, cover.bottom() - 110, textW, 70), state.title.isEmpty() ? "Nothing playing" : state.title, theme::white);
        f.setWeight(QFont::Normal);
        f.setPixelSize(22);
        p.setFont(f);
        elidedText(p, QRectF(textX, cover.bottom() - 38, textW, 32), state.artist, QColor(255, 255, 255, 190));
    }

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

void FullScreenView::hideEvent(QHideEvent *) {
    listen(false);  // never listen while nobody is looking
    ambient = false;
    frames->stop();
    std::fill(levels.begin(), levels.end(), 0.f);
}
