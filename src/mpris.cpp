#include "mpris.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QRegularExpression>

static const QString kPath = "/org/mpris/MediaPlayer2";

Mpris::Mpris(QObject *parent) : QObject(parent) {
    new MprisRoot(this);  // adaptors are owned by, and exported with, this object
    new MprisPlayer(this);
    auto bus = QDBusConnection::sessionBus();
    // A second ignify (or a test run) takes a per-process name instead of failing silently.
    if (!bus.registerService("org.mpris.MediaPlayer2.ignify"))
        bus.registerService("org.mpris.MediaPlayer2.ignify.instance" + QString::number(QCoreApplication::applicationPid()));
    bus.registerObject(kPath, this);
}

QVariantMap Mpris::metadata() const {
    if (!state.hasTrack) return {};
    // Track ids become object paths, which only allow [A-Za-z0-9_]; Spotify ids are base62.
    QString id = QString(state.trackId).replace(QRegularExpression("[^A-Za-z0-9_]"), "_");
    return {{"mpris:trackid", QVariant::fromValue(QDBusObjectPath("/org/ignify/track/" + id))},
            {"mpris:length", qlonglong(state.lengthUs)},
            {"mpris:artUrl", state.artUrl},
            {"xesam:title", state.title},
            {"xesam:album", state.album},
            {"xesam:artist", state.artists}};
}

void Mpris::update(const State &s) {
    QVariantMap changed;
    State old = std::exchange(state, s);
    if (old.trackId != s.trackId || old.title != s.title || old.artUrl != s.artUrl || old.hasTrack != s.hasTrack) {
        changed["Metadata"] = metadata();
        for (auto *can : {"CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek"}) changed[can] = s.hasTrack;
    }
    if (old.playing != s.playing || old.hasTrack != s.hasTrack)
        changed["PlaybackStatus"] = !s.hasTrack ? "Stopped" : s.playing ? "Playing" : "Paused";
    if (old.shuffle != s.shuffle) changed["Shuffle"] = s.shuffle;
    if (old.loop != s.loop) changed["LoopStatus"] = s.loop;
    if (qAbs(old.volume - s.volume) > 0.005) changed["Volume"] = s.volume;
    if (changed.isEmpty()) return;
    auto msg = QDBusMessage::createSignal(kPath, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    msg << QString("org.mpris.MediaPlayer2.Player") << changed << QStringList();
    QDBusConnection::sessionBus().send(msg);
}

void Mpris::seeked(qint64 positionUs) {
    if (auto *player = findChild<MprisPlayer *>()) emit player->Seeked(positionUs);
}

void MprisPlayer::SetPosition(const QDBusObjectPath &track, qlonglong positionUs) {
    // The spec ignores stale requests: only act if it names the current track.
    if (track == m->metadata().value("mpris:trackid").value<QDBusObjectPath>() && m->seekTo) m->seekTo(positionUs);
}
