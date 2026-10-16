#include "api.h"
#include "mpris.h"
#include "ui.h"
#include "util.h"
#include "views.h"
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
enum Kind { AlbumsPage = 1, LikedSongs, Playlist, HomePage };

// Web API device "type" → the icon Spotify's picker shows for it.
static Glyph deviceGlyph(const QString &type) {
    if (type == "Computer") return Glyph::Computer;
    if (type == "Smartphone" || type == "Tablet") return Glyph::Phone;
    return Glyph::Speaker;
}
enum Col { Num, Title, AlbumCol, Added, Time };

// A frame that runs a callback when clicked: Home's shortcut tiles.
class Clickable : public QFrame {
public:
    std::function<void()> onClick;

protected:
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (rect().contains(e->position().toPoint()) && onClick) onClick();
    }
};

struct ListInfo {
    QString kind, title, owner, coverUrl, path, context;
    QString lead;          // first part of the meta line, rich text, already escaped (owner or artist links)
    QStringList extra;     // further meta parts, escaped (an album's year)
    int total = -1;        // song count when known up front
    QPixmap fallback;      // shown when there's no cover url (Liked Songs)
    bool albumView = false;  // album tracks: no thumbnails, album or date columns
    bool dated = false;      // show "Date added" (playlists, Liked Songs)
    QString cacheId;         // lists seen before show at once from the cache; empty = never cached
    bool fixed = false;      // an album's songs never change, so a cached copy isn't checked again
    QColor tint;             // header colour before a cover arrives (Liked Songs' purple)
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
        pages->addWidget(buildHomePage());
        pages->addWidget(buildArtistPage());
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
        buildShortcuts();
        wireMpris();
        sinceProgress.start();
        setPlaying(false);
        nav->setFocus();  // otherwise search grabs focus and shows its ring on launch
        showIdle();
        tintPanel({});
        updateHistoryButtons();
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
            myName = me["display_name"].toString(myId);
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
        backButton = iconButton(Glyph::Back, 18, "Go back (Alt+Left)");
        forwardButton = iconButton(Glyph::Forward, 18, "Go forward (Alt+Right)");
        brand->addWidget(backButton);
        brand->addWidget(forwardButton);
        connect(backButton, &QToolButton::clicked, this, [this] { stepHistory(-1); });
        connect(forwardButton, &QToolButton::clicked, this, [this] { stepHistory(1); });
        layout->addLayout(brand);
        layout->addWidget(search);

        // Spotify's "Your Library" header: filter, sort and create.
        auto *libRow = new QHBoxLayout;
        libRow->setContentsMargins(6, 4, 0, 0);
        libRow->setSpacing(2);
        libRow->addWidget(label("Your Library", "artistName"));
        libRow->addStretch();
        auto *filterButton = iconButton(Glyph::Search, 16, "Search in Your Library");
        sortButton = iconButton(Glyph::Sort, 16, "Sort: Recents");
        auto *createButton = iconButton(Glyph::Plus, 18, "Create playlist (Ctrl+N)");
        libRow->addWidget(filterButton);
        libRow->addWidget(sortButton);
        libRow->addWidget(createButton);
        layout->addLayout(libRow);
        libFilter = new QLineEdit;
        libFilter->setObjectName("libFilter");
        libFilter->setPlaceholderText("Search in Your Library");
        libFilter->setClearButtonEnabled(true);
        libFilter->hide();
        layout->addWidget(libFilter);
        layout->addWidget(nav, 1);
        connect(filterButton, &QToolButton::clicked, this, [this] {
            libFilter->setVisible(!libFilter->isVisible());
            if (libFilter->isVisible()) libFilter->setFocus();
            else libFilter->clear();
        });
        connect(libFilter, &QLineEdit::textChanged, this, &Window::applyLibraryView);
        connect(sortButton, &QToolButton::clicked, this, [this] {
            QMenu menu(this);
            for (auto [key, text] : {std::pair{"recents", "Recents"}, {"alpha", "Alphabetical"}, {"creator", "Creator"}}) {
                auto *a = menu.addAction(text);
                a->setCheckable(true);
                a->setChecked(librarySort == key);
                connect(a, &QAction::triggered, this, [this, key = QString(key), text = QString(text)] {
                    librarySort = key;
                    sortButton->setToolTip("Sort: " + text);
                    applyLibraryView();
                });
            }
            menu.exec(sortButton->mapToGlobal(QPoint(0, sortButton->height())));
        });
        connect(createButton, &QToolButton::clicked, this, [this] { createPlaylist({}); });

