#include "api.h"
#include "ui.h"
#include "util.h"
#include <QtNetwork>
#include <QtWidgets>
#include <csignal>

static const QString kDevice = "ignify";  // name librespot announces; how we find our own speaker
static const QColor likedFrom("#450AF5"), likedTo("#C4EFD9");  // Spotify's Liked Songs gradient
// Keyless public services for what Spotify's public API doesn't offer; overridable for the mock.
static const QString kLyrics = qEnvironmentVariable("IGNIFY_LYRICS", "https://lrclib.net");
static const QString kWiki = qEnvironmentVariable("IGNIFY_WIKI", "https://en.wikipedia.org");

// "uris=a,b" with each URI encoded but the separating commas kept, as Spotify's docs show.
static QString encodedUris(const QStringList &uris) {
    QStringList out;
    for (const auto &u : uris) out << QString::fromLatin1(QUrl::toPercentEncoding(u));
    return out.join(',');
}
enum Kind { AlbumsPage = 1, LikedSongs, Playlist };

// Web API device "type" → the icon Spotify's picker shows for it.
static Glyph deviceGlyph(const QString &type) {
    if (type == "Computer") return Glyph::Computer;
    if (type == "Smartphone" || type == "Tablet") return Glyph::Phone;
    return Glyph::Speaker;
}
enum Col { Num, Title, Artist, AlbumCol, Time };

struct ListInfo {
    QString kind, title, meta, coverUrl, path, context;
    QPixmap fallback;  // shown when there's no cover url (Liked Songs)
    bool albumView = false;
};

class Window : public QMainWindow {
public:
    explicit Window(Api *api) : api(api) {
        setWindowTitle("ignify");
        setMinimumSize(1080, 660);
        auto *side = buildSidebar();
        auto *panel = buildNowPlaying();
        pages = new QStackedWidget;
        pages->setObjectName("pages");
        pages->addWidget(buildAlbumsPage());
        pages->addWidget(buildListPage());
        pages->addWidget(buildLyricsPage());
        connect(pages, &QStackedWidget::currentChanged, this, [this] {
            bool on = pages->currentWidget() == lyricsPage;
            lyricsButton->setIcon(glyphIcon(Glyph::Mic, on ? theme::green : theme::white, 20));
            if (on) loadLyrics();
        });

        auto *body = new QHBoxLayout;
        body->setContentsMargins(8, 8, 8, 0);
        body->setSpacing(8);
        body->addWidget(side);
        body->addWidget(pages, 1);
        body->addWidget(panel);
        auto *root = new QWidget;
        root->setObjectName("main");
        auto *rootLayout = new QVBoxLayout(root);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);
        rootLayout->addLayout(body, 1);
        rootLayout->addWidget(buildBar());
        connectBar = new QFrame;
        connectBar->setObjectName("connectBar");
        auto *strip = new QHBoxLayout(connectBar);
        strip->setContentsMargins(16, 3, 24, 3);
        strip->setSpacing(8);
        auto *stripIcon = new QLabel;
        stripIcon->setPixmap(glyphIcon(Glyph::Devices, theme::black, 16).pixmap(16, 16));
        connectText = new QLabel;
        connectText->setTextFormat(Qt::PlainText);  // device names are user-chosen
        strip->addStretch();
        strip->addWidget(stripIcon);
        strip->addWidget(connectText);
        connectBar->hide();
        rootLayout->addWidget(connectBar);
        setCentralWidget(root);

        connect(api, &Api::message, this, [this](const QString &m) { statusBar()->showMessage(m, 6000); });
        // ponytail: 1s poll is the source of truth; the 200ms tick only interpolates the progress bar
        connect(&pollTimer, &QTimer::timeout, this, &Window::poll);
        connect(&tick, &QTimer::timeout, this, &Window::advanceProgress);
        tick.start(200);
        new QShortcut(QKeySequence("Ctrl+Space"), this, [this] { playPause->click(); });
        new QShortcut(QKeySequence("Ctrl+L"), this, [this] { search->setFocus(); search->selectAll(); });
        sinceProgress.start();
        setPlaying(false);
        nav->setFocus();  // otherwise search grabs focus and shows its ring on launch
        showIdle();
        tintPanel({});
    }

    ~Window() override {
        // librespot leaves Spotify Connect cleanly only on SIGINT (its Ctrl-C handler);
        // QProcess's destructor would SIGKILL it and phones would keep listing "ignify".
        if (engine && engine->state() != QProcess::NotRunning) {
            ::kill(pid_t(engine->processId()), SIGINT);
            if (!engine->waitForFinished(3000)) engine->kill();
        }
    }

    void start() {
        startLibrespot();
        findDevice(15);
        api->get("/me", [this](const QJsonObject &me) {
            myId = me["id"].toString();
            loadPlaylists();  // needs myId to tell owned playlists from followed ones
        });
        nav->setCurrentRow(0);
        pollTimer.start(1000);
        poll();
    }

