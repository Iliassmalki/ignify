#include "api.h"
#include "util.h"
#include <QDesktopServices>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

static const QString kRedirect = "http://127.0.0.1:8888/callback";
// Overridable so the UI can be exercised against a local mock (see README "Testing").
static const QString kApi = qEnvironmentVariable("IGNIFY_API", "https://api.spotify.com/v1");
static const QString kAccounts = qEnvironmentVariable("IGNIFY_ACCOUNTS", "https://accounts.spotify.com");
static const QString kScopes = "user-read-playback-state user-modify-playback-state "
                               "playlist-read-private playlist-read-collaborative user-library-read "
                               "user-library-modify playlist-modify-public playlist-modify-private";

static QString randomToken(int words) {
    QByteArray b(words * 4, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(b.data()), words);
    return b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

void Api::login() {
    QSettings s;
    clientId = s.value("clientId").toString();
    refreshToken = s.value("refreshToken").toString();
    refresh();
}

void Api::refresh() {
    if (busy) return;
    if (refreshToken.isEmpty()) return authorize();
    busy = true;
    requestToken({{"grant_type", "refresh_token"}, {"refresh_token", refreshToken}, {"client_id", clientId}}, true);
}

void Api::authorize() {
    busy = true;
    verifier = randomToken(16);  // 64 chars
    state = randomToken(4);
    auto *server = new QTcpServer(this);
    if (!server->listen(QHostAddress::LocalHost, 8888)) {
        busy = false;
        server->deleteLater();
        emit message("Port 8888 is in use; sign-in needs it free. Close whatever holds it and restart.");
        return;
    }
    connect(server, &QTcpServer::newConnection, this, [this, server] {
        auto *sock = server->nextPendingConnection();
        sock->setParent(this);  // outlive the server, or deleting it drops the reply unsent
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        connect(sock, &QTcpSocket::readyRead, this, [this, server, sock] {
            if (!sock->canReadLine()) return;
            // Request line: "GET /callback?code=...&state=... HTTP/1.1"
            QUrl url(QString::fromLatin1("http://127.0.0.1" + sock->readLine().split(' ').value(1)));
            QUrlQuery q(url);
            bool isCallback = url.path() == "/callback";
            bool ok = isCallback && q.queryItemValue("state") == state && q.hasQueryItem("code");
            if (!isCallback) {  // favicon and friends: keep listening
                sock->write("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
                sock->disconnectFromHost();
                return;
            }
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n");
            sock->write(ok ? "<h3>ignify is signed in. You can close this tab.</h3>"
                           : "<h3>Sign-in failed. See ignify for details.</h3>");
            sock->disconnectFromHost();
            server->close();
            server->deleteLater();
            if (!ok) {
                busy = false;
                emit message("Spotify sign-in failed: " + q.queryItemValue("error"));
                return;
            }
            requestToken({{"grant_type", "authorization_code"},
                          {"code", q.queryItemValue("code")},
                          {"redirect_uri", kRedirect},
                          {"client_id", clientId},
                          {"code_verifier", verifier}},
                         false);
        });
    });

    QUrl url(kAccounts + "/authorize");
    url.setQuery(QUrlQuery{{"client_id", clientId},
                           {"response_type", "code"},
                           {"redirect_uri", kRedirect},
                           {"code_challenge_method", "S256"},
                           {"code_challenge", pkceChallenge(verifier)},
                           {"scope", kScopes},
                           {"state", state}});
    emit message("Sign in to Spotify in your browser…");
    QDesktopServices::openUrl(url);
}

void Api::requestToken(const QUrlQuery &form, bool isRefresh) {
    QNetworkRequest req(QUrl(kAccounts + "/api/token"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    auto *reply = net.post(req, form.query(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, isRefresh] {
        reply->deleteLater();
        busy = false;
        auto o = QJsonDocument::fromJson(reply->readAll()).object();
        if (o["access_token"].toString().isEmpty()) {
            emit message("Spotify sign-in failed: " + o["error_description"].toString(reply->errorString()));
            if (isRefresh && o["error"] == "invalid_grant") {  // refresh token revoked or expired
                refreshToken.clear();
                QSettings().remove("refreshToken");
                authorize();
            } else if (!reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()) {
                QTimer::singleShot(10'000, this, &Api::refresh);  // offline: try again shortly
            }
            return;
        }
        // A sign-in from an older ignify lacks newer permissions (liking, editing playlists):
        // ask once in the browser instead of failing those actions with 403s.
        if (auto granted = o["scope"].toString().split(' '); o.contains("scope")) {
            for (const auto &need : kScopes.split(' '))
                if (!granted.contains(need)) {
                    emit message("ignify needs permission to like songs and edit playlists. Approve it in your browser.");
                    refreshToken.clear();
                    QSettings().remove("refreshToken");
                    return authorize();
                }
        }
        accessToken = o["access_token"].toString();
        expiresAt = QDateTime::currentDateTimeUtc().addSecs(o["expires_in"].toInt(3600) - 60);
        if (auto r = o["refresh_token"].toString(); !r.isEmpty()) {
            refreshToken = r;
            QSettings s;
            s.setValue("refreshToken", r);
            s.sync();
            // ponytail: plaintext config locked to the owner; move to libsecret if that's not enough
            QFile::setPermissions(s.fileName(), QFile::ReadOwner | QFile::WriteOwner);
        }
        for (auto &retry : std::exchange(waiting, {})) retry();
        if (!std::exchange(signedIn, true)) emit ready();
    });
}

void Api::send(const QByteArray &verb, const QString &path, const QJsonObject &body, Callback cb) {
    // Wall-clock expiry also covers waking from suspend with a stale token.
    if (accessToken.isEmpty() || QDateTime::currentDateTimeUtc() >= expiresAt) {
        waiting << [=] { send(verb, path, body, cb); };
        refresh();
        return;
    }
    // "next" links arrive absolute. Only follow ones inside the API: the Bearer token goes with
    // the request, so a link elsewhere would hand it to whoever runs that host.
    if (path.startsWith("http") && !path.startsWith(kApi + "/")) {
        emit message("Ignored a link outside the Spotify API.");
        return;
    }
    QNetworkRequest req(QUrl(path.startsWith("http") ? path : kApi + path));
    req.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
    QByteArray data;
    if (!body.isEmpty()) {
        data = QJsonDocument(body).toJson(QJsonDocument::Compact);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    }
    if (verb != "GET") req.setHeader(QNetworkRequest::ContentLengthHeader, data.size());  // Spotify 411s without it
    auto *reply = verb == "GET" ? net.get(req) : net.sendCustomRequest(req, verb, data);
    connect(reply, &QNetworkReply::finished, this, [=] {
        reply->deleteLater();
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 429) {
            int wait = std::max(1, reply->rawHeader("Retry-After").toInt());
            QTimer::singleShot(wait * 1000, this, [=] { send(verb, path, body, cb); });
            return;
        }
        auto doc = QJsonDocument::fromJson(reply->readAll());
        // Some endpoints answer with a bare array (e.g. library/contains); hand it over as {"items": [...]}.
        auto json = doc.isArray() ? QJsonObject{{"items", doc.array()}} : doc.object();
        if (reply->error() != QNetworkReply::NoError) {
            emit message(json["error"].toObject()["message"].toString(reply->errorString()));
            return;
        }
        if (cb) cb(json);
    });
}

void Api::getAll(const QString &path, PageCallback onPage) {
    get(path, [this, onPage](const QJsonObject &page) {
        auto next = page["next"].toString();
        if (onPage(page) && !next.isEmpty()) getAll(next, onPage);
    });
}
