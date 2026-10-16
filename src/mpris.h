#pragma once
// MPRIS 2 on the session bus, so media keys, the desktop's sound menu and the lock screen
// can drive ignify. Spec: https://specifications.freedesktop.org/mpris-spec/latest/
#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <functional>

class Mpris : public QObject {
    Q_OBJECT
public:
    struct State {
        QString trackId, title, album, artUrl;
        QStringList artists;
        qint64 lengthUs = 0;
        bool playing = false, hasTrack = false, shuffle = false;
        QString loop = "None";  // None, Track, Playlist
        double volume = 1;
    };
    explicit Mpris(QObject *parent);
    void update(const State &s);  // emits PropertiesChanged for whatever changed
    void seeked(qint64 positionUs);
    QVariantMap metadata() const;

    State state;
    std::function<qint64()> positionUs;  // read on demand, never pushed (per the spec)
    std::function<void()> raise, playPause, play, pause, next, previous;
    std::function<void(qint64)> seekBy, seekTo;  // microseconds
    std::function<void(bool)> setShuffle;
    std::function<void(const QString &)> setLoop;
    std::function<void(double)> setVolume;
};

class MprisRoot : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ no)
    Q_PROPERTY(bool CanRaise READ yes)
    Q_PROPERTY(bool HasTrackList READ no)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ none)
    Q_PROPERTY(QStringList SupportedMimeTypes READ none)
public:
    explicit MprisRoot(Mpris *m) : QDBusAbstractAdaptor(m), m(m) {}
    bool yes() const { return true; }
    bool no() const { return false; }
    QString identity() const { return "ignify"; }
    QString desktopEntry() const { return "ignify"; }
    QStringList none() const { return {}; }
public slots:
    void Raise() { if (m->raise) m->raise(); }
    void Quit() {}

private:
    Mpris *m;
};

class MprisPlayer : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double Rate READ one)
    Q_PROPERTY(double MinimumRate READ one)
    Q_PROPERTY(double MaximumRate READ one)
    Q_PROPERTY(bool CanGoNext READ hasTrack)
    Q_PROPERTY(bool CanGoPrevious READ hasTrack)
    Q_PROPERTY(bool CanPlay READ hasTrack)
    Q_PROPERTY(bool CanPause READ hasTrack)
    Q_PROPERTY(bool CanSeek READ hasTrack)
    Q_PROPERTY(bool CanControl READ yes)
public:
    explicit MprisPlayer(Mpris *m) : QDBusAbstractAdaptor(m), m(m) {}
    QString playbackStatus() const { return !m->state.hasTrack ? "Stopped" : m->state.playing ? "Playing" : "Paused"; }
    QString loopStatus() const { return m->state.loop; }
    void setLoopStatus(const QString &s) { if (m->setLoop) m->setLoop(s); }
    bool shuffle() const { return m->state.shuffle; }
    void setShuffle(bool on) { if (m->setShuffle) m->setShuffle(on); }
    QVariantMap metadata() const { return m->metadata(); }
    double volume() const { return m->state.volume; }
    void setVolume(double v) { if (m->setVolume) m->setVolume(v); }
    qlonglong position() const { return m->positionUs ? m->positionUs() : 0; }
    double one() const { return 1.0; }
    bool hasTrack() const { return m->state.hasTrack; }
    bool yes() const { return true; }
public slots:
    void Next() { if (m->next) m->next(); }
    void Previous() { if (m->previous) m->previous(); }
    void Pause() { if (m->pause) m->pause(); }
    void PlayPause() { if (m->playPause) m->playPause(); }
    void Stop() { if (m->pause) m->pause(); }
    void Play() { if (m->play) m->play(); }
    void Seek(qlonglong offsetUs) { if (m->seekBy) m->seekBy(offsetUs); }
    void SetPosition(const QDBusObjectPath &track, qlonglong positionUs);
    void OpenUri(const QString &) {}
signals:
    void Seeked(qlonglong Position);

private:
    Mpris *m;
};
