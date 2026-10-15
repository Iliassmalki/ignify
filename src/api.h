#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <functional>

class QUrlQuery;

// Spotify Web API client: PKCE sign-in in the browser, token refresh, JSON requests.
class Api : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    using Callback = std::function<void(const QJsonObject &)>;
    using PageCallback = std::function<bool(const QJsonObject &)>;  // return false to stop paging

    void login();
    void get(const QString &path, Callback cb = {}) { send("GET", path, {}, std::move(cb)); }
    void send(const QByteArray &verb, const QString &path, const QJsonObject &body = {}, Callback cb = {});
    void getAll(const QString &path, PageCallback onPage);  // follows "next" links
    bool signingIn() const { return busy; }

signals:
    void ready();  // first token obtained
    void message(const QString &text);

private:
    void authorize();
    void refresh();
    void requestToken(const QUrlQuery &form, bool isRefresh);

    QNetworkAccessManager net;
    QString clientId, accessToken, refreshToken, verifier, state;
    QDateTime expiresAt;
    bool busy = false, signedIn = false;
    QList<std::function<void()>> waiting;  // requests parked until a token arrives
};
