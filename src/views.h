#pragma once
// Two extra windows that mirror the now-playing state the main window pushes to them:
// Spotify's always-on-top mini player and its full-screen now-playing view.
#include <QColor>
#include <QPixmap>
#include <QWidget>
#include <functional>

struct NowPlaying {
    QString title, artist;
    QPixmap cover;
    QColor mood;  // invalid = monochrome cover
    bool playing = false;
    qint64 position = 0, duration = 0;
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
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *) override { backdrop = {}; }

private:
    NowPlaying state;
    QPixmap backdrop;  // blurred cover at window size, rebuilt only when the cover or size changes
    QRectF closeRect, prevRect, playRect, nextRect;
};