        Glyph grid = Glyph::Grid, heart = Glyph::Heart, home = Glyph::Home;
        navItem("Home", HomePage, tile(40, 4, dpr(), theme::highlight, theme::white, &home));
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
        connect(nav, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) { if (it) go(navView(it)); });
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
        layout->addSpacing(10);
        auto *tools = new QHBoxLayout;
        albumFilter = new QLineEdit;
        albumFilter->setObjectName("libFilter");
        albumFilter->setPlaceholderText("Filter albums");
        albumFilter->setClearButtonEnabled(true);
        albumFilter->setFixedWidth(240);
        albumSortButton = new QPushButton("Recently added");
        albumSortButton->setObjectName("ghost");
        tools->addWidget(albumFilter);
        tools->addStretch();
        tools->addWidget(albumSortButton);
        tools->addSpacing(16);
        layout->addLayout(tools);
        layout->addSpacing(6);
        layout->addWidget(grid, 1);
        connect(albumFilter, &QLineEdit::textChanged, this, &Window::applyAlbumView);
        connect(albumSortButton, &QPushButton::clicked, this, [this] {
            QMenu menu(this);
            for (auto [key, text] : {std::pair{"recent", "Recently added"}, {"alpha", "Alphabetical"}, {"artist", "Artist"}}) {
                auto *a = menu.addAction(text);
                a->setCheckable(true);
                a->setChecked(albumSort == key);
                connect(a, &QAction::triggered, this, [this, key = QString(key), text = QString(text)] {
                    albumSort = key;
                    albumSortButton->setText(text);
                    applyAlbumView();
                });
            }
            menu.exec(albumSortButton->mapToGlobal(QPoint(0, albumSortButton->height())));
        });
        // Covers load as they scroll near the view and are dropped two screens away (syncGridCovers).
        connect(grid->verticalScrollBar(), &QScrollBar::valueChanged, this, &Window::syncGridCovers);
        connect(grid->verticalScrollBar(), &QScrollBar::rangeChanged, this, &Window::syncGridCovers);
        connect(grid, &QListWidget::itemActivated, this,
                [this](QListWidgetItem *it) { openAlbum(it->data(Role::Json).toJsonObject()); });
        connect(grid, &QListWidget::itemClicked, this,
                [this](QListWidgetItem *it) { openAlbum(it->data(Role::Json).toJsonObject()); });
        return page;
    }

    // Spotify's playlist page: a header band in the cover's colour (cover, title sized to fit,
    // owner and length), an action row, then the song table. Header and table scroll together.
    QWidget *buildListPage() {
        QVBoxLayout *layout;
        listPage = scrollPage("listScroll", layout);
        layout->setContentsMargins(0, 0, 0, 24);
        layout->setSpacing(0);

        headBand = new QFrame;
        headBand->setObjectName("listHead");
        auto *band = new QVBoxLayout(headBand);
        band->setContentsMargins(28, 28, 24, 16);
        band->setSpacing(26);
        headCover = new QLabel;
        headCover->setFixedSize(232, 232);
        auto *shadow = new QGraphicsDropShadowEffect(headCover);
        shadow->setBlurRadius(48);
        shadow->setOffset(0, 8);
        shadow->setColor(QColor(0, 0, 0, 150));
        headCover->setGraphicsEffect(shadow);
        headKind = label({}, "kind");
        headTitle = label({}, "listTitle");
        headTitle->setTextFormat(Qt::PlainText);
        headTitle->setWordWrap(true);
        headTitle->installEventFilter(this);  // re-fit the title size when the column width changes
        headMeta = label({}, "muted");
        headMeta->setTextFormat(Qt::RichText);  // owner/artist in bold, artist links; every part escaped
        headMeta->setWordWrap(true);
        headMeta->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        connect(headMeta, &QLabel::linkActivated, this, &Window::openLink);
        auto *text = new QVBoxLayout;
        text->setSpacing(8);
        text->addStretch();
        text->addWidget(headKind);
        text->addWidget(headTitle);
        text->addWidget(headMeta);
        auto *head = new QHBoxLayout;
        head->setSpacing(24);
        head->addWidget(headCover, 0, Qt::AlignLeft | Qt::AlignBottom);
        head->addLayout(text, 1);
        band->addLayout(head);

        headPlay = new QToolButton;
        headPlay->setObjectName("bigPlay");
        headPlay->setIcon(glyphIcon(Glyph::Play, theme::base, 26));
        headPlay->setIconSize({26, 26});
        headPlay->setFixedSize(56, 56);
        headPlay->setToolTip("Play");
        headShuffle = new QToolButton;
        headShuffle->setIconSize({28, 34});
        headMore = iconButton(Glyph::More, 28, "More options");
        auto *findButton = iconButton(Glyph::Search, 20, "Search in this list");
        listFilter = new QLineEdit;
        listFilter->setObjectName("libFilter");
        listFilter->setPlaceholderText("Search in playlist");
        listFilter->setClearButtonEnabled(true);
        listFilter->setFixedWidth(220);
        listFilter->hide();
        listSortButton = new QPushButton("Custom order");
        listSortButton->setObjectName("sortText");
        listSortButton->setIcon(glyphIcon(Glyph::Sort, theme::subdued, 16));
        listSortButton->setLayoutDirection(Qt::RightToLeft);  // label, then the icon, as in Spotify
        auto *actions = new QHBoxLayout;
        actions->setSpacing(18);
        actions->addWidget(headPlay);
        actions->addWidget(headShuffle);
        actions->addWidget(headMore);
        actions->addStretch();
        actions->addWidget(listFilter);
        actions->addWidget(findButton);
        actions->addWidget(listSortButton);
        band->addLayout(actions);
        layout->addWidget(headBand);

        // Shown instead of an empty table when Spotify refuses to list the songs.
        listNote = new QFrame;
        auto *note = new QVBoxLayout(listNote);
        note->setContentsMargins(28, 22, 28, 6);
        note->setSpacing(6);
        noteTitle = label({}, "noteTitle");
        noteTitle->setWordWrap(true);
        noteBody = label({}, "muted");
        noteBody->setWordWrap(true);
        note->addWidget(noteTitle);
        note->addWidget(noteBody);
        listNote->hide();
        layout->addWidget(listNote);

        tracks = new QTreeWidget;
        tracks->setObjectName("trackTable");
        tracks->setHeaderLabels({"#", "Title", "Album", "Date added", ""});
        tracks->headerItem()->setIcon(Time, glyphIcon(Glyph::Clock, theme::subdued, 16));
        tracks->setToolTip({});
        tracks->setRootIsDecorated(false);
        tracks->setUniformRowHeights(true);  // Qt skips measuring every row: 5000 songs stay instant
        tracks->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // the page scrolls, header and all
        tracks->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tracks->setMouseTracking(true);
        tracks->setSelectionMode(QAbstractItemView::ExtendedSelection);  // shift/ctrl-click several songs
        tracks->setContextMenuPolicy(Qt::CustomContextMenu);
        titleDelegate = new TrackTitleDelegate(tracks);
        tracks->viewport()->installEventFilter(this);
        tracks->setItemDelegateForColumn(Title, titleDelegate);
        auto *h = tracks->header();
        h->setStretchLastSection(false);
        h->setSectionResizeMode(QHeaderView::Stretch);
        for (auto [col, width] : {std::pair{int(Num), 48}, {int(Added), 150}, {int(Time), 70}}) {
            h->setSectionResizeMode(col, QHeaderView::Fixed);  // ResizeToContents would rescan every row
            h->resizeSection(col, width);
        }
        tracks->headerItem()->setTextAlignment(Time, Qt::AlignRight | Qt::AlignVCenter);
        auto *tableBox = new QWidget;
        auto *tableLayout = new QHBoxLayout(tableBox);
        tableLayout->setContentsMargins(28, 0, 24, 0);
        tableLayout->addWidget(tracks);
        layout->addWidget(tableBox);
        layout->addStretch(1);  // a short list leaves the spare height here, not in the header band

        connect(tracks, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
            QStringList songs;
            for (auto *it : tracks->selectedItems()) songs << it->data(Num, Role::Uri).toString();
            trackMenu(songs, tracks->viewport()->mapToGlobal(pos));
        });
        // The grey artist line under a title opens the artist, as Spotify's links do.
        connect(tracks, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it, int column) {
            if (column != Title) return;
            QRect cell = tracks->visualRect(tracks->indexFromItem(it, Title));
            QPoint at = tracks->viewport()->mapFromGlobal(QCursor::pos());
            int textX = cell.x() + 4 + (titleDelegate->thumbs ? 52 : 0);
            int textW = QFontMetrics(tracks->font()).horizontalAdvance(it->data(Title, Role::Subtitle).toString());
            if (at.y() >= cell.y() + 28 && at.x() >= textX && at.x() <= textX + textW)
                openArtistOf(it->data(Title, Role::Json).toJsonArray(), QCursor::pos());
        });
        connect(tracks, &QTreeWidget::itemActivated, this,
                [this](QTreeWidgetItem *it) { playFrom(tracks->indexOfTopLevelItem(it)); });
        connect(headPlay, &QToolButton::clicked, this, [this] { playFrom(0); });
        connect(headShuffle, &QToolButton::clicked, this, [this] { setShuffle(!shuffleOn); });
        connect(headMore, &QToolButton::clicked, this, [this] {
            QMenu menu(this);
            auto *queue = menu.addAction(glyphIcon(Glyph::Queue, theme::white, 18), "Add to queue");
            auto *add = menu.addAction(glyphIcon(Glyph::Plus, theme::white, 18), "Add to playlist");
            queue->setEnabled(!uris.isEmpty());
            add->setEnabled(!uris.isEmpty());
            QPoint at = headMore->mapToGlobal(QPoint(0, headMore->height()));
            QAction *chosen = menu.exec(at);
            if (chosen == queue) queueSongs(uris, 0);
            if (chosen == add) showSaveIn(uris, at);
        });
        connect(findButton, &QToolButton::clicked, this, [this] {
            listFilter->setVisible(!listFilter->isVisible());
            if (listFilter->isVisible()) listFilter->setFocus();
            else listFilter->clear();
        });
        connect(listFilter, &QLineEdit::textChanged, this, &Window::applyListView);
        connect(listSortButton, &QPushButton::clicked, this, [this] {
            QMenu menu(this);
            for (auto [order, name] : {std::pair{"custom", "Custom order"}, {"title", "Title"}, {"artist", "Artist"},
                                       {"album", "Album"}, {"added", "Date added"}, {"duration", "Duration"}}) {
                if (QString(order) == "added" && tracks->isColumnHidden(Added)) continue;
                auto *a = menu.addAction(name);
                a->setCheckable(true);
                a->setChecked(listSort == order);
                connect(a, &QAction::triggered, this, [this, order = QString(order), name = QString(name)] {
                    listSort = order;
                    listSortButton->setText(name);
                    applyListView();
                });
            }
            menu.exec(listSortButton->mapToGlobal(QPoint(0, listSortButton->height())));
        });
        connect(listPage->verticalScrollBar(), &QScrollBar::valueChanged, this, &Window::syncTrackThumbs);
        return listPage;
    }

    // Spotify sizes a playlist title to fit: huge for short names, smaller for long ones.
    void fitTitle() {
        int width = headTitle->width(), px = 32;
        if (width < 50) return;
        QFont f("Inter Display");
        f.setWeight(QFont::ExtraBold);
        for (int size : {96, 72, 56, 40}) {
            f.setPixelSize(size);
            // Half an em of slack: colour emoji render a little wider than their measured advance.
            if (QFontMetrics(f).horizontalAdvance(headTitle->text()) + size / 2 <= width) {
                px = size;
                break;
            }
        }
        // Through the stylesheet: a plain setFont is undone when the page is first polished.
        if (headTitle->property("px").toInt() != px) {
            headTitle->setProperty("px", px);
            headTitle->setStyleSheet(QString("font-size: %1px;").arg(px));
        }
    }

    void setHeadTint(const QColor &c) {
        headBand->setStyleSheet(QString("#listHead{background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %1,stop:1 %2)}")
                                    .arg(c.name(), theme::base.name()));
    }

    // "Owner • 2015 • 45 songs, 3 hr 39 min", owner/artist bold, as in Spotify's header.
    void refreshMeta() {
        QStringList parts;
        if (!listLead.isEmpty()) parts << "<b style=\"color:#FFFFFF\">" + listLead + "</b>";
        parts << listExtra;
        int count = listTotal >= 0 ? listTotal : int(uris.size());
        QString songs = formatCount(count) + (count == 1 ? " song" : " songs");
        if (listDurationMs > 0 && !listBlocked) songs += ", " + formatLength(listDurationMs);
        if (listTotal >= 0 || count > 0) parts << songs;  // not "0 songs" while the first page loads
        headMeta->setText(parts.join(" &#8226; "));
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
        sideStack = new QStackedWidget;  // Spotify swaps the right panel between now playing and the queue
        sideStack->setObjectName("sideStack");
        sideStack->addWidget(scroll);
        sideStack->addWidget(buildQueuePanel());
        outer->addWidget(sideStack);
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
        npArtist->setTextFormat(Qt::RichText);  // artist links, names escaped by artistLinks()
        npArtist->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        connect(npArtist, &QLabel::linkActivated, this, &Window::openLink);
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
        artistName->setTextFormat(Qt::RichText);  // links to the artist page; name escaped
        artistName->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        connect(artistName, &QLabel::linkActivated, this, &Window::openLink);
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
        auto *prev = prevButton = iconButton(Glyph::Prev, 20, "Previous (Ctrl+Left)");
        auto *next = nextButton = iconButton(Glyph::Next, 20, "Next (Ctrl+Right)");
        shuffleButton = new QToolButton;
        repeatButton = new QToolButton;
        for (auto *b : {shuffleButton, repeatButton}) b->setIconSize({20, 26});
        playPause = new QToolButton;
        playPause->setObjectName("play");
        playPause->setFixedSize(40, 40);
        playPause->setIconSize({20, 20});
        playPause->setToolTip("Play or pause (Space)");
        seek = new QSlider(Qt::Horizontal);
        volume = new QSlider(Qt::Horizontal);
        volume->setRange(0, 100);
        volume->setFixedWidth(90);
        volume->setToolTip("Volume");
        elapsed = label("0:00", "muted");
        total = label("0:00", "muted");
        barCover = new QLabel;
        barCover->setFixedSize(56, 56);
        barTrack = new QLabel;
        barTrack->setTextFormat(Qt::RichText);  // bold title + grey artist, both escaped in showTrack
        barTrack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        barTrack->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        connect(barTrack, &QLabel::linkActivated, this, &Window::openLink);
        devicesButton = iconButton(Glyph::Devices, 20, "Connect to a device");
        lyricsButton = iconButton(Glyph::Mic, 20, "Lyrics");
        queueButton = new QToolButton;
        queueButton->setIconSize({20, 26});
        queueButton->setIcon(toggleIcon(Glyph::Queue, false, 20));
        queueButton->setToolTip("Queue");
        miniButton = new QToolButton;
        miniButton->setIconSize({18, 24});
        miniButton->setIcon(toggleIcon(Glyph::MiniPlayer, false, 18));
        miniButton->setToolTip("Open mini player (Ctrl+M)");
        fullButton = iconButton(Glyph::FullScreen, 18, "Full screen (F11)");
        likeButton = iconButton(Glyph::HeartOutline, 18, "Save to your Liked Songs");
        addButton = iconButton(Glyph::Plus, 18, "Add to playlist");
        auto *speaker = new QLabel;
        speaker->setPixmap(glyphIcon(Glyph::Volume, theme::subdued, 18).pixmap(18, 18));

        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(14);
        buttons->addStretch();
        buttons->addWidget(shuffleButton);
        buttons->addWidget(prev);
        buttons->addWidget(playPause);
        buttons->addWidget(next);
        buttons->addWidget(repeatButton);
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
        r->setSpacing(2);
        r->addWidget(lyricsButton);
        r->addWidget(queueButton);
        r->addWidget(devicesButton);
        r->addSpacing(4);
        r->addWidget(speaker);
        r->addWidget(volume);
        r->addSpacing(6);
        r->addWidget(miniButton);
        r->addWidget(fullButton);

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
        connect(shuffleButton, &QToolButton::clicked, this, [this] { setShuffle(!shuffleOn); });
        connect(repeatButton, &QToolButton::clicked, this, [this] { setRepeat(nextRepeat(repeatState)); });
        connect(queueButton, &QToolButton::clicked, this, [this] { toggleQueue(sideStack->currentIndex() == 0); });
        connect(miniButton, &QToolButton::clicked, this, &Window::toggleMini);
        connect(fullButton, &QToolButton::clicked, this, &Window::toggleFull);
        updateModeButtons();
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
        connect(volume, &QSlider::sliderReleased, this, [this] { setVolumePercent(volume->value()); });
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
                it->setData(Role::Order, playlistOrder++);  // API order = Spotify's "Recents"
                setIconAsync(nav, it, imageUrl(pl["images"].toArray(), 80), 40, 4);
            }
            applyLibraryView();
            return true;
        });
    }

    // Sets an item's picture once it downloads. Rows can be re-ordered meanwhile (library sort and
    // filter take them out and put them back), which invalidates the index: then the row is found
    // again by the URL it carries. A row that is gone for good simply isn't found.
    void setIconAsync(QListWidget *list, QListWidgetItem *it, const QString &url, int size, qreal radius) {
        it->setData(Role::Image, url);
        QPersistentModelIndex idx = list->model()->index(list->row(it), 0);
        bool asIcon = list == nav;
        covers.load(url, list, [=](const QPixmap &pm) {
            auto r = rounded(pm, size, radius, dpr());
            QVariant picture = asIcon ? QVariant(QIcon(r)) : QVariant(r);
            if (idx.isValid()) return void(list->model()->setData(idx, picture, Qt::DecorationRole));
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Role::Image).toString() == url) list->item(i)->setData(Qt::DecorationRole, picture);
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
                it->setData(Role::Order, grid->count());
            }
            applyAlbumView();
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

    std::function<void()> navView(QListWidgetItem *it) {
        int kind = it->data(Role::Kind).toInt();
        QJsonObject pl = it->data(Role::Json).toJsonObject();
        return [this, kind, pl] { showNav(kind, pl); };
    }

    void showNav(int kind, const QJsonObject &pl) {
        selectNav(kind, pl["id"].toString());
        Glyph heart = Glyph::Heart;
        switch (kind) {
        case HomePage:
            pages->setCurrentWidget(homePage);
            if (!homeLoadedAt.isValid() || homeLoadedAt.elapsed() > 120'000) loadHome();
            break;
        case AlbumsPage:
            pages->setCurrentIndex(0);
            if (!albumsLoaded) loadAlbums();
            break;
        case LikedSongs: {
            ListInfo info;
            info.kind = "Playlist";
            info.title = "Liked Songs";
            info.lead = myName.toHtmlEscaped();
            info.path = "/me/tracks?limit=50";
            info.cacheId = "liked";
            info.fallback = tile(232, 6, dpr(), likedFrom, theme::white, &heart, likedTo);
            info.dated = true;
            info.tint = QColor("#5038A0");  // Spotify's Liked Songs header
            openList(info);
            break;
        }
        case Playlist: {
            const auto owner = pl["owner"].toObject();
            ListInfo info;
            info.kind = pl["public"].toBool() ? "Public Playlist" : "Playlist";
            info.title = pl["name"].toString();
            info.owner = owner["display_name"].toString();
            info.lead = info.owner.toHtmlEscaped();
            // 2026 API: the track count moved from "tracks" to "items".
            info.total = (pl.contains("items") ? pl["items"] : pl["tracks"]).toObject()["total"].toInt(-1);
            info.coverUrl = imageUrl(pl["images"].toArray(), 464);
            // Always ask; for a playlist you only follow Spotify answers 403, and that is what we show.
            info.path = "/playlists/" + pl["id"].toString() + "/items?limit=50";
            info.cacheId = "playlist:" + pl["id"].toString();
            info.context = pl["uri"].toString();
            info.dated = true;
            openList(info);
            break;
        }
        }
    }

    void openAlbum(const QJsonObject &a) {
        if (!a.isEmpty()) go([this, a] { showAlbum(a); });
    }

    void showAlbum(const QJsonObject &a) {
        selectNav(0, {});
        QString type = a["album_type"].toString();
        ListInfo info;
        info.kind = type == "single" ? "Single" : type == "compilation" ? "Compilation" : "Album";
        info.title = a["name"].toString();
        info.lead = artistLinks(a["artists"].toArray(), "#FFFFFF");  // artist names link to their pages
        info.extra = {a["release_date"].toString().left(4).toHtmlEscaped()};
        info.total = a["total_tracks"].toInt(-1);
        info.coverUrl = imageUrl(a["images"].toArray(), 464);
        info.path = "/albums/" + a["id"].toString() + "/tracks?limit=50";
        info.cacheId = "album:" + a["id"].toString();
        info.fixed = true;
        info.context = a["uri"].toString();
        info.albumView = true;
        openList(info);
    }

    void runSearch(const QString &q) {
        if (!q.isEmpty()) go([this, q] { showSearch(q); });
    }

    void showSearch(const QString &q) {
        selectNav(0, {});
        ListInfo info;
        info.kind = "Search";
        info.title = "\u201C" + q + "\u201D";
        openList(info);
        int gen = loadGen;
        // Search is capped at 10 per page since 2026; five pages in parallel, appended in arrival order.
        for (int offset = 0; offset < 50; offset += 10)
            api->get(QString("/search?type=track&limit=10&offset=%1&q=").arg(offset) + QUrl::toPercentEncoding(q),
                     [this, gen](const QJsonObject &o) {
                         if (gen != loadGen) return;
                         addTracks(o["tracks"].toObject()["items"].toArray());
                         refreshMeta();
                     });
    }

    void openList(const ListInfo &info) {
        pages->setCurrentWidget(listPage);
        listPage->verticalScrollBar()->setValue(0);
        ++loadGen;
        tracks->clear();
        uris.clear();
        listDurationMs = 0;
        listBlocked = false;
        listSort = "custom";
        listSortButton->setText("Custom order");
        listFilter->clear();
        listFilter->hide();
        listNote->hide();
        context = info.context;
        listOwner = info.owner;
        albumName = info.albumView ? info.title : QString();
        titleDelegate->thumbs = !info.albumView;
        tracks->setColumnHidden(AlbumCol, info.albumView);
        tracks->setColumnHidden(Added, !info.dated);
        headKind->setText(info.kind);
        headTitle->setText(info.title);
        fitTitle();
        QTimer::singleShot(0, this, &Window::fitTitle);  // again once a first-shown page has its width
        listLead = info.lead;
        listExtra = info.extra;
        listTotal = info.total;
        refreshMeta();
        headCover->setVisible(!info.coverUrl.isEmpty() || !info.fallback.isNull());
        headCover->setPixmap(info.fallback.isNull() ? tile(232, 6, dpr(), theme::highlight) : info.fallback);
        setHeadTint(info.tint.isValid() ? info.tint : QColor("#535353"));
        headCoverUrl = info.coverUrl;
        covers.load(info.coverUrl, this, [this, url = info.coverUrl](const QPixmap &pm) {
            if (url != headCoverUrl) return;
            headCover->setPixmap(rounded(pm, 232, 6, dpr()));
            QColor mood = moodColor(pm);  // the band takes the cover's colour, like Spotify's
            setHeadTint(mood.isValid() ? QColor::fromHslF(mood.hslHueF(), std::min(mood.hslSaturationF(), 0.6f), 0.36f)
                                       : QColor("#535353"));
        });
        fitTracks();
        if (info.path.isEmpty()) return;
        int gen = loadGen;
        // A list seen before shows at once from the cache, then is checked against Spotify in the
        // background and redrawn only if it changed.
        QJsonArray cached = info.cacheId.isEmpty() ? QJsonArray() : cachedList(info.cacheId);
        bool shown = !cached.isEmpty();
        if (shown) {
            addTracks(cached);
            refreshMeta();
            if (info.fixed) return;
        }
        auto fresh = std::make_shared<QJsonArray>();
        api->getAll(
            info.path,
            [this, gen, shown, cached, fresh, id = info.cacheId](const QJsonObject &page) {
                if (gen != loadGen) return false;  // user moved on; stop paging (and cache nothing)
                listTotal = page["total"].toInt(listTotal);  // fresher than the library's count
                QJsonArray items;
                for (const auto &v : page["items"].toArray()) items << compactEntry(v.toObject());
                for (const auto &v : items) fresh->append(v);
                if (!shown) addTracks(items);
                if (page["next"].toString().isEmpty()) {  // the last page
                    if (!id.isEmpty()) storeList(id, *fresh);
                    if (shown && *fresh != cached) {
                        tracks->clear();
                        uris.clear();
                        listDurationMs = 0;
                        addTracks(*fresh);
                    }
                }
                refreshMeta();
                return true;
            },
            [this, gen](int status, const QString &message) {
                if (gen == loadGen) showListBlocked(status, message);
            });
    }

    // Spotify's answer is shown as given. For playlists you follow it is a 403: its public API only
    // lists playlists you own or collaborate on (developer docs, "Get Playlist Items").
    void showListBlocked(int status, const QString &message) {
        listBlocked = true;
        refreshMeta();
        if (status == 403) {
            noteTitle->setText("Spotify doesn't share this playlist's songs with other apps");
            noteBody->setText(QString("It belongs to %1, and Spotify only lets apps list playlists you own or collaborate "
                                      "on. Press play, and the songs show up here as Spotify queues them.")
                                  .arg(listOwner.isEmpty() ? QString("someone else") : listOwner));
        } else {
            noteTitle->setText("Couldn't load the songs");
            noteBody->setText(QString("Spotify answered %1: %2").arg(status).arg(message));
        }
        listNote->show();
        fitTracks();
    }

    // The queue does list a followed playlist's upcoming songs once it is playing.
    void revealFromQueue(int attempt = 0) {
        if (!listBlocked) return;
        int gen = loadGen;
        QString ctx = context;
        QTimer::singleShot(1200, this, [this, gen, ctx, attempt] {
            if (gen != loadGen) return;
            if (nowContextUri != ctx) {  // playback hasn't switched to this playlist yet
                if (attempt < 4) revealFromQueue(attempt + 1);
                return;
            }
            api->get("/me/player/queue", [this, gen](const QJsonObject &o) {
                if (gen != loadGen) return;
                QJsonArray fresh;
                auto add = [&](const QJsonObject &t) { if (!t.isEmpty() && !uris.contains(t["uri"].toString())) fresh << t; };
                add(o["currently_playing"].toObject());
                for (const auto &v : o["queue"].toArray()) add(v.toObject());
                addTracks(fresh);
                noteBody->setText("These are the songs Spotify has queued from this playlist so far. More appear as it plays.");
            });
        });
    }

    // Track lists are kept in memory and on disk, so a list opened before shows at once, even
    // after a restart. ponytail: one small file per list ever opened, never pruned; prune by age
    // if the folder ever matters.
    static QString listFile(const QString &id) {
        return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/lists/" +
               QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex() + ".json";
    }

    QJsonArray cachedList(const QString &id) {
        if (auto it = listCache.constFind(id); it != listCache.cend()) return *it;
        QFile f(listFile(id));
        if (!f.open(QIODevice::ReadOnly)) return {};
        auto doc = QJsonDocument::fromJson(f.readAll()).object();
        if (doc["id"].toString() != id) return {};  // unreadable, or not the file we wrote
        return listCache[id] = doc["items"].toArray();
    }

    void storeList(const QString &id, const QJsonArray &items) {
        listCache[id] = items;
        QString path = listFile(id), dir = QFileInfo(path).path();
        QDir().mkpath(dir);
        // What someone listens to is private: owner-only, like librespot's folder.
        QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QSaveFile f(path);  // written whole or not at all
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(QJsonObject{{"id", id}, {"items", items}}).toJson(QJsonDocument::Compact));
            f.commit();
        }
    }

    void addTracks(const QJsonArray &items) {
        QList<QTreeWidgetItem *> rows;
        for (const auto &v : items) {
            auto entry = v.toObject();
            auto t = unwrapTrack(entry);
            auto uri = t["uri"].toString();
            if (uri.isEmpty() || t["is_local"].toBool()) continue;  // removed or local files can't stream
            auto album = t["album"].toObject();
            qint64 duration = t["duration_ms"].toInteger();
            QString added = entry["added_at"].toString();
            auto *row = new QTreeWidgetItem({QString::number(uris.size() + 1), t["name"].toString(),
                                             album["name"].toString(albumName), formatAdded(added), formatTime(duration)});
            row->setData(Num, Role::Uri, uri);
            row->setData(Num, Role::Order, int(uris.size()));  // "Custom order"
            row->setData(Title, Role::Subtitle, joinNames(t["artists"].toArray()));
            row->setData(Title, Role::Json, t["artists"].toArray());
            if (titleDelegate->thumbs) row->setData(Title, Role::Image, imageUrl(album["images"].toArray(), 64 * dpr()));
            row->setData(Added, Role::Id, added);
            row->setData(Time, Role::Order, duration);
            for (int c : {int(Num), int(AlbumCol), int(Added), int(Time)}) row->setForeground(c, theme::subdued);
            row->setTextAlignment(Num, Qt::AlignCenter);
            row->setTextAlignment(Time, Qt::AlignRight | Qt::AlignVCenter);
            if (uri == nowUri) row->setForeground(Title, theme::green);
            listDurationMs += duration;
            rows << row;
            uris << uri;
        }
        tracks->addTopLevelItems(rows);  // one batch per page keeps scrolling smooth while loading
        if (listSort != "custom" || !listFilter->text().isEmpty()) applyListView();
        fitTracks();
        QTimer::singleShot(0, this, &Window::syncTrackThumbs);
    }

    // The table shows every row (the page scrolls), so it is exactly as tall as its visible rows.
    void fitTracks() {
        int rows = 0;
        for (int i = 0; i < tracks->topLevelItemCount(); ++i) rows += !tracks->topLevelItem(i)->isHidden();
        tracks->setVisible(rows > 0 || !listBlocked);
        tracks->setFixedHeight(tracks->header()->sizeHint().height() + rows * 56 + 4);
    }

    void applyListView() {
        QList<QTreeWidgetItem *> rows;
        while (tracks->topLevelItemCount()) rows << tracks->takeTopLevelItem(0);
        auto text = [](QTreeWidgetItem *r, int c, int role) { return r->data(c, role).toString(); };
        std::stable_sort(rows.begin(), rows.end(), [&](QTreeWidgetItem *a, QTreeWidgetItem *b) {
            if (listSort == "title") return a->text(Title).localeAwareCompare(b->text(Title)) < 0;
            if (listSort == "artist") return text(a, Title, Role::Subtitle).localeAwareCompare(text(b, Title, Role::Subtitle)) < 0;
            if (listSort == "album") return a->text(AlbumCol).localeAwareCompare(b->text(AlbumCol)) < 0;
            if (listSort == "added") return text(a, Added, Role::Id) > text(b, Added, Role::Id);  // newest first
            if (listSort == "duration") return a->data(Time, Role::Order).toLongLong() < b->data(Time, Role::Order).toLongLong();
            return a->data(Num, Role::Order).toInt() < b->data(Num, Role::Order).toInt();
        });
        QString filter = listFilter->text().trimmed();
        uris.clear();
        for (auto *r : rows) {
            tracks->addTopLevelItem(r);
            uris << r->data(Num, Role::Uri).toString();
            r->setText(Num, QString::number(uris.size()));
            bool match = filter.isEmpty() || r->text(Title).contains(filter, Qt::CaseInsensitive) ||
                         text(r, Title, Role::Subtitle).contains(filter, Qt::CaseInsensitive) ||
                         r->text(AlbumCol).contains(filter, Qt::CaseInsensitive);
            r->setHidden(!match);
        }
        fitTracks();
        QTimer::singleShot(0, this, &Window::syncTrackThumbs);
    }

    // Album thumbnails only for rows on screen; rows of the same album share one image.
    void syncTrackThumbs() {
        if (!titleDelegate->thumbs || pages->currentWidget() != listPage || !tracks->isVisible()) return;
        // The part of the table inside the page's viewport (the table itself never scrolls).
        auto *view = listPage->viewport();
        QRect visible = QRect(tracks->viewport()->mapFrom(view, QPoint(0, 0)), view->size()).intersected(tracks->viewport()->rect());
        if (visible.isEmpty()) return;
        for (auto *it = tracks->itemAt(QPoint(4, visible.top() + 1)); it && tracks->visualItemRect(it).top() <= visible.bottom();
             it = tracks->itemBelow(it)) {
            QString url = it->data(Title, Role::Image).toString();
            if (url.isEmpty() || it->data(Title, Role::Loaded).toBool()) continue;
            it->setData(Title, Role::Loaded, true);
            covers.load(url, tracks, [this, url](const QPixmap &pm) {
                QPixmap thumb = rounded(pm, 40, 4, dpr());
                for (int i = 0; i < tracks->topLevelItemCount(); ++i)
                    if (auto *row = tracks->topLevelItem(i); row->data(Title, Role::Image).toString() == url) {
                        row->setData(Title, Qt::DecorationRole, thumb);
                        row->setData(Title, Role::Loaded, true);
                    }
            });
        }
    }

    // ---- playback -----------------------------------------------------------

    void playFrom(int row) {
        if (uris.isEmpty()) {
            if (context.isEmpty()) return;
            play({{"context_uri", context}});
            revealFromQueue();  // a followed playlist: list its songs from the queue as they come
            return;
        }
        // A re-sorted view plays in the order shown; the context can only play its own order.
        play(listSort == "custom" ? playBody(context, uris, row) : playBody({}, uris, row));
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
        mpris->seeked(ms * 1000);
    }

    qint64 positionMs() const {
        return std::min<qint64>(progressAt + (playing ? sinceProgress.elapsed() : 0), seek->maximum());
    }

    Api::Callback pollSoon() {
        return [this](const QJsonObject &) { QTimer::singleShot(400, this, &Window::poll); };
    }

    void setPlaying(bool on) {
        if (playing == on && !playPause->icon().isNull()) return;
        playing = on;
        playPause->setIcon(glyphIcon(on ? Glyph::Pause : Glyph::Play, theme::base, 20));
        pushNowPlaying();
    }

    void poll() {
        if (api->signingIn()) return;  // don't stack polls behind a browser sign-in
        api->get("/me/player", [this](const QJsonObject &p) {
            setPlaying(p["is_playing"].toBool());
            auto dev = p["device"].toObject();
            activeDeviceId = dev["id"].toString();
            // Only a named, different device earns the strip (an empty name showed "Playing on" alone).
            bool remote = !dev["name"].toString().isEmpty() && activeDeviceId != deviceId;
            connectText->setText("Playing on " + dev["name"].toString());
            connectBar->setVisible(remote);
            devicesButton->setIcon(glyphIcon(Glyph::Devices, remote ? theme::green : theme::white, 20));
            volume->setEnabled(dev["supports_volume"].toBool(true));  // some phones refuse remote volume
            bool shuffle = p["shuffle_state"].toBool();
            QString repeat = p["repeat_state"].toString("off");
            if (shuffle != shuffleOn || repeat != repeatState) {
                shuffleOn = shuffle;
                repeatState = repeat;
                updateModeButtons();
            }
            QString ctx = p["context"].toObject()["uri"].toString();
            if (ctx != nowContextUri) resolveContext(ctx);
            auto item = p["item"].toObject();
            if (item.isEmpty()) return showIdle();
            if (item["uri"].toString() != nowUri) showTrack(item);
            seek->setMaximum(item["duration_ms"].toInt());
            total->setText(formatTime(seek->maximum()));
            progressAt = p["progress_ms"].toInt();
            sinceProgress.restart();
            advanceProgress();
            if (!volume->isSliderDown()) volume->setValue(dev["volume_percent"].toInt());
            pushMpris();
        });
    }

    void advanceProgress() {
        if (seek->isSliderDown()) return elapsed->setText(formatTime(seek->value()));
        qint64 pos = positionMs();
        seek->setValue(pos);
        elapsed->setText(formatTime(pos));
        if (pos / 1000 != pushedSecond) pushNowPlaying();  // mini player / full screen: once a second
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
        if (sideStack->currentIndex() == 1) loadQueue();
        if (listBlocked && pages->currentWidget() == listPage && nowContextUri == context) revealFromQueue();
        npTitle->setText(item["name"].toString());
        barTrack->setText(QString("<b>%1</b><br>").arg(item["name"].toString().toHtmlEscaped()) +
                          artistLinks(item["artists"].toArray(), "#B3B3B3"));
        npArtist->setText(artistLinks(item["artists"].toArray(), "#B3B3B3"));
        npAlbum->setText(nowAlbum.isEmpty() ? QString()
                                            : "<a href=\"#\" style=\"color:#B3B3B3;text-decoration:none\">" +
                                                  nowAlbum["name"].toString().toHtmlEscaped() + "</a>");
        auto url = imageUrl(nowAlbum["images"].toArray(), 268 * dpr());
        nowCoverUrl = url;
        covers.load(url, this, [this, url](const QPixmap &pm) {
            if (url != nowCoverUrl) return;  // skipped ahead meanwhile
            npCover->setPixmap(rounded(pm, 268, 10, dpr()));
            barCover->setPixmap(rounded(pm, 56, 4, dpr()));
            nowCover = pm;
            nowMood = moodColor(pm);
            tintPanel(nowMood);
            pushNowPlaying();
        });
        if (full && full->isVisible()) loadBigCover();
        pushMpris();
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
            nowCover = {};
            nowMood = {};
            pushNowPlaying();
            pushMpris();
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
        auto *create = new QPushButton("New playlist");
        create->setObjectName("ghost");
        connect(create, &QPushButton::clicked, menu, [this, menu, songs] {
            menu->close();
            createPlaylist(songs);
        });
        if (list->count() == 0) {
            list->hide();
            v->addWidget(label("You don't have a playlist you can add to yet.", "muted"));
        } else {
            v->addWidget(list);
        }
        auto *buttons = new QHBoxLayout;
        buttons->addWidget(create);
        buttons->addStretch();
        buttons->addWidget(done);
        v->addLayout(buttons);
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

    // Right-click menu for one or several songs (track lists, artist page).
    void trackMenu(QStringList songs, const QPoint &at) {
        songs.removeAll(QString());
        if (songs.isEmpty()) return;
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
            auto *queue = menu->addAction(glyphIcon(Glyph::Queue, theme::white, 18), "Add to queue");
            connect(queue, &QAction::triggered, this, [this, songs] {
                queueSongs(songs, 0);
                statusBar()->showMessage(songs.size() == 1 ? QString("Added to queue")
                                                           : QString("Added %1 songs to queue").arg(songs.size()), 3000);
            });
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
        // Pages are built one after another; a resize can arrive before the last grid exists.
        if (e->type() == QEvent::Resize && o == headTitle) fitTitle();
        // Also when the table grows: rows that arrive are only on screen once the table has their height.
        if (e->type() == QEvent::Resize && listPage && (o == listPage->viewport() || o == tracks->viewport()))
            QTimer::singleShot(0, this, &Window::syncTrackThumbs);
        if (e->type() == QEvent::Resize && artistAlbums && (o == homePage->viewport() || o == artistPage->viewport())) {
            QTimer::singleShot(0, this, [this] {
                for (auto *g : {homeArtists, homeSongs, artistAlbums}) fitGrid(g);
            });
        }
        if (e->type() == QEvent::MouseButtonRelease && o->property("ms").isValid()) {
            seekTo(o->property("ms").toLongLong());
            return true;
        }
        return QMainWindow::eventFilter(o, e);
    }

    // ---- history (back / forward) -------------------------------------------------
    // Every navigation is recorded as "how to show this page"; back/forward replay those.

    void go(std::function<void()> view) {
        if (histPos + 1 < history.size()) history.erase(history.begin() + histPos + 1, history.end());
        history << std::move(view);
        if (history.size() > 50) history.removeFirst();  // ponytail: 50 steps back is plenty
        histPos = int(history.size()) - 1;
        history[histPos]();
        updateHistoryButtons();
    }

    void stepHistory(int step) {
        int to = histPos + step;
        if (to < 0 || to >= history.size()) return;
        histPos = to;
        history[histPos]();
        updateHistoryButtons();
    }

    void updateHistoryButtons() {
        backButton->setEnabled(histPos > 0);
        forwardButton->setEnabled(histPos + 1 < history.size());
        backButton->setIcon(glyphIcon(Glyph::Back, histPos > 0 ? theme::white : QColor("#535353"), 18));
        forwardButton->setIcon(glyphIcon(Glyph::Forward, histPos + 1 < history.size() ? theme::white : QColor("#535353"), 18));
    }

    // Highlight the sidebar row for the page being shown (kind 0 = none) without re-navigating.
    void selectNav(int kind, const QString &playlistId) {
        QSignalBlocker quiet(nav);
        nav->clearSelection();
        nav->setCurrentItem(nullptr);
        for (int i = 0; kind && i < nav->count(); ++i) {
            auto *it = nav->item(i);
            if (it->data(Role::Kind).toInt() == kind &&
                (kind != Playlist || it->data(Role::Json).toJsonObject()["id"].toString() == playlistId)) {
                nav->setCurrentItem(it);
                break;
            }
        }
    }

    void goNav(Kind kind) {
        for (int i = 0; i < nav->count(); ++i)
            if (nav->item(i)->data(Role::Kind).toInt() == kind) return go(navView(nav->item(i)));
    }

    // ---- library sort / filter ----------------------------------------------------

    // Reorders the playlist rows under the "Playlists" header and hides ones the filter rules out.
    void applyLibraryView() {
        QSignalBlocker quiet(nav);  // moving the current row must not count as a click
        QString currentId = nav->currentItem() ? nav->currentItem()->data(Role::Json).toJsonObject()["id"].toString() : QString();
        QList<QListWidgetItem *> rows;
        for (int i = nav->count() - 1; i >= 0; --i)
            if (nav->item(i)->data(Role::Kind).toInt() == Playlist) rows.prepend(nav->takeItem(i));
        auto owner = [](QListWidgetItem *it) { return it->data(Role::Json).toJsonObject()["owner"].toObject()["display_name"].toString(); };
        std::stable_sort(rows.begin(), rows.end(), [&](QListWidgetItem *a, QListWidgetItem *b) {
            if (librarySort == "alpha") return a->text().localeAwareCompare(b->text()) < 0;
            if (librarySort == "creator") {
                int c = owner(a).localeAwareCompare(owner(b));
                return c ? c < 0 : a->text().localeAwareCompare(b->text()) < 0;
            }
            return a->data(Role::Order).toInt() < b->data(Role::Order).toInt();
        });
        QString filter = libFilter->text().trimmed();
        for (auto *it : rows) {
            nav->addItem(it);
            it->setHidden(!filter.isEmpty() && !it->text().contains(filter, Qt::CaseInsensitive));
            if (!currentId.isEmpty() && it->data(Role::Json).toJsonObject()["id"].toString() == currentId) nav->setCurrentItem(it);
        }
    }

    void applyAlbumView() {
        QList<QListWidgetItem *> items;
        while (grid->count()) items << grid->takeItem(0);
        std::stable_sort(items.begin(), items.end(), [this](QListWidgetItem *a, QListWidgetItem *b) {
            if (albumSort == "alpha") return a->text().localeAwareCompare(b->text()) < 0;
            if (albumSort == "artist") {
                int c = a->data(Role::Subtitle).toString().localeAwareCompare(b->data(Role::Subtitle).toString());
                return c ? c < 0 : a->text().localeAwareCompare(b->text()) < 0;
            }
            return a->data(Role::Order).toInt() < b->data(Role::Order).toInt();
        });
        QString filter = albumFilter->text().trimmed();
        for (auto *it : items) {
            grid->addItem(it);
            it->setHidden(!filter.isEmpty() && !it->text().contains(filter, Qt::CaseInsensitive) &&
                          !it->data(Role::Subtitle).toString().contains(filter, Qt::CaseInsensitive));
        }
        QTimer::singleShot(0, this, &Window::syncGridCovers);
    }

    void createPlaylist(const QStringList &songs) {
        bool ok;
        QString name = QInputDialog::getText(this, "Create playlist", "Name", QLineEdit::Normal,
                                             QString("My Playlist #%1").arg(playlistOrder + 1), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        api->send("POST", "/me/playlists", {{"name", name}, {"public", false}}, [this, songs, name](const QJsonObject &created) {
            QJsonObject pl = created;
            if (!pl.contains("uri")) pl["uri"] = "spotify:playlist:" + pl["id"].toString();
            auto *it = new QListWidgetItem(QIcon(tile(40, 4, dpr(), theme::highlight)), pl["name"].toString(name));
            it->setData(Role::Kind, Playlist);
            it->setData(Role::Uri, pl["uri"].toString());
            it->setData(Role::Json, pl);
            it->setData(Role::Order, -(++createdCount));  // newest first under Recents, like Spotify
            nav->addItem(it);
            applyLibraryView();
            if (!songs.isEmpty()) {
                for (int k = 0; k < songs.size(); k += 100)
                    api->send("POST", "/playlists/" + pl["id"].toString() + "/items",
                              {{"uris", QJsonArray::fromStringList(songs.mid(k, 100))}});
                statusBar()->showMessage("Added to " + it->text(), 4000);
            } else {
                go(navView(it));
            }
        });
    }

    // ---- home ---------------------------------------------------------------------

    QListWidget *cardGrid(bool round) {
        auto *g = new QListWidget;
        g->setViewMode(QListView::IconMode);
        g->setResizeMode(QListView::Adjust);
        g->setMovement(QListView::Static);
        g->setUniformItemSizes(true);
        g->setGridSize(CardDelegate::cell());
        g->setItemDelegate(new CardDelegate(g));
        g->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // the page scrolls, not the grid
        g->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        g->setMouseTracking(true);
        g->viewport()->setAttribute(Qt::WA_Hover);
        g->setProperty("round", round);
        return g;
    }

    // Grids inside a scrolling page show every row: size them to their content.
    void fitGrid(QListWidget *g) {
        int visible = 0;
        for (int i = 0; i < g->count(); ++i) visible += !g->item(i)->isHidden();
        int perRow = std::max(1, g->viewport()->width() / CardDelegate::cell().width());
        int rows = (visible + perRow - 1) / perRow;
        g->setFixedHeight(std::max(1, rows) * CardDelegate::cell().height() + 6);
        g->setVisible(visible > 0);
    }

    QScrollArea *scrollPage(const char *name, QVBoxLayout *&layout) {
        auto *page = new QScrollArea;
        page->setObjectName(name);
        page->setWidgetResizable(true);
        page->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *content = new QWidget;
        layout = new QVBoxLayout(content);
        layout->setContentsMargins(28, 26, 16, 28);
        layout->setSpacing(14);
        page->setWidget(content);
        page->viewport()->installEventFilter(this);  // re-fit the card grids on resize
        return page;
    }

    QWidget *buildHomePage() {
        QVBoxLayout *layout;
        homePage = scrollPage("homeScroll", layout);
        shortcutGrid = new QGridLayout;
        shortcutGrid->setSpacing(8);
        layout->addLayout(shortcutGrid);
        homeEmpty = label("Play a few songs and your recent albums, playlists and top artists show up here.", "muted");
        homeEmpty->setWordWrap(true);
        homeEmpty->hide();
        layout->addWidget(homeEmpty);
        homeArtistsTitle = label("Your top artists", "section");
        homeArtists = cardGrid(true);
        homeSongsTitle = label("Your top songs this month", "section");
        homeSongs = cardGrid(false);
        layout->addSpacing(12);
        layout->addWidget(homeArtistsTitle);
        layout->addWidget(homeArtists);
        layout->addWidget(homeSongsTitle);
        layout->addWidget(homeSongs);
        layout->addStretch();
        connect(homeArtists, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { openArtist(it->data(Role::Id).toString()); });
        connect(homeSongs, &QListWidget::itemClicked, this,
                [this](QListWidgetItem *it) { play(playBody({}, homeSongUris, homeSongs->row(it))); });
        return homePage;
    }

    void loadHome() {
        homeLoadedAt.start();
        int gen = ++homeGen;
        while (auto *item = shortcutGrid->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        homeArtists->clear();
        homeSongs->clear();
        homeSongUris.clear();
        homeEmpty->hide();
        // Spotify's shortcut grid: the albums and playlists you played most recently.
        api->get("/me/player/recently-played?limit=50", [this, gen](const QJsonObject &o) {
            if (gen != homeGen) return;
            QStringList seen;
            for (const auto &v : o["items"].toArray()) {
                QString uri = v.toObject()["context"].toObject()["uri"].toString();
                if (uri.isEmpty() || seen.contains(uri)) continue;
                seen << uri;
                addShortcut(uri, int(seen.size()) - 1);
                if (seen.size() == 8) break;
            }
            homeEmpty->setVisible(seen.isEmpty() && homeArtists->count() == 0);
        });
        api->get("/me/top/artists?limit=10&time_range=short_term", [this, gen](const QJsonObject &o) {
            if (gen != homeGen) return;
            for (const auto &v : o["items"].toArray()) {
                auto a = v.toObject();
                auto *it = new QListWidgetItem(a["name"].toString(), homeArtists);
                it->setData(Role::Subtitle, "Artist");
                it->setData(Role::Kind, CardDelegate::Round);
                it->setData(Role::Id, a["id"].toString());
                setIconAsync(homeArtists, it, imageUrl(a["images"].toArray(), CardDelegate::Cover * dpr()),
                             CardDelegate::Cover, CardDelegate::Cover / 2.0);
            }
            homeArtistsTitle->setVisible(homeArtists->count() > 0);
            fitGrid(homeArtists);
        });
        api->get("/me/top/tracks?limit=10&time_range=short_term", [this, gen](const QJsonObject &o) {
            if (gen != homeGen) return;
            for (const auto &v : o["items"].toArray()) {
                auto t = v.toObject();
                auto *it = new QListWidgetItem(t["name"].toString(), homeSongs);
                it->setData(Role::Subtitle, joinNames(t["artists"].toArray()));
                homeSongUris << t["uri"].toString();
                setIconAsync(homeSongs, it, imageUrl(t["album"].toObject()["images"].toArray(), CardDelegate::Cover * dpr()),
                             CardDelegate::Cover, 6);
            }
            homeSongsTitle->setVisible(homeSongs->count() > 0);
            fitGrid(homeSongs);
        });
    }

    void addShortcut(const QString &uri, int index) {
        auto *tileFrame = new Clickable;
        tileFrame->setObjectName("shortcut");
        tileFrame->setFixedHeight(56);
        tileFrame->setCursor(Qt::PointingHandCursor);
        auto *row = new QHBoxLayout(tileFrame);
        row->setContentsMargins(0, 0, 10, 0);
        row->setSpacing(12);
        auto *cover = new QLabel;
        cover->setFixedSize(56, 56);
        cover->setPixmap(tile(56, 4, dpr(), theme::highlight));
        auto *name = label({}, "shortcutName");
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row->addWidget(cover);
        row->addWidget(name, 1);
        shortcutGrid->addWidget(tileFrame, index / 4, index % 4);
        QPointer<Clickable> guard(tileFrame);
        auto fill = [this, guard, cover, name](const QString &title, const QJsonArray &images, std::function<void()> open) {
            if (!guard) return;
            name->setText(title);
            guard->onClick = std::move(open);
            covers.load(imageUrl(images, 112), guard, [this, cover](const QPixmap &pm) { cover->setPixmap(rounded(pm, 56, 4, dpr())); });
        };
        QString id = uri.section(':', 2);
        if (uri.startsWith("spotify:album:"))
            api->get("/albums/" + id, [this, fill](const QJsonObject &a) { fill(a["name"].toString(), a["images"].toArray(), [this, a] { openAlbum(a); }); });
        else if (uri.startsWith("spotify:playlist:"))
            api->get("/playlists/" + id, [this, fill](const QJsonObject &pl) {
                fill(pl["name"].toString(), pl["images"].toArray(), [this, pl] { go([this, pl] { showNav(Playlist, pl); }); });
            });
        else if (uri.startsWith("spotify:artist:"))
            api->get("/artists/" + id, [this, fill, id](const QJsonObject &a) { fill(a["name"].toString(), a["images"].toArray(), [this, id] { openArtist(id); }); });
        else if (uri.endsWith(":collection")) {
            Glyph heart = Glyph::Heart;
            cover->setPixmap(tile(56, 4, dpr(), likedFrom, theme::white, &heart, likedTo));
            name->setText("Liked Songs");
            tileFrame->onClick = [this] { goNav(LikedSongs); };
        }
    }

    // ---- artist page --------------------------------------------------------------

    QWidget *buildArtistPage() {
        QVBoxLayout *layout;
        artistPage = scrollPage("artistScroll", layout);
        artistAvatar = new QLabel;
        artistAvatar->setFixedSize(184, 184);
        artistTitle = label({}, "h1");
        artistTitle->setWordWrap(true);
        artistMeta = label({}, "muted");
        artistPlay = new QToolButton;
        artistPlay->setObjectName("bigPlay");
        artistPlay->setIcon(glyphIcon(Glyph::Play, theme::base, 26));
        artistPlay->setIconSize({26, 26});
        artistPlay->setFixedSize(56, 56);
        artistPlay->setToolTip("Play");
        auto *text = new QVBoxLayout;
        text->setSpacing(6);
        text->addStretch();
        text->addWidget(label("Artist", "kind"));
        text->addWidget(artistTitle);
        text->addWidget(artistMeta);
        text->addSpacing(10);
        text->addWidget(artistPlay, 0, Qt::AlignLeft);
        auto *head = new QHBoxLayout;
        head->setSpacing(24);
        head->addWidget(artistAvatar, 0, Qt::AlignLeft | Qt::AlignBottom);
        head->addLayout(text, 1);
        layout->addLayout(head);
        layout->addSpacing(10);

        // Spotify shows "Popular" here; its top-tracks endpoint is gone (2026), so these are
        // the artist's songs from search, honestly labelled.
        layout->addWidget(label("Songs", "section"));
        artistSongs = new QTreeWidget;
        artistSongs->setHeaderLabels({"#", "Title", "Album", "Time"});
        artistSongs->setRootIsDecorated(false);
        artistSongs->setUniformRowHeights(true);
        artistSongs->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        artistSongs->setSelectionMode(QAbstractItemView::ExtendedSelection);
        artistSongs->setContextMenuPolicy(Qt::CustomContextMenu);
        auto *h = artistSongs->header();
        h->setStretchLastSection(false);
        h->setSectionResizeMode(QHeaderView::Stretch);
        h->setSectionResizeMode(0, QHeaderView::Fixed);
        h->setSectionResizeMode(3, QHeaderView::Fixed);
        h->resizeSection(0, 48);
        h->resizeSection(3, 64);
        layout->addWidget(artistSongs);
        artistMore = new QPushButton("Show more");
        artistMore->setObjectName("ghost");
        layout->addWidget(artistMore, 0, Qt::AlignLeft);

        layout->addSpacing(12);
        layout->addWidget(label("Discography", "section"));
        auto *chips = new QHBoxLayout;
        chipAlbums = new QPushButton("Albums");
        chipSingles = new QPushButton("Singles and EPs");
        auto *group = new QButtonGroup(this);
        for (auto *c : {chipAlbums, chipSingles}) {
            c->setObjectName("chip");
            c->setCheckable(true);
            group->addButton(c);
            chips->addWidget(c);
        }
        chipAlbums->setChecked(true);
        chips->addStretch();
        layout->addLayout(chips);
        artistAlbums = cardGrid(false);
        layout->addWidget(artistAlbums);
        artistNoAlbums = label({}, "muted");
        layout->addWidget(artistNoAlbums);
        layout->addStretch();

        connect(artistPlay, &QToolButton::clicked, this, [this] { play({{"context_uri", "spotify:artist:" + currentArtistId}}); });
        connect(artistSongs, &QTreeWidget::itemActivated, this,
                [this](QTreeWidgetItem *it) { play(playBody({}, artistSongUris, artistSongs->indexOfTopLevelItem(it))); });
        connect(artistSongs, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
            QStringList songs;
            for (auto *it : artistSongs->selectedItems()) songs << artistSongUris.value(artistSongs->indexOfTopLevelItem(it));
            trackMenu(songs, artistSongs->viewport()->mapToGlobal(pos));
        });
        connect(artistMore, &QPushButton::clicked, this, [this] {
            artistSongsExpanded = !artistSongsExpanded;
            showArtistSongs();
        });
        connect(group, &QButtonGroup::buttonClicked, this, [this] { filterDiscography(); });
        connect(artistAlbums, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { openAlbum(it->data(Role::Json).toJsonObject()); });
        return artistPage;
    }

    void openLink(const QString &link) {
        if (link.startsWith("artist:")) openArtist(link.mid(7));
        else if (link == "#") openAlbum(nowAlbum);
    }

    void openArtist(const QString &id) {
        if (!id.isEmpty()) go([this, id] { showArtist(id); });
    }

    // One artist opens directly; several get a small menu, since a table cell is one click target.
    void openArtistOf(const QJsonArray &artists, const QPoint &at) {
        if (artists.size() == 1) return openArtist(artists.first().toObject()["id"].toString());
        if (artists.isEmpty()) return;
        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        for (const auto &v : artists) {
            auto a = v.toObject();
            connect(menu->addAction(a["name"].toString()), &QAction::triggered, this,
                    [this, id = a["id"].toString()] { openArtist(id); });
        }
        menu->popup(at);
    }

    void showArtist(const QString &id) {
        selectNav(0, {});
        pages->setCurrentWidget(artistPage);
        artistPage->verticalScrollBar()->setValue(0);
        currentArtistId = id;
        int gen = ++artistGen;
        artistTitle->clear();
        artistMeta->clear();
        artistAvatar->setPixmap(tile(184, 92, dpr(), theme::highlight));
        artistSongs->clear();
        artistSongUris.clear();
        artistSongRows.clear();
        artistSongsExpanded = false;
        artistAlbums->clear();
        artistNoAlbums->clear();
        api->get("/artists/" + id, [this, id, gen](const QJsonObject &a) {
            if (gen != artistGen) return;
            QStringList genres;
            for (const auto &g : a["genres"].toArray()) genres << g.toString();
            artistTitle->setText(a["name"].toString());
            artistMeta->setText(genres.mid(0, 3).join(", "));
            covers.load(imageUrl(a["images"].toArray(), 184 * dpr()), this, [this, gen](const QPixmap &pm) {
                if (gen == artistGen) artistAvatar->setPixmap(rounded(pm, 184, 92, dpr()));
            });
            loadArtistSongs(id, a["name"].toString(), gen);
        });
        // Discography arrives 10 at a time (the 2026 limit for this endpoint).
        api->getAll("/artists/" + id + "/albums?include_groups=album,single&limit=10", [this, gen](const QJsonObject &page) {
            if (gen != artistGen) return false;
            for (const auto &v : page["items"].toArray()) {
                auto a = v.toObject();
                QString type = a["album_type"].toString();
                auto *it = new QListWidgetItem(a["name"].toString(), artistAlbums);
                it->setData(Role::Subtitle, a["release_date"].toString().left(4) + (type == "single" ? ", Single" : ", Album"));
                it->setData(Role::Json, a);
                it->setData(Role::Kind, 0);
                it->setData(Role::Id, type);
                // ponytail: an artist's whole discography loads its covers; page this like the albums grid if one runs to hundreds
                setIconAsync(artistAlbums, it, imageUrl(a["images"].toArray(), CardDelegate::Cover * dpr()), CardDelegate::Cover, 6);
            }
            filterDiscography();
            return true;
        });
    }

    // Five pages of 10 (search's 2026 cap), kept in result order, only songs this artist is on.
    void loadArtistSongs(const QString &id, QString name, int gen) {
        name.remove('"');
        auto pages = std::make_shared<QVector<QJsonArray>>(5);
        auto done = std::make_shared<int>(0);
        for (int i = 0; i < 5; ++i)
            api->get(QString("/search?type=track&limit=10&offset=%1&q=").arg(i * 10) +
                         QUrl::toPercentEncoding("artist:\"" + name + "\""),
                     [this, id, gen, i, pages, done](const QJsonObject &o) {
                         if (gen != artistGen) return;
                         (*pages)[i] = o["tracks"].toObject()["items"].toArray();
                         if (++*done < 5) return;
                         QSet<QString> seenNames;
                         for (const auto &page : *pages)
                             for (const auto &v : page) {
                                 auto t = v.toObject();
                                 bool theirs = false;
                                 for (const auto &a : t["artists"].toArray()) theirs |= a.toObject()["id"].toString() == id;
                                 QString key = t["name"].toString().toLower();
                                 if (!theirs || seenNames.contains(key)) continue;  // features elsewhere, duplicate versions
                                 seenNames << key;
                                 artistSongRows << t;
                                 artistSongUris << t["uri"].toString();
                             }
                         showArtistSongs();
                     });
    }

    void showArtistSongs() {
        artistSongs->clear();
        int shown = artistSongsExpanded ? int(artistSongRows.size()) : std::min<int>(10, int(artistSongRows.size()));
        for (int i = 0; i < shown; ++i) {
            const auto &t = artistSongRows[i];
            auto *row = new QTreeWidgetItem({QString::number(i + 1), t["name"].toString(),
                                             t["album"].toObject()["name"].toString(), formatTime(t["duration_ms"].toInteger())});
            for (int c : {0, 2, 3}) row->setForeground(c, theme::subdued);
            row->setTextAlignment(0, Qt::AlignCenter);
            row->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
            if (t["uri"].toString() == nowUri) row->setForeground(1, theme::green);
            artistSongs->addTopLevelItem(row);
        }
        artistSongs->setFixedHeight(std::max(1, shown) * 40 + artistSongs->header()->sizeHint().height() + 4);
        artistSongs->setVisible(shown > 0);
        artistMore->setVisible(artistSongRows.size() > 10);
        artistMore->setText(artistSongsExpanded ? "Show less" : "Show more");
    }

    void filterDiscography() {
        bool singles = chipSingles->isChecked();
        for (int i = 0; i < artistAlbums->count(); ++i) {
            auto *it = artistAlbums->item(i);
            it->setHidden((it->data(Role::Id).toString() == "single") != singles);
        }
        fitGrid(artistAlbums);
        artistNoAlbums->setText(artistAlbums->isVisible() ? QString() : singles ? "No singles or EPs." : "No albums.");
    }

    // ---- queue panel ------------------------------------------------------------------

    QWidget *buildQueuePanel() {
        auto *w = new QWidget;
        auto *layout = new QVBoxLayout(w);
        layout->setContentsMargins(16, 18, 12, 12);
        layout->setSpacing(8);
        auto *tabs = new QHBoxLayout;
        queueTab = new QPushButton("Queue");
        recentTab = new QPushButton("Recently played");
        auto *group = new QButtonGroup(w);
        for (auto *t : {queueTab, recentTab}) {
            t->setObjectName("tab");
            t->setCheckable(true);
            group->addButton(t);
            tabs->addWidget(t);
        }
        queueTab->setChecked(true);
        tabs->addStretch();
        auto *close = iconButton(Glyph::Close, 16, "Close");
        tabs->addWidget(close);
        layout->addLayout(tabs);
        queueList = new QListWidget;
        queueList->setItemDelegate(new RowDelegate(queueList));
        queueList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        queueList->setMouseTracking(true);
        queueList->viewport()->setAttribute(Qt::WA_Hover);
        queueList->setSelectionMode(QAbstractItemView::NoSelection);
        layout->addWidget(queueList, 1);
        queueEmpty = label({}, "muted");
        queueEmpty->setWordWrap(true);
        layout->addWidget(queueEmpty);
        layout->addStretch();
        connect(close, &QToolButton::clicked, this, [this] { toggleQueue(false); });
        connect(group, &QButtonGroup::buttonClicked, this, [this] { loadQueue(); });
        // Recently played rows play on double-click; queued rows can't be jumped to through the API.
        connect(queueList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
            if (recentTab->isChecked() && !it->data(Role::Uri).toString().isEmpty())
                play({{"uris", QJsonArray{it->data(Role::Uri).toString()}}});
        });
        return w;
    }

    void toggleQueue(bool open) {
        sideStack->setCurrentIndex(open ? 1 : 0);
        queueButton->setIcon(toggleIcon(Glyph::Queue, open, 20));
        if (open) loadQueue();
    }

    void addQueueRow(const QJsonObject &t, int kind) {
        auto *it = new QListWidgetItem(t["name"].toString(), queueList);
        it->setData(Role::Subtitle, joinNames(t["artists"].toArray()));
        it->setData(Role::Kind, kind);
        it->setData(Role::Uri, t["uri"].toString());
        setIconAsync(queueList, it, imageUrl(t["album"].toObject()["images"].toArray(), 80), 40, 4);
    }

    void addQueueHeader(const QString &text) {
        auto *it = new QListWidgetItem(text, queueList);
        it->setData(Role::Kind, RowDelegate::Header);
        it->setFlags(Qt::ItemIsEnabled);
    }

    void loadQueue() {
        if (sideStack->currentIndex() != 1) return;
        int gen = ++queueGen;
        queueList->clear();
        queueEmpty->clear();
        if (recentTab->isChecked()) {
            api->get("/me/player/recently-played?limit=50", [this, gen](const QJsonObject &o) {
                if (gen != queueGen) return;
                for (const auto &v : o["items"].toArray()) addQueueRow(v.toObject()["track"].toObject(), 0);
                queueEmpty->setText(queueList->count() ? QString() : "Songs you play show up here.");
            });
            return;
        }
        api->get("/me/player/queue", [this, gen](const QJsonObject &o) {
            if (gen != queueGen) return;
            auto now = o["currently_playing"].toObject();
            auto next = o["queue"].toArray();
            if (!now.isEmpty()) {
                addQueueHeader("Now playing");
                addQueueRow(now, RowDelegate::Current);
            }
            if (!next.isEmpty()) {
                addQueueHeader(nowContextName.isEmpty() ? QString("Next up") : "Next from: " + nowContextName);
                for (const auto &v : next) addQueueRow(v.toObject(), 0);
            }
            queueEmpty->setText(now.isEmpty() && next.isEmpty() ? "Your queue is empty. Right-click any song and choose Add to queue."
                                                                 : QString());
        });
    }

    // Queue in order: each add waits for the previous, or parallel requests could land shuffled.
    void queueSongs(const QStringList &songs, int i) {
        if (i >= songs.size()) return QTimer::singleShot(400, this, &Window::loadQueue);
        api->send("POST", "/me/player/queue?uri=" + QUrl::toPercentEncoding(songs[i]), {},
                  [this, songs, i](const QJsonObject &) { queueSongs(songs, i + 1); });
    }

    // "Next from: <album or playlist>" needs the playing context's name.
    void resolveContext(const QString &uri) {
        nowContextUri = uri;
        nowContextName.clear();
        QString id = uri.section(':', 2);
        auto setName = [this, uri](const QJsonObject &o) {
            if (uri == nowContextUri) nowContextName = o["name"].toString();
            loadQueue();
        };
        if (uri.startsWith("spotify:album:")) api->get("/albums/" + id, setName);
        else if (uri.startsWith("spotify:playlist:")) api->get("/playlists/" + id, setName);
        else if (uri.startsWith("spotify:artist:")) api->get("/artists/" + id, setName);
        else if (uri.endsWith(":collection")) nowContextName = "Liked Songs";
    }

    // ---- shuffle, repeat, volume ----------------------------------------------------

    void updateModeButtons() {
        shuffleButton->setIcon(toggleIcon(Glyph::Shuffle, shuffleOn, 20));
        headShuffle->setIcon(toggleIcon(Glyph::Shuffle, shuffleOn, 28));
        headShuffle->setToolTip(shuffleOn ? "Disable shuffle" : "Enable shuffle");
        shuffleButton->setToolTip(shuffleOn ? "Disable shuffle (Ctrl+S)" : "Enable shuffle (Ctrl+S)");
        repeatButton->setIcon(toggleIcon(repeatState == "track" ? Glyph::RepeatOne : Glyph::Repeat, repeatState != "off", 20));
        repeatButton->setToolTip(repeatState == "off" ? "Enable repeat (Ctrl+R)"
                                 : repeatState == "context" ? "Enable repeat one (Ctrl+R)" : "Disable repeat (Ctrl+R)");
    }

    void setShuffle(bool on) {
        shuffleOn = on;  // optimistic; the next poll corrects it
        updateModeButtons();
        api->send("PUT", QString("/me/player/shuffle?state=%1").arg(on ? "true" : "false"), {}, pollSoon());
        pushMpris();
    }

    void setRepeat(const QString &state) {
        repeatState = state;
        updateModeButtons();
        api->send("PUT", "/me/player/repeat?state=" + state, {}, pollSoon());
        pushMpris();
    }

    void setVolumePercent(int v) {
        v = std::clamp(v, 0, 100);
        volume->setValue(v);
        api->send("PUT", QString("/me/player/volume?volume_percent=%1").arg(v));
        pushMpris();
    }

    // ---- mini player, full screen, media keys ---------------------------------------

    Transport transport() {
        return {[this] { playPause->click(); }, [this] { prevButton->click(); }, [this] { nextButton->click(); }, {}};
    }

    void toggleMini() {
        if (!mini) {
            mini = new MiniPlayer(this);
            mini->transport = transport();
            mini->transport.closed = [this] { miniButton->setIcon(toggleIcon(Glyph::MiniPlayer, false, 18)); };
            QRect screen = this->screen()->availableGeometry();
            mini->move(screen.right() - mini->width() - 24, screen.bottom() - mini->height() - 24);
        }
        if (mini->isVisible()) return void(mini->close());
        mini->show();
        miniButton->setIcon(toggleIcon(Glyph::MiniPlayer, true, 18));
        pushNowPlaying();
    }

    void toggleFull() {
        if (!full) {
            full = new FullScreenView(this);
            full->transport = transport();
        }
        if (full->isVisible()) return void(full->close());
        full->showFullScreen();
        loadBigCover();
        pushNowPlaying();
    }

    // The panel's cover is ~300 px; full screen wants the 640 px one.
    void loadBigCover() {
        QString url = imageUrl(nowAlbum["images"].toArray(), 640);
        covers.load(url, this, [this, url](const QPixmap &pm) {
            if (url == imageUrl(nowAlbum["images"].toArray(), 640)) {
                bigCover = pm;
                pushNowPlaying();
            }
        });
    }

    void pushNowPlaying() {
        bool miniOn = mini && mini->isVisible(), fullOn = full && full->isVisible();
        if (!miniOn && !fullOn) return;
        NowPlaying s{nowItem["name"].toString(), joinNames(nowItem["artists"].toArray()), nowCover, nowMood,
                     playing, positionMs(), seek->maximum()};
        pushedSecond = s.position / 1000;
        if (miniOn) mini->setState(s);
        if (fullOn) {
            if (!bigCover.isNull() && !nowItem.isEmpty()) s.cover = bigCover;
            full->setState(s);
        }
    }

    void wireMpris() {
        mpris = new Mpris(this);
        mpris->raise = [this] {
            showNormal();
            raise();
            activateWindow();
        };
        mpris->playPause = [this] { playPause->click(); };
        mpris->play = [this] { if (!playing) playPause->click(); };
        mpris->pause = [this] { if (playing) playPause->click(); };
        mpris->next = [this] { nextButton->click(); };
        mpris->previous = [this] { prevButton->click(); };
        mpris->seekBy = [this](qint64 us) { seekTo(std::clamp<qint64>(positionMs() + us / 1000, 0, seek->maximum())); };
        mpris->seekTo = [this](qint64 us) { seekTo(std::clamp<qint64>(us / 1000, 0, seek->maximum())); };
        mpris->setShuffle = [this](bool on) { setShuffle(on); };
        mpris->setLoop = [this](const QString &s) { setRepeat(s == "Track" ? "track" : s == "Playlist" ? "context" : "off"); };
        mpris->setVolume = [this](double v) { setVolumePercent(qRound(v * 100)); };
        mpris->positionUs = [this] { return positionMs() * 1000; };
    }

    void pushMpris() {
        if (!mpris) return;
        Mpris::State s;
        s.hasTrack = !nowUri.isEmpty();
        s.trackId = nowItem["id"].toString();
        s.title = nowItem["name"].toString();
        s.album = nowAlbum["name"].toString();
        s.artUrl = imageUrl(nowAlbum["images"].toArray(), 300);
        for (const auto &a : nowItem["artists"].toArray()) s.artists << a.toObject()["name"].toString();
        s.lengthUs = qint64(seek->maximum()) * 1000;
        s.playing = playing;
        s.shuffle = shuffleOn;
        s.loop = repeatState == "track" ? "Track" : repeatState == "context" ? "Playlist" : "None";
        s.volume = volume->value() / 100.0;
        mpris->update(s);
    }

    // ---- keyboard shortcuts -----------------------------------------------------------

    void buildShortcuts() {
        auto focusSearch = [this] { search->setFocus(); search->selectAll(); };
        shortcuts = {
            {"Space", "Play or pause", [this] { playPause->click(); }},
            {"Ctrl+Right", "Next song", [this] { nextButton->click(); }},
            {"Ctrl+Left", "Previous song", [this] { prevButton->click(); }},
            {"Shift+Right", "Seek forward 5 seconds", [this] { seekTo(std::min<qint64>(positionMs() + 5000, seek->maximum())); }},
            {"Shift+Left", "Seek back 5 seconds", [this] { seekTo(std::max<qint64>(positionMs() - 5000, 0)); }},
            {"Ctrl+Up", "Volume up", [this] { setVolumePercent(volume->value() + 10); }},
            {"Ctrl+Down", "Volume down", [this] { setVolumePercent(volume->value() - 10); }},
            {"Ctrl+S", "Shuffle", [this] { setShuffle(!shuffleOn); }},
            {"Ctrl+R", "Repeat", [this] { setRepeat(nextRepeat(repeatState)); }},
            {"Alt+Shift+B", "Like the current song", [this] { if (!nowUri.isEmpty()) setLiked({nowUri}, !nowLiked); }},
            {"Ctrl+L", "Search", focusSearch},
            {"Alt+Left", "Go back", [this] { stepHistory(-1); }},
            {"Alt+Right", "Go forward", [this] { stepHistory(1); }},
            {"Alt+Shift+H", "Home", [this] { goNav(HomePage); }},
            {"Alt+Shift+A", "Your albums", [this] { goNav(AlbumsPage); }},
            {"Alt+Shift+S", "Liked Songs", [this] { goNav(LikedSongs); }},
            {"Alt+Shift+Q", "Queue", [this] { toggleQueue(sideStack->currentIndex() == 0); }},
            {"Alt+Shift+L", "Lyrics", [this] { lyricsButton->click(); }},
            {"Ctrl+N", "Create playlist", [this] { createPlaylist({}); }},
            {"Ctrl+M", "Mini player", [this] { toggleMini(); }},
            {"F11", "Full screen", [this] { toggleFull(); }},
            {"F1", "Keyboard shortcuts", [this] { showShortcuts(); }},
        };
        for (const auto &sc : shortcuts) new QShortcut(QKeySequence(sc.keys), this, sc.run);
        // Ctrl+/ is the usual sheet key on QWERTY; on AZERTY "/" needs Shift, so F1 is the listed one.
        new QShortcut(QKeySequence("Ctrl+/"), this, [this] { showShortcuts(); });
    }

    void showShortcuts() {
        QDialog dialog(this);
        dialog.setWindowTitle("Keyboard shortcuts");
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(24, 20, 24, 20);
        layout->addWidget(label("Keyboard shortcuts", "section"));
        auto *gridLayout = new QGridLayout;
        gridLayout->setHorizontalSpacing(40);
        gridLayout->setVerticalSpacing(8);
        int half = int(shortcuts.size() + 1) / 2;
        for (int i = 0; i < shortcuts.size(); ++i) {
            int col = i < half ? 0 : 2, row = i < half ? i : i - half;
            gridLayout->addWidget(label(shortcuts[i].what), row, col);
            gridLayout->addWidget(label(QKeySequence(shortcuts[i].keys).toString(QKeySequence::NativeText), "kbd"), row, col + 1, Qt::AlignRight);
        }
        layout->addLayout(gridLayout);
        auto *close = new QPushButton("Close");
        close->setObjectName("ghost");
        connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
        layout->addWidget(close, 0, Qt::AlignRight);
        dialog.exec();
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
    // playlist page
    QScrollArea *listPage = nullptr;
    QFrame *headBand, *listNote;
    QLabel *noteTitle, *noteBody;
    QToolButton *headShuffle, *headMore;
    QPushButton *listSortButton;
    QLineEdit *listFilter;
    TrackTitleDelegate *titleDelegate;
    QString listLead, listOwner, listSort = "custom", myName;
    QStringList listExtra;
    int listTotal = -1;
    qint64 listDurationMs = 0;
    bool listBlocked = false;
    QHash<QString, QJsonArray> listCache;  // track lists already read this session, by cache id
    // history, library, home, artist, queue, modes, extra views, media keys
    QList<std::function<void()>> history;
    int histPos = -1;
    QToolButton *backButton, *forwardButton, *sortButton, *prevButton, *nextButton, *shuffleButton, *repeatButton,
        *queueButton, *miniButton, *fullButton, *artistPlay;
    QPushButton *albumSortButton, *artistMore, *chipAlbums, *chipSingles, *queueTab, *recentTab;
    QLineEdit *libFilter, *albumFilter;
    QString librarySort = "recents", albumSort = "recent", repeatState = "off";
    int playlistOrder = 0, createdCount = 0, homeGen = 0, artistGen = 0, queueGen = 0;
    QScrollArea *homePage = nullptr, *artistPage = nullptr;
    QGridLayout *shortcutGrid;
    QListWidget *homeArtists = nullptr, *homeSongs = nullptr, *artistAlbums = nullptr, *queueList = nullptr;
    QLabel *homeEmpty, *homeArtistsTitle, *homeSongsTitle, *artistAvatar, *artistTitle, *artistMeta, *artistNoAlbums, *queueEmpty;
    QElapsedTimer homeLoadedAt;
    QStringList homeSongUris, artistSongUris;
    QList<QJsonObject> artistSongRows;
    bool artistSongsExpanded = false, shuffleOn = false;
    QString currentArtistId, nowContextUri, nowContextName;
    QTreeWidget *artistSongs;
    QStackedWidget *sideStack;
    MiniPlayer *mini = nullptr;
    FullScreenView *full = nullptr;
    Mpris *mpris = nullptr;
    QPixmap nowCover, bigCover;
    QColor nowMood;
    qint64 pushedSecond = -1;
    struct Shortcut {
        const char *keys, *what;
        std::function<void()> run;
    };
    QList<Shortcut> shortcuts;
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
    app.setWindowIcon(QIcon(appIcon(256, 1)));
    app.setDesktopFileName("ignify");  // matches ignify.desktop, so docks group and pin the window

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
