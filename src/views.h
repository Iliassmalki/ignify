#pragma once
// Two extra windows that mirror the now-playing state the main window pushes to them:
// Spotify's always-on-top mini player and its full-screen now-playing view.
#include <QColor>
#include <QPixmap>
#include <QVector>
#include <QWidget>
#include <functional>
#include <vector>

class QProcess;
class QTimer;

struct NowPlaying {
    QString title, artist;
    QPixmap cover;
    QColor mood;  // invalid = monochrome cover
    bool playing = false;
    qint64 position = 0, duration = 0;
    bool local = false;  // the music plays on this computer, so the spectrum can listen to it
};

// Shared by both views: the transport callbacks the main window wires up.
struct Transport {
    std::function<void()> playPause, previous, next, closed;
};

class MiniPlayer : public QWidget {
public:
    explicit MiniPlayer(QWidget *owner);
    void setState(const NowPlaying &s);
    Transport transport;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private:
    NowPlaying state;
    bool hovered = false;
    QRectF closeRect, prevRect, playRect, nextRect;
};

class FullScreenView : public QWidget {
public:
    explicit FullScreenView(QWidget *owner);
    void setState(const NowPlaying &s);
    Transport transport;

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void hideEvent(QHideEvent *) override;
    void resizeEvent(QResizeEvent *) override { backdrop = {}; }

private:
    void buildBackdrop();
    QRectF coverRect() const;
    QRectF spectrumRect() const;
    QRectF barRect(int i, qreal h) const;  // bar i at height h; its rounded bottom falls off-screen
    void listen(bool on);
    void tick();
    void paintSpectrum(QPainter &p, const QRegion &dirty);

    NowPlaying state;
    QPixmap backdrop;  // blurred cover, scrim, shadow and grain at window size; rebuilt per cover/size
    QRectF closeRect, prevRect, playRect, nextRect;
    QVector<QColor> colors;       // the cover's palette, left to right across the bars
    QProcess *capture = nullptr;  // parec reading what the speakers play, only while shown and playing here
    QByteArray pcm;               // the latest samples, s16le mono
    QTimer *frames;
    std::vector<float> levels;    // what each bar shows now, eased towards the spectrum
};