private:
    qreal dpr() const { return devicePixelRatioF(); }

    // Plain text by default: titles, playlist names, lyrics and bios come from the network, and
    // QLabel's AutoText would render "<img src=/home/...>" or "<h1>" in them as rich text.
    // The few labels that need links opt in with setTextFormat(Qt::RichText) and escape their data.
    static QLabel *label(const QString &text = {}, const char *name = nullptr) {
        auto *l = new QLabel;
        l->setTextFormat(Qt::PlainText);
        l->setText(text);
        if (name) l->setObjectName(name);
        return l;
    }

    QToolButton *iconButton(Glyph g, int icon, const QString &tip) {
        auto *b = new QToolButton;
        b->setIcon(glyphIcon(g, theme::white, icon));
        b->setIconSize({icon, icon});
        b->setToolTip(tip);
        return b;
    }

    // ---- layout -------------------------------------------------------------

    QWidget *buildSidebar() {
        auto *side = new QFrame;
        side->setObjectName("sidebar");
        side->setFixedWidth(264);
        auto *layout = new QVBoxLayout(side);
        layout->setContentsMargins(12, 18, 10, 12);
        layout->setSpacing(12);
        search = new QLineEdit;
        search->setPlaceholderText("Search songs");
        search->setClearButtonEnabled(true);
        nav = new QListWidget;
        nav->setObjectName("nav");
        nav->setIconSize({40, 40});
        nav->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        auto *brand = new QHBoxLayout;
        brand->setContentsMargins(6, 0, 0, 4);
        brand->setSpacing(10);
        auto *mark = new QLabel;
        mark->setPixmap(spotifyLogo(30, dpr()));
        mark->setToolTip("Powered by Spotify");
        brand->addWidget(mark);
        brand->addWidget(label("ignify", "brand"));
        brand->addStretch();
        layout->addLayout(brand);
        layout->addWidget(search);
        layout->addWidget(nav, 1);

        Glyph grid = Glyph::Grid, heart = Glyph::Heart;
        navItem("Albums", AlbumsPage, tile(40, 4, dpr(), theme::highlight, theme::white, &grid));
        navItem("Liked Songs", LikedSongs, tile(40, 4, dpr(), likedFrom, theme::white, &heart, likedTo));
        auto *section = new QListWidgetItem("Playlists", nav);
        section->setFlags(Qt::NoItemFlags);
        section->setSizeHint({0, 44});  // breathing room above the list; QSS padding doesn't reach disabled items
        section->setTextAlignment(Qt::AlignLeft | Qt::AlignBottom);
        QFont small = font();
        small.setPointSizeF(9);
        small.setWeight(QFont::DemiBold);
        section->setFont(small);

        connect(search, &QLineEdit::returnPressed, this, [this] { runSearch(search->text().trimmed()); });
        connect(nav, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) { if (it) openNav(it); });
        // Double-click plays a playlist outright, the way into ones Spotify won't list.
        connect(nav, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
            if (auto ctx = it->data(Role::Uri).toString(); !ctx.isEmpty()) play({{"context_uri", ctx}});
        });
        return side;
    }

    QListWidgetItem *navItem(const QString &text, Kind kind, const QPixmap &icon) {
        auto *it = new QListWidgetItem(QIcon(icon), text, nav);
        it->setData(Role::Kind, kind);
        return it;
    }

    QWidget *buildAlbumsPage() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(28, 26, 8, 0);
        layout->setSpacing(4);
        albumsMeta = label({}, "muted");
        grid = new QListWidget;
        grid->setViewMode(QListView::IconMode);
        grid->setResizeMode(QListView::Adjust);
        grid->setMovement(QListView::Static);
        grid->setUniformItemSizes(true);
        grid->setGridSize(CardDelegate::cell());
        grid->setItemDelegate(new CardDelegate(grid));
        grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        grid->setMouseTracking(true);
        grid->viewport()->setAttribute(Qt::WA_Hover);
        layout->addWidget(label("Albums", "h1"));
        layout->addWidget(albumsMeta);
        layout->addSpacing(14);
        layout->addWidget(grid, 1);
        // Covers load as they scroll near the view and are dropped two screens away (syncGridCovers).
        connect(grid->verticalScrollBar(), &QScrollBar::valueChanged, this, &Window::syncGridCovers);
        connect(grid->verticalScrollBar(), &QScrollBar::rangeChanged, this, &Window::syncGridCovers);
        connect(grid, &QListWidget::itemActivated, this,
                [this](QListWidgetItem *it) { openAlbum(it->data(Role::Json).toJsonObject()); });
        connect(grid, &QListWidget::itemClicked, this,
                [this](QListWidgetItem *it) { openAlbum(it->data(Role::Json).toJsonObject()); });
        return page;
    }

    QWidget *buildListPage() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(28, 26, 8, 0);
        layout->setSpacing(22);

        headCover = new QLabel;
        headCover->setFixedSize(184, 184);
        headKind = label({}, "kind");
        headTitle = label({}, "h1");
        headTitle->setWordWrap(true);
        headMeta = label({}, "muted");
        headMeta->setWordWrap(true);
        headPlay = new QToolButton;
        headPlay->setObjectName("bigPlay");
        headPlay->setIcon(glyphIcon(Glyph::Play, theme::base, 26));
        headPlay->setIconSize({26, 26});
        headPlay->setFixedSize(54, 54);
        headPlay->setToolTip("Play");
        auto *text = new QVBoxLayout;
        text->setSpacing(6);
        text->addStretch();
        text->addWidget(headKind);
        text->addWidget(headTitle);
        text->addWidget(headMeta);
        text->addSpacing(10);
        text->addWidget(headPlay, 0, Qt::AlignLeft);  // unaligned, its fixed width would cap the whole column
        auto *head = new QHBoxLayout;
        head->setSpacing(24);
        head->addWidget(headCover, 0, Qt::AlignLeft | Qt::AlignBottom);
        head->addLayout(text, 1);

        tracks = new QTreeWidget;
        tracks->setHeaderLabels({"#", "Title", "Artist", "Album", "Time"});
        tracks->setRootIsDecorated(false);
        tracks->setUniformRowHeights(true);  // Qt skips measuring every row: 5000 songs stay instant
        tracks->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        tracks->setMouseTracking(true);
        tracks->setSelectionMode(QAbstractItemView::ExtendedSelection);  // shift/ctrl-click several songs
        tracks->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tracks, &QWidget::customContextMenuRequested, this, &Window::showTrackMenu);
        auto *h = tracks->header();
        h->setStretchLastSection(false);
        h->setSectionResizeMode(QHeaderView::Stretch);
        h->setSectionResizeMode(Num, QHeaderView::Fixed);  // ResizeToContents would rescan all rows per page
        h->setSectionResizeMode(Time, QHeaderView::Fixed);
        h->resizeSection(Num, 48);
        h->resizeSection(Time, 64);
        tracks->headerItem()->setTextAlignment(Time, Qt::AlignRight | Qt::AlignVCenter);

        layout->addLayout(head);
        layout->addWidget(tracks, 1);
        connect(tracks, &QTreeWidget::itemActivated, this,
                [this](QTreeWidgetItem *it) { playFrom(tracks->indexOfTopLevelItem(it)); });
        connect(headPlay, &QToolButton::clicked, this, [this] { playFrom(0); });
        return page;
    }

    QWidget *buildNowPlaying() {
        panel = new QFrame;
        panel->setObjectName("panel");
        panel->setFixedWidth(316);
        auto *scroll = new QScrollArea;  // the artist card can run past the window's height
        scroll->setObjectName("panelScroll");
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *outer = new QVBoxLayout(panel);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(scroll);
        auto *content = new QWidget;
        scroll->setWidget(content);
        auto *layout = new QVBoxLayout(content);
        layout->setContentsMargins(24, 28, 12, 24);
        layout->setSpacing(4);
        npCover = new QLabel;
        npCover->setFixedSize(268, 268);
        npTitle = label({}, "npTitle");
        npTitle->setWordWrap(true);
        npArtist = label({}, "npArtist");
        npArtist->setWordWrap(true);
        npAlbum = label({}, "muted");
        npAlbum->setWordWrap(true);
        npAlbum->setToolTip("Open album");
        npAlbum->setTextFormat(Qt::RichText);  // link built below with an escaped album name
        npAlbum->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        layout->addWidget(npCover);
        layout->addSpacing(18);
        layout->addWidget(npTitle);
        layout->addWidget(npArtist);
        layout->addSpacing(6);
        layout->addWidget(npAlbum);
        layout->addSpacing(22);
        layout->addWidget(buildArtistCard());
        layout->addStretch();
        connect(npAlbum, &QLabel::linkActivated, this, [this] { openAlbum(nowAlbum); });
        return panel;
    }

    // Spotify's "About the artist": photo with a badge, name, genres, then a bio. Spotify's own bio
    // and listener counts are private API, so the bio comes from Wikipedia, credited on the card.
    QWidget *buildArtistCard() {
        artistCard = new QFrame;
        artistCard->setObjectName("artistCard");
        artistCard->setFixedWidth(268);
        auto *layout = new QVBoxLayout(artistCard);
        layout->setContentsMargins(0, 0, 0, 16);
        layout->setSpacing(4);
        artistPhoto = new QLabel;
        artistPhoto->setFixedSize(268, 170);
        auto *badge = new QLabel("About the artist", artistPhoto);
        badge->setObjectName("aboutBadge");
        badge->ensurePolished();
        badge->adjustSize();
        badge->move(12, 12);
        artistName = label({}, "artistName");
        artistGenres = label({}, "muted");
        artistBio = label({}, "muted");
        artistBio->setWordWrap(true);
        artistSource = label({}, "muted");
        artistSource->setTextFormat(Qt::RichText);  // "From Wikipedia" link, URL checked and escaped
        artistSource->setOpenExternalLinks(true);
        layout->addWidget(artistPhoto);
        layout->addSpacing(10);
        for (auto *l : {artistName, artistGenres, artistBio, artistSource}) {
            l->setContentsMargins(16, 0, 16, 0);
            layout->addWidget(l);
        }
        artistCard->hide();
        return artistCard;
    }

    // Spotify's lyrics view: the song's colour fills the page, the sung line is white, the
    // rest dim, and it scrolls itself. Lyrics come from LRCLIB (Spotify's are private API).
    QWidget *buildLyricsPage() {
        lyricsPage = new QFrame;
        lyricsPage->setObjectName("lyricsPage");
        lyricsScroll = new QScrollArea;
        lyricsScroll->setObjectName("lyricsScroll");
        lyricsScroll->setWidgetResizable(true);
        lyricsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *outer = new QVBoxLayout(lyricsPage);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(lyricsScroll);
        auto *content = new QWidget;
        lyricsBox = new QVBoxLayout(content);
        lyricsBox->setContentsMargins(56, 48, 56, 240);
        lyricsBox->setSpacing(14);
        lyricsScroll->setWidget(content);
        scrollAnim = new QPropertyAnimation(lyricsScroll->verticalScrollBar(), "value", this);
        scrollAnim->setDuration(350);
        scrollAnim->setEasingCurve(QEasingCurve::OutCubic);
        return lyricsPage;
    }

    QWidget *buildBar() {
        auto *bar = new QWidget;
        bar->setObjectName("bar");
        bar->setFixedHeight(88);
        auto *prev = iconButton(Glyph::Prev, 20, "Previous");
        auto *next = iconButton(Glyph::Next, 20, "Next");
        playPause = new QToolButton;
        playPause->setObjectName("play");
        playPause->setFixedSize(40, 40);
        playPause->setIconSize({20, 20});
        playPause->setToolTip("Play or pause (Ctrl+Space)");
        seek = new QSlider(Qt::Horizontal);
        volume = new QSlider(Qt::Horizontal);
        volume->setRange(0, 100);
        volume->setFixedWidth(110);
        volume->setToolTip("Volume");
        elapsed = label("0:00", "muted");
        total = label("0:00", "muted");
        barCover = new QLabel;
        barCover->setFixedSize(56, 56);
        barTrack = new QLabel;
        barTrack->setTextFormat(Qt::RichText);  // bold title + grey artist, both escaped in showTrack
        barTrack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        devicesButton = iconButton(Glyph::Devices, 20, "Connect to a device");
        lyricsButton = iconButton(Glyph::Mic, 20, "Lyrics");
        likeButton = iconButton(Glyph::HeartOutline, 18, "Save to your Liked Songs");
        addButton = iconButton(Glyph::Plus, 18, "Add to playlist");
        auto *speaker = new QLabel;
        speaker->setPixmap(glyphIcon(Glyph::Volume, theme::subdued, 18).pixmap(18, 18));

        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(14);
        buttons->addStretch();
        buttons->addWidget(prev);
        buttons->addWidget(playPause);
        buttons->addWidget(next);
        buttons->addStretch();
        auto *progress = new QHBoxLayout;
        progress->addWidget(elapsed);
        progress->addWidget(seek, 1);
        progress->addWidget(total);
        auto *center = new QVBoxLayout;
        center->setSpacing(4);
        center->addLayout(buttons);
        center->addLayout(progress);

        auto *left = new QWidget, *right = new QWidget;  // equal widths keep the transport centred
        left->setFixedWidth(320);
        right->setFixedWidth(320);
        auto *l = new QHBoxLayout(left);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(12);
        l->addWidget(barCover);
        l->addWidget(barTrack, 1);
        l->addWidget(likeButton);
        l->addWidget(addButton);
        auto *r = new QHBoxLayout(right);
        r->setContentsMargins(0, 0, 0, 0);
        r->addStretch();
        r->addWidget(lyricsButton);
        r->addWidget(devicesButton);
        r->addSpacing(4);
        r->addWidget(speaker);
        r->addWidget(volume);

        auto *layout = new QHBoxLayout(bar);
        layout->setContentsMargins(20, 8, 20, 10);
        layout->setSpacing(24);
        layout->addWidget(left);
        layout->addLayout(center, 1);
        layout->addWidget(right);

        connect(playPause, &QToolButton::clicked, this, [this] {
            api->send("PUT", playing ? "/me/player/pause" : "/me/player/play");
            setPlaying(!playing);  // optimistic; the next poll corrects it
        });
        connect(devicesButton, &QToolButton::clicked, this, &Window::showDevices);
        connect(lyricsButton, &QToolButton::clicked, this, [this] {
            if (pages->currentWidget() == lyricsPage) return pages->setCurrentIndex(pageBeforeLyrics);
            pageBeforeLyrics = pages->currentIndex();
            pages->setCurrentWidget(lyricsPage);
        });
        connect(likeButton, &QToolButton::clicked, this, [this] { if (!nowUri.isEmpty()) setLiked({nowUri}, !nowLiked); });
        connect(addButton, &QToolButton::clicked, this, [this] {
            if (!nowUri.isEmpty()) showSaveIn({nowUri}, addButton->mapToGlobal(QPoint(0, -8)), true);
        });
        connect(prev, &QToolButton::clicked, this, [this] { api->send("POST", "/me/player/previous", {}, pollSoon()); });
        connect(next, &QToolButton::clicked, this, [this] { api->send("POST", "/me/player/next", {}, pollSoon()); });
        connect(seek, &QSlider::sliderReleased, this, [this] { seekTo(seek->value()); });
        connect(volume, &QSlider::sliderReleased, this,
                [this] { api->send("PUT", QString("/me/player/volume?volume_percent=%1").arg(volume->value())); });
        return bar;
    }

    // ---- library ------------------------------------------------------------

    void loadPlaylists() {
        api->getAll("/me/playlists?limit=50", [this](const QJsonObject &page) {
            for (const auto &v : page["items"].toArray()) {
                auto pl = v.toObject();
                auto *it = navItem(pl["name"].toString(), Playlist, tile(40, 4, dpr(), theme::highlight));
                it->setData(Role::Uri, pl["uri"].toString());
                it->setData(Role::Json, pl);
                setIconAsync(nav, it, imageUrl(pl["images"].toArray(), 80), 40, 4);
            }
            return true;
        });
    }

    // Sets an item's picture once it downloads; the persistent index goes invalid if the row is gone.
    void setIconAsync(QListWidget *list, QListWidgetItem *it, const QString &url, int size, qreal radius) {
        QPersistentModelIndex idx = list->model()->index(list->row(it), 0);
        bool asIcon = list == nav;
        covers.load(url, this, [=](const QPixmap &pm) {
            if (!idx.isValid()) return;
            auto r = rounded(pm, size, radius, dpr());
            list->model()->setData(idx, asIcon ? QVariant(QIcon(r)) : QVariant(r), Qt::DecorationRole);
        });
    }

    void loadAlbums() {
        albumsLoaded = true;
        api->getAll("/me/albums?limit=50", [this](const QJsonObject &page) {
            int n = page["total"].toInt();
            albumsMeta->setText(n ? QString("%1 saved albums").arg(n)
                                  : "No saved albums yet. Albums you save in any Spotify app show up here.");
            for (const auto &v : page["items"].toArray()) {
                auto a = v.toObject()["album"].toObject();
                auto *it = new QListWidgetItem(a["name"].toString(), grid);
                it->setData(Role::Subtitle, joinNames(a["artists"].toArray()));
                it->setData(Role::Json, a);
                it->setData(Role::Image, imageUrl(a["images"].toArray(), CardDelegate::Cover * dpr()));
            }
            QTimer::singleShot(0, this, &Window::syncGridCovers);
            return true;
        });
    }

    // Loading every cover up front cost ~80 MB of heap on a 300-album library (tools/footprint.py).
    // Keeping only covers within a screen of the view holds memory flat at any library size.
    void syncGridCovers() {
        QRect view = grid->viewport()->rect();
        QRect keep = view.adjusted(0, -view.height(), 0, view.height());
        for (int i = 0; i < grid->count(); ++i) {
            auto *it = grid->item(i);
            bool near = grid->visualItemRect(it).intersects(keep), loaded = it->data(Role::Loaded).toBool();
            if (near && !loaded) {
                it->setData(Role::Loaded, true);
                setIconAsync(grid, it, it->data(Role::Image).toString(), CardDelegate::Cover, 6);
            } else if (!near && loaded) {
                it->setData(Role::Loaded, false);
                it->setData(Qt::DecorationRole, QVariant());  // reloads from the disk cache if it comes back
            }
        }
    }

    void openNav(QListWidgetItem *it) {
        Glyph heart = Glyph::Heart;
        switch (it->data(Role::Kind).toInt()) {
        case AlbumsPage:
            pages->setCurrentIndex(0);
            if (!albumsLoaded) loadAlbums();
            break;
        case LikedSongs:
            openList({"Playlist", "Liked Songs", {}, {}, "/me/tracks?limit=50", {},
                      tile(184, 8, dpr(), likedFrom, theme::white, &heart, likedTo)});
            break;
        case Playlist: {
            const auto pl = it->data(Role::Json).toJsonObject();
            const auto owner = pl["owner"].toObject();
            // 2026 API: the track count moved from "tracks" to "items".
            int n = (pl.contains("items") ? pl["items"] : pl["tracks"]).toObject()["total"].toInt();
            bool listable = owner["id"].toString() == myId || pl["collaborative"].toBool();
            QString meta = QString("%1, %2 songs").arg(owner["display_name"].toString(), QString::number(n));
            if (!listable) meta += "\nYou follow this playlist. Spotify only lists songs for playlists you own, so press play to hear it.";
            openList({"Playlist", pl["name"].toString(), meta, imageUrl(pl["images"].toArray(), 368),
                      listable ? "/playlists/" + pl["id"].toString() + "/items?limit=50" : QString(),
                      pl["uri"].toString()});
            break;
        }
        }
    }

    void openAlbum(const QJsonObject &a) {
        if (a.isEmpty()) return;
        nav->setCurrentItem(nullptr);
        int n = a["total_tracks"].toInt();
        QString meta = QString("%1, %2, %3 songs")
                           .arg(joinNames(a["artists"].toArray()), a["release_date"].toString().left(4),
                                QString::number(n));
        openList({"Album", a["name"].toString(), meta, imageUrl(a["images"].toArray(), 368),
                  "/albums/" + a["id"].toString() + "/tracks?limit=50", a["uri"].toString(), {}, true});
    }

    void runSearch(const QString &q) {
        if (q.isEmpty()) return;
        nav->setCurrentItem(nullptr);
        openList({"Search", "“" + q + "”", {}, {}, {}, {}});
        headCover->hide();
        int gen = loadGen;
        // Search is capped at 10 per page since 2026; five pages in parallel, appended in arrival order.
        for (int offset = 0; offset < 50; offset += 10)
            api->get(QString("/search?type=track&limit=10&offset=%1&q=").arg(offset) + QUrl::toPercentEncoding(q),
                     [this, gen](const QJsonObject &o) {
                         if (gen != loadGen) return;
                         addTracks(o["tracks"].toObject()["items"].toArray());
                         headMeta->setText(QString("%1 songs").arg(uris.size()));
                     });
    }

    void openList(const ListInfo &info) {
        pages->setCurrentIndex(1);
        ++loadGen;
        tracks->clear();
        uris.clear();
        context = info.context;
        albumName = info.albumView ? info.title : QString();
        tracks->setColumnHidden(AlbumCol, info.albumView);
        headKind->setText(info.kind);
        headTitle->setText(info.title);
        headMeta->setText(info.meta);
        headCover->show();
        headCover->setPixmap(info.fallback.isNull() ? tile(184, 8, dpr(), theme::highlight) : info.fallback);
        headCoverUrl = info.coverUrl;
        covers.load(info.coverUrl, this, [this, url = info.coverUrl](const QPixmap &pm) {
            if (url == headCoverUrl) headCover->setPixmap(rounded(pm, 184, 8, dpr()));
        });
        if (info.path.isEmpty()) return;
        int gen = loadGen;
        api->getAll(info.path, [this, gen](const QJsonObject &page) {
            if (gen != loadGen) return false;  // user moved on; stop paging
            if (headMeta->text().isEmpty()) headMeta->setText(QString("%1 songs").arg(page["total"].toInt()));
            addTracks(page["items"].toArray());
            return true;
        });
    }

    void addTracks(const QJsonArray &items) {
        QList<QTreeWidgetItem *> rows;
        for (const auto &v : items) {
            auto t = unwrapTrack(v.toObject());
            auto uri = t["uri"].toString();
            if (uri.isEmpty() || t["is_local"].toBool()) continue;  // removed or local files can't stream
            auto album = t["album"].toObject()["name"].toString();
            auto *row = new QTreeWidgetItem({QString::number(uris.size() + 1), t["name"].toString(),
                                             joinNames(t["artists"].toArray()), album.isEmpty() ? albumName : album,
                                             formatTime(t["duration_ms"].toInteger())});
            for (int c : {Num, Artist, AlbumCol, Time}) row->setForeground(c, theme::subdued);
            row->setTextAlignment(Num, Qt::AlignCenter);
            row->setTextAlignment(Time, Qt::AlignRight | Qt::AlignVCenter);
            if (uri == nowUri) row->setForeground(Title, theme::green);
            rows << row;
            uris << uri;
        }
        tracks->addTopLevelItems(rows);  // one batch per page keeps scrolling smooth while loading
    }

    // ---- playback -----------------------------------------------------------

    void playFrom(int row) {
        if (uris.isEmpty()) {
            if (!context.isEmpty()) play({{"context_uri", context}});
            return;
        }
        play(playBody(context, uris, row));
    }

    void play(const QJsonObject &body) {
        // Like Spotify: play where you're listening (maybe your phone), else on ignify itself.
        QString target = activeDeviceId.isEmpty() ? deviceId : activeDeviceId;
        api->send("PUT", "/me/player/play" + (target.isEmpty() ? QString() : "?device_id=" + target), body,
                  pollSoon());
    }

    void seekTo(qint64 ms) {
        progressAt = ms;
        sinceProgress.restart();
        api->send("PUT", QString("/me/player/seek?position_ms=%1").arg(ms));
    }

    Api::Callback pollSoon() {
        return [this](const QJsonObject &) { QTimer::singleShot(400, this, &Window::poll); };
    }

    void setPlaying(bool on) {
        playing = on;
        playPause->setIcon(glyphIcon(on ? Glyph::Pause : Glyph::Play, theme::base, 20));
    }

    void poll() {
        if (api->signingIn()) return;  // don't stack polls behind a browser sign-in
        api->get("/me/player", [this](const QJsonObject &p) {
            setPlaying(p["is_playing"].toBool());
            auto dev = p["device"].toObject();
            activeDeviceId = dev["id"].toString();
            bool remote = !dev.isEmpty() && activeDeviceId != deviceId;
            connectText->setText("Playing on " + dev["name"].toString());
            connectBar->setVisible(remote);
            devicesButton->setIcon(glyphIcon(Glyph::Devices, remote ? theme::green : theme::white, 20));
            volume->setEnabled(dev["supports_volume"].toBool(true));  // some phones refuse remote volume
            auto item = p["item"].toObject();
            if (item.isEmpty()) return showIdle();
            if (item["uri"].toString() != nowUri) showTrack(item);
            seek->setMaximum(item["duration_ms"].toInt());
            total->setText(formatTime(seek->maximum()));
            progressAt = p["progress_ms"].toInt();
            sinceProgress.restart();
            advanceProgress();
            if (!volume->isSliderDown()) volume->setValue(dev["volume_percent"].toInt());
        });
    }

    void advanceProgress() {
        if (seek->isSliderDown()) return elapsed->setText(formatTime(seek->value()));
        qint64 pos = std::min<qint64>(progressAt + (playing ? sinceProgress.elapsed() : 0), seek->maximum());
        seek->setValue(pos);
        elapsed->setText(formatTime(pos));
        if (pages->currentWidget() == lyricsPage && !lyrics.isEmpty()) followLyrics(pos);
    }

    void showTrack(const QJsonObject &item) {
        markRow(nowUri, theme::white);
        nowUri = item["uri"].toString();
        markRow(nowUri, theme::green);
        nowAlbum = item["album"].toObject();
        nowItem = item;
        likeButton->show();
        addButton->show();
        checkLiked();
        loadArtist(item);
        if (pages->currentWidget() == lyricsPage) loadLyrics();
        npTitle->setText(item["name"].toString());
        barTrack->setText(QString("<b>%1</b><br><span style=\"color:#B3B3B3\">%2</span>")
                              .arg(item["name"].toString().toHtmlEscaped(),
                                   joinNames(item["artists"].toArray()).toHtmlEscaped()));
        npArtist->setText(joinNames(item["artists"].toArray()));
        npAlbum->setText(nowAlbum.isEmpty() ? QString()
                                            : "<a href=\"#\" style=\"color:#B3B3B3;text-decoration:none\">" +
                                                  nowAlbum["name"].toString().toHtmlEscaped() + "</a>");
        auto url = imageUrl(nowAlbum["images"].toArray(), 268 * dpr());
        nowCoverUrl = url;
        covers.load(url, this, [this, url](const QPixmap &pm) {
            if (url != nowCoverUrl) return;  // skipped ahead meanwhile
            npCover->setPixmap(rounded(pm, 268, 10, dpr()));
            barCover->setPixmap(rounded(pm, 56, 4, dpr()));
            tintPanel(moodColor(pm));
        });
    }

    void showIdle() {
        if (!nowUri.isEmpty() || npTitle->text().isEmpty()) {
            markRow(nowUri, theme::white);
            nowUri.clear();
            nowCoverUrl.clear();
            Glyph play = Glyph::Play;
            npCover->setPixmap(tile(268, 10, dpr(), theme::highlight, theme::subdued, &play));
            npTitle->setText("Nothing playing");
            npArtist->setText("Double-click any song to play it here.");
            npAlbum->clear();
            barCover->clear();
            barTrack->clear();
            nowItem = {};
            nowArtistId.clear();
            artistCard->hide();
            likeButton->hide();
            addButton->hide();
            seek->setMaximum(0);
            total->setText("0:00");
        }
    }

    // Like Spotify's now-playing view: the cover's colour washes down the panel.
    void tintPanel(const QColor &mood) {
        QColor wash = mood.isValid() ? washFrom(mood) : theme::elevated;
        panel->setStyleSheet(QString("#panel{border-radius:8px;background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                                     "stop:0 %1,stop:0.75 %2)}").arg(wash.name(), theme::base.name()));
        QColor fill = mood.isValid() ? QColor::fromHslF(mood.hslHueF(), std::min(mood.hslSaturationF(), 0.55f), 0.34f)
                                     : QColor("#535353");
        lyricsPage->setStyleSheet(QString("#lyricsPage{border-radius:8px;background:%1}").arg(fill.name()));
    }

    void markRow(const QString &uri, const QColor &c) {
        if (int row = uri.isEmpty() ? -1 : uris.indexOf(uri); row >= 0) tracks->topLevelItem(row)->setForeground(Title, c);
    }

    // ---- audio engine -------------------------------------------------------

    void startLibrespot() {
        auto exe = QStandardPaths::findExecutable("librespot");
        if (exe.isEmpty()) exe = QStandardPaths::findExecutable("librespot", {QDir::homePath() + "/.cargo/bin"});
        if (exe.isEmpty()) {
            statusBar()->showMessage("librespot isn't installed, so ignify is controlling your other devices.", 10'000);
            return;
        }
        auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        // librespot saves a reusable Spotify login here with group/world-readable permissions;
        // a private directory keeps other accounts on the machine out of it.
        QDir().mkpath(cache);
        QFile::setPermissions(cache, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        auto *proc = new QProcess(this);  // dies with the window
        engine = proc;
        proc->setProcessChannelMode(QProcess::MergedChannels);
        // First run: --enable-oauth opens librespot's own browser sign-in; later runs reuse the cache.
        connect(proc, &QProcess::readyRead, this, [proc] {
            while (proc->canReadLine()) qInfo().noquote() << "librespot:" << proc->readLine().trimmed();
        });
        proc->start(exe, {"--name", kDevice, "--cache", cache, "--system-cache", cache, "--bitrate", "320",
                          "--enable-oauth", "--initial-volume", "60", "--enable-volume-normalisation",
                          "--device-type", "computer"});
    }

    void findDevice(int tries) {
        api->get("/me/player/devices", [this, tries](const QJsonObject &o) {
            for (const auto &v : o["devices"].toArray())
                if (v.toObject()["name"] == kDevice) {
                    deviceId = v.toObject()["id"].toString();
                    statusBar()->showMessage("ignify is ready to play", 3000);
                    return;
                }
            if (tries > 0) QTimer::singleShot(2000, this, [this, tries] { findDevice(tries - 1); });
        });
    }

    // Spotify's Connect picker, rebuilt on the public Web API (see README "Connect").
    void showDevices() {
        api->get("/me/player/devices", [this](const QJsonObject &o) {
            QJsonObject current;
            QList<QJsonObject> others;
            for (const auto &v : o["devices"].toArray()) {
                auto d = v.toObject();
                if (d["is_active"].toBool()) current = d;
                else others << d;
            }
            auto name = [this](const QJsonObject &d) {
                return d["id"].toString() == deviceId ? QString("This computer") : d["name"].toString();
            };
            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            if (!current.isEmpty()) {
                auto *row = new QWidget;
                auto *h = new QHBoxLayout(row);
                h->setContentsMargins(12, 8, 22, 8);
                h->setSpacing(12);
                auto *icon = new QLabel;
                icon->setPixmap(glyphIcon(deviceGlyph(current["type"].toString()), theme::green, 28).pixmap(28, 28));
                auto *text = new QVBoxLayout;
                text->setSpacing(0);
                text->addWidget(label("Current device", "muted"));
                text->addWidget(label(name(current), "currentDevice"));
                h->addWidget(icon);
                h->addLayout(text);
                auto *action = new QWidgetAction(menu);
                action->setDefaultWidget(row);
                menu->addAction(action);
                menu->addSeparator();
            }
            menu->addAction(others.isEmpty() ? "No other devices found" : "Select another device")->setEnabled(false);
            for (const auto &d : others) {
                auto *a = menu->addAction(glyphIcon(deviceGlyph(d["type"].toString()), theme::white, 20), name(d));
                a->setEnabled(!d["is_restricted"].toBool());  // the official picker disables these too
                connect(a, &QAction::triggered, this, [this, id = d["id"].toString()] {
                    // No "play" field: keep playing or paused as it was, like the official transfer.
                    api->send("PUT", "/me/player", {{"device_ids", QJsonArray{id}}}, pollSoon());
                });
            }
            QSize size = menu->sizeHint();
            menu->popup(devicesButton->mapToGlobal(QPoint(devicesButton->width() - size.width(), -size.height() - 8)));
        });
    }

    // ---- like and save to playlists ------------------------------------------

    void checkLiked() {
        QString uri = nowUri;
        api->get("/me/library/contains?uris=" + encodedUris({uri}), [this, uri](const QJsonObject &o) {
            if (uri == nowUri) setHeart(o["items"].toArray().at(0).toBool());  // at(): Undefined, not UB, if empty
        });
    }

    void setHeart(bool liked) {
        nowLiked = liked;
        likeButton->setIcon(glyphIcon(liked ? Glyph::Heart : Glyph::HeartOutline, liked ? theme::green : theme::white, 18));
        likeButton->setToolTip(liked ? "Remove from your Liked Songs" : "Save to your Liked Songs");
    }

    void setLiked(const QStringList &songs, bool on) {
        for (int i = 0; i < songs.size(); i += 40)  // the library endpoints take 40 URIs per call
            api->send(on ? "PUT" : "DELETE", "/me/library?uris=" + encodedUris(songs.mid(i, 40)));
        if (songs.contains(nowUri)) setHeart(on);
        statusBar()->showMessage(on ? "Added to Liked Songs" : "Removed from Liked Songs", 3000);
    }

    // Spotify's "Add to playlist" picker: tick one or several of your playlists, then Done.
    void showSaveIn(const QStringList &songs, const QPoint &at, bool above = false) {
        auto *menu = new QMenu(this);
        menu->setObjectName("saveIn");
        menu->setAttribute(Qt::WA_DeleteOnClose);
        auto *box = new QWidget;
        auto *v = new QVBoxLayout(box);
        v->setContentsMargins(12, 10, 12, 12);
        v->setSpacing(10);
        v->addWidget(label(songs.size() == 1 ? QString("Add to playlist") : QString("Add %1 songs to playlist").arg(songs.size()),
                           "artistName"));
        auto *list = new QListWidget(box);  // owned by the menu even when it's left out of the layout
        list->setIconSize({32, 32});
        list->setFixedSize(280, 240);
        list->setSelectionMode(QAbstractItemView::NoSelection);  // a click ticks; no second highlight
        for (int i = 0; i < nav->count(); ++i) {
            auto *src = nav->item(i);
            if (src->data(Role::Kind).toInt() != Playlist) continue;
            const auto pl = src->data(Role::Json).toJsonObject();
            // Spotify only lets you add to playlists you own or collaborate on.
            if (pl["owner"].toObject()["id"].toString() != myId && !pl["collaborative"].toBool()) continue;
            auto *it = new QListWidgetItem(src->icon(), src->text(), list);
            it->setFlags(Qt::ItemIsEnabled);  // the row click toggles, so the box can't double-toggle
            it->setCheckState(Qt::Unchecked);
            it->setSizeHint({0, 42});  // fixed: rows that resize after the first click move under the cursor
            it->setData(Role::Id, pl["id"].toString());
        }
        connect(list, &QListWidget::itemClicked, list, [](QListWidgetItem *it) {
            it->setCheckState(it->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        });
        auto *done = new QPushButton("Done");
        done->setObjectName("done");
        if (list->count() == 0) {
            list->hide();
            v->addWidget(label("You don't have a playlist you can add to yet.", "muted"));
        } else {
            v->addWidget(list);
        }
        v->addWidget(done, 0, Qt::AlignRight);
        auto *action = new QWidgetAction(menu);
        action->setDefaultWidget(box);
        menu->addAction(action);
        connect(done, &QPushButton::clicked, menu, [this, menu, list, songs] {
            QStringList names;
            for (int i = 0; i < list->count(); ++i) {
                auto *it = list->item(i);
                if (it->checkState() != Qt::Checked) continue;
                for (int k = 0; k < songs.size(); k += 100)  // 100 items per call
                    api->send("POST", "/playlists/" + it->data(Role::Id).toString() + "/items",
                              {{"uris", QJsonArray::fromStringList(songs.mid(k, 100))}});
                names << it->text();
            }
            if (!names.isEmpty())
                statusBar()->showMessage(names.size() == 1 ? "Added to " + names[0]
                                                           : QString("Added to %1 playlists").arg(names.size()), 4000);
            menu->close();
        });
        menu->popup(above ? at - QPoint(0, menu->sizeHint().height()) : at);
    }

    void showTrackMenu(const QPoint &pos) {
        QStringList songs;
        for (auto *it : tracks->selectedItems()) songs << uris.value(tracks->indexOfTopLevelItem(it));
        songs.removeAll(QString());
        if (songs.isEmpty()) return;
        QPoint at = tracks->viewport()->mapToGlobal(pos);
        // contains takes 40 URIs; a bigger selection is judged on its first 40
        api->get("/me/library/contains?uris=" + encodedUris(songs.mid(0, 40)), [this, songs, at](const QJsonObject &o) {
            bool allLiked = true;
            for (const auto &v : o["items"].toArray()) allLiked = allLiked && v.toBool();
            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            auto *add = menu->addAction(glyphIcon(Glyph::Plus, theme::white, 18),
                                        songs.size() == 1 ? QString("Add to playlist") : QString("Add %1 songs to playlist").arg(songs.size()));
            connect(add, &QAction::triggered, this, [this, songs, at] { showSaveIn(songs, at); });
            auto *like = menu->addAction(glyphIcon(allLiked ? Glyph::Heart : Glyph::HeartOutline, allLiked ? theme::green : theme::white, 18),
                                         allLiked ? "Remove from your Liked Songs" : "Save to your Liked Songs");
            connect(like, &QAction::triggered, this, [this, songs, allLiked] { setLiked(songs, !allLiked); });
            menu->popup(at);
        });
    }

    // ---- artist card and lyrics ------------------------------------------------

    // Plain GET for the keyless services; an empty document on any failure.
    void fetchJson(const QUrl &url, std::function<void(const QJsonDocument &)> done) {
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader, "ignify/0.1 (personal desktop music client)");
        auto *reply = web.get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, done] {
            reply->deleteLater();
            done(reply->error() ? QJsonDocument() : QJsonDocument::fromJson(reply->readAll()));
        });
    }

    void loadArtist(const QJsonObject &item) {
        auto artists = item["artists"].toArray();
        QString id = artists.isEmpty() ? QString() : artists.first().toObject()["id"].toString();
        if (id == nowArtistId) return;
        nowArtistId = id;
        artistCard->hide();
        if (id.isEmpty()) return;  // podcasts have no artist
        api->get("/artists/" + id, [this, id](const QJsonObject &a) {
            if (id != nowArtistId) return;
            QStringList genres;
            for (const auto &g : a["genres"].toArray()) genres << g.toString();
            artistName->setText(a["name"].toString());
            artistGenres->setText(genres.mid(0, 3).join(", "));
            artistGenres->setVisible(!genres.isEmpty());
            artistBio->hide();
            artistSource->hide();
            artistPhoto->setPixmap(tile(268, 8, dpr(), theme::highlight).copy(0, 0, 268 * dpr(), 170 * dpr()));
            covers.load(imageUrl(a["images"].toArray(), 268 * dpr()), this, [this, id](const QPixmap &pm) {
                if (id == nowArtistId) artistPhoto->setPixmap(rounded(pm, QSize(268, 170), 8, dpr()));
            });
            artistCard->show();
            QString name = a["name"].toString();
            loadBio(id, {name, name + " (musician)", name + " (band)", name + " (singer)", name + " (rapper)"});
        });
    }

    // Try likely Wikipedia titles in turn; accept the first whose description says "musician".
    void loadBio(const QString &artistId, QStringList titles) {
        if (titles.isEmpty()) return;  // no confident match: the card keeps photo, name and genres
        QString title = titles.takeFirst().replace(' ', '_');
        fetchJson(QUrl(kWiki + "/api/rest_v1/page/summary/" + QUrl::toPercentEncoding(title)),
                  [this, artistId, titles](const QJsonDocument &doc) {
                      if (artistId != nowArtistId) return;
                      auto o = doc.object();
                      if (o["type"].toString() != "standard" || !looksLikeMusician(o["description"].toString()))
                          return loadBio(artistId, titles);
                      QUrl page(o["content_urls"].toObject()["desktop"].toObject()["page"].toString());
                      artistBio->setText(clip(o["extract"].toString(), 280));
                      // The link opens in the browser: only ever an https Wikipedia page.
                      bool wikiLink = page.scheme() == "https" && page.host().endsWith(".wikipedia.org");
                      artistSource->setText(wikiLink ? QString("<a href=\"%1\" style=\"color:#B3B3B3\">From Wikipedia</a>")
                                                           .arg(page.toString(QUrl::FullyEncoded).toHtmlEscaped())
                                                     : QString("From Wikipedia"));
                      artistBio->show();
                      artistSource->show();
                  });
    }

    void loadLyrics() {
        if (nowItem.isEmpty() || lyricsFor == nowUri) return;
        lyricsFor = nowUri;
        lyrics.clear();
        fillLyrics({}, "Loading lyrics…", false);
        auto artists = nowItem["artists"].toArray();
        auto q = [](const char *k, const QString &v) { return QString(k) + "=" + QUrl::toPercentEncoding(v); };
        QString track = q("track_name", nowItem["name"].toString()) + "&" +
                        q("artist_name", artists.isEmpty() ? QString() : artists.first().toObject()["name"].toString());
        QString exact = track + "&" + q("album_name", nowItem["album"].toObject()["name"].toString()) + "&" +
                        q("duration", QString::number(nowItem["duration_ms"].toInt() / 1000));
        QString uri = nowUri;
        auto show = [this, uri](const QJsonObject &o) {
            if (uri != lyricsFor) return;
            if (o["instrumental"].toBool()) return fillLyrics({}, "This song is instrumental.", false);
            lyrics = parseLrc(o["syncedLyrics"].toString());
            QStringList lines;
            for (const auto &l : lyrics) lines << l.text;
            if (!lines.isEmpty()) return fillLyrics(lines, "Lyrics from LRCLIB", true);
            auto plain = o["plainLyrics"].toString();
            if (!plain.isEmpty())
                return fillLyrics(plain.split('\n'), "These lyrics aren't synced to the song. Lyrics from LRCLIB", false);
            fillLyrics({}, "No lyrics found for this song.", false);
        };
        // Exact match first (title, artist, album, length); then a looser search.
        fetchJson(QUrl(kLyrics + "/api/get?" + exact), [this, track, show](const QJsonDocument &d) {
            if (!d.object().isEmpty()) return show(d.object());
            fetchJson(QUrl(kLyrics + "/api/search?" + track), [show](const QJsonDocument &s) {
                QJsonObject best;
                for (const auto &v : s.array())
                    if (!v.toObject()["syncedLyrics"].toString().isEmpty() || best.isEmpty()) best = v.toObject();
                show(best);
            });
        });
    }

    void fillLyrics(const QStringList &lines, const QString &note, bool synced) {
        while (auto *item = lyricsBox->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        lyricLabels.clear();
        currentLyric = -1;
        for (int i = 0; i < lines.size(); ++i) {
            auto *l = label(lines[i].trimmed().isEmpty() ? QString("♪") : lines[i], "lyric");
            l->setWordWrap(true);
            l->setStyleSheet(synced ? "color: rgba(0,0,0,0.55)" : "color: white");
            if (synced) {  // click a line to jump there, like Spotify
                l->setProperty("ms", lyrics[i].ms);
                l->setCursor(Qt::PointingHandCursor);
                l->installEventFilter(this);
            }
            lyricsBox->addWidget(l);
            lyricLabels << l;
        }
        lyricsBox->addSpacing(24);
        lyricsBox->addWidget(label(note, "lyricsNote"));
        lyricsBox->addStretch();
        lyricsScroll->verticalScrollBar()->setValue(0);
    }

    // Restyle only the lines whose state changed, then glide the sung line to a third down.
    void followLyrics(qint64 pos) {
        int now = lineAt(lyrics, pos);
        if (now == currentLyric || lyricLabels.size() != lyrics.size()) return;
        int lo = std::max(0, std::min(now, currentLyric)), hi = std::max(now, currentLyric);
        for (int k = lo; k <= hi && k < lyricLabels.size(); ++k)
            lyricLabels[k]->setStyleSheet(k < now ? "color: rgba(255,255,255,0.65)"
                                                  : k == now ? "color: white" : "color: rgba(0,0,0,0.55)");
        currentLyric = now;
        if (now < 0) return;
        scrollAnim->stop();
        scrollAnim->setStartValue(lyricsScroll->verticalScrollBar()->value());
        scrollAnim->setEndValue(std::max(0, lyricLabels[now]->y() - lyricsScroll->viewport()->height() / 3));
        scrollAnim->start();
    }

    bool eventFilter(QObject *o, QEvent *e) override {
        if (e->type() == QEvent::MouseButtonRelease && o->property("ms").isValid()) {
            seekTo(o->property("ms").toLongLong());
            return true;
        }
        return QMainWindow::eventFilter(o, e);
    }

    Api *api;
    ImageCache covers;
    QLineEdit *search;
    QListWidget *nav, *grid;
    QStackedWidget *pages;
    QLabel *albumsMeta, *headCover, *headKind, *headTitle, *headMeta;
    QToolButton *headPlay, *playPause, *devicesButton, *lyricsButton, *likeButton, *addButton;
    QNetworkAccessManager web;  // lyrics + Wikipedia
    QFrame *artistCard, *lyricsPage;
    QLabel *artistPhoto, *artistName, *artistGenres, *artistBio, *artistSource;
    QScrollArea *lyricsScroll;
    QVBoxLayout *lyricsBox;
    QPropertyAnimation *scrollAnim;
    QList<QLabel *> lyricLabels;
    QList<LyricLine> lyrics;
    QString nowArtistId, lyricsFor;
    QJsonObject nowItem;
    int currentLyric = -1, pageBeforeLyrics = 0;
    bool nowLiked = false;
    QProcess *engine = nullptr;
    QFrame *connectBar;
    QLabel *connectText, *barCover, *barTrack;
    QTreeWidget *tracks;
    QFrame *panel;
    QLabel *npCover, *npTitle, *npArtist, *npAlbum, *elapsed, *total;
    QSlider *seek, *volume;
    QTimer pollTimer, tick;
    QElapsedTimer sinceProgress;
    qint64 progressAt = 0;
    QString context, albumName, headCoverUrl, nowUri, nowCoverUrl, deviceId, activeDeviceId, myId;
    QJsonObject nowAlbum;
    QStringList uris;
    int loadGen = 0;
    bool playing = false, albumsLoaded = false;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QLoggingCategory::setFilterRules("qt.gui.icc=false");  // some covers carry ICC profiles Qt grumbles about
    app.setOrganizationName("ignify");
    app.setApplicationName("ignify");
    theme::apply(app);
    app.setWindowIcon(QIcon(spotifyLogo(256, 1)));

    QSettings settings;
    QDir().mkpath(QFileInfo(settings.fileName()).path());
    QFile::setPermissions(QFileInfo(settings.fileName()).path(),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    static const QRegularExpression clientIdShape("^[0-9a-f]{32}$");
    while (!clientIdShape.match(settings.value("clientId").toString()).hasMatch()) {
        bool ok;
        auto id = QInputDialog::getText(nullptr, "Connect ignify to Spotify",
                                        "Paste the Client ID of your app from developer.spotify.com/dashboard.\n"
                                        "Its redirect URI must be http://127.0.0.1:8888/callback.",
                                        QLineEdit::Normal, {}, &ok)
                      .trimmed();
        if (!ok) return 1;
        settings.setValue("clientId", id);
    }

    Api api;
    Window window(&api);
    window.resize(1440, 880);
    window.show();
    QObject::connect(&api, &Api::ready, &window, [&window] { window.start(); });
    api.login();
    return app.exec();
}
