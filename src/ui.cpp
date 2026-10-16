#include "ui.h"
#include <QApplication>
#include <algorithm>
#include <tuple>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QStyleFactory>

namespace {
// Clicking a slider's groove jumps there instead of paging by a few milliseconds.
class Style : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    int styleHint(StyleHint hint, const QStyleOption *o, const QWidget *w, QStyleHintReturn *r) const override {
        if (hint == SH_Slider_AbsoluteSetButtons) return Qt::LeftButton;
        if (hint == SH_DialogButtonBox_ButtonsHaveIcons) return 0;  // no stock icons on OK/Cancel
        return QProxyStyle::styleHint(hint, o, w, r);
    }
};

void drawGlyph(QPainter &p, Glyph g, const QColor &color, const QRectF &box) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.topLeft());
    p.scale(box.width(), box.height());  // draw in unit coordinates
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    auto tri = [&p](QPointF a, QPointF b, QPointF c) { p.drawPolygon(QPolygonF{a, b, c}); };
    auto stroke = [&] {  // outline pen for the line glyphs; outline()/ring() restore the fill brush after
        QPen pen(color, .07);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
    };
    auto line = [&](QPointF a, QPointF b) { p.drawLine(a, b); };
    auto outline = [&](const QRectF &r, qreal radius) {
        stroke();
        p.drawRoundedRect(r, radius, radius);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
    };
    auto ring = [&](QPointF c, qreal r) {
        stroke();
        p.drawEllipse(c, r, r);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
    };
    switch (g) {
    case Glyph::Play: tri({.34, .22}, {.34, .78}, {.80, .50}); break;
    case Glyph::Pause:
        p.drawRoundedRect(QRectF(.27, .22, .15, .56), .03, .03);
        p.drawRoundedRect(QRectF(.58, .22, .15, .56), .03, .03);
        break;
    case Glyph::Prev:
        p.drawRoundedRect(QRectF(.24, .25, .08, .50), .02, .02);
        tri({.76, .25}, {.76, .75}, {.34, .50});
        break;
    case Glyph::Next:
        p.drawRoundedRect(QRectF(.68, .25, .08, .50), .02, .02);
        tri({.24, .25}, {.24, .75}, {.66, .50});
        break;
    case Glyph::Heart: {
        QPainterPath h;
        h.moveTo(.50, .82);
        h.cubicTo(.08, .55, .12, .18, .33, .20);
        h.cubicTo(.43, .21, .48, .28, .50, .34);
        h.cubicTo(.52, .28, .57, .21, .67, .20);
        h.cubicTo(.88, .18, .92, .55, .50, .82);
        p.drawPath(h);
        break;
    }
    case Glyph::Grid:
        for (qreal x : {.22, .54})
            for (qreal y : {.22, .54}) p.drawRoundedRect(QRectF(x, y, .24, .24), .04, .04);
        break;
    case Glyph::Volume: {
        p.drawPolygon(QPolygonF{{.16, .40}, {.30, .40}, {.48, .24}, {.48, .76}, {.30, .60}, {.16, .60}});
        QPen pen(color, .07);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(.42, .32, .26, .36), -50 * 16, 100 * 16);
        p.drawArc(QRectF(.42, .20, .44, .60), -50 * 16, 100 * 16);
        break;
    }
    case Glyph::HeartOutline: {
        QPainterPath h;
        h.moveTo(.50, .80);
        h.cubicTo(.10, .55, .14, .20, .34, .22);
        h.cubicTo(.43, .23, .48, .29, .50, .35);
        h.cubicTo(.52, .29, .57, .23, .66, .22);
        h.cubicTo(.86, .20, .90, .55, .50, .80);
        stroke();
        p.drawPath(h);
        break;
    }
    case Glyph::Plus:  // Spotify's "add to playlist": plus in a circle
        ring(QPointF(.50, .50), .36);
        p.drawRoundedRect(QRectF(.47, .32, .06, .36), .02, .02);
        p.drawRoundedRect(QRectF(.32, .47, .36, .06), .02, .02);
        break;
    case Glyph::Check: {
        QPen pen(color, .12);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF{{.26, .52}, {.43, .68}, {.74, .34}});
        break;
    }
    case Glyph::Shuffle: {  // two crossing paths, arrowheads on the right
        stroke();
        QPainterPath a(QPointF(.12, .30)), b(QPointF(.12, .70));
        a.cubicTo(.44, .30, .48, .70, .78, .70);
        b.cubicTo(.44, .70, .48, .30, .78, .30);
        p.drawPath(a);
        p.drawPath(b);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        tri({.76, .20}, {.76, .40}, {.92, .30});
        tri({.76, .60}, {.76, .80}, {.92, .70});
        break;
    }
    case Glyph::Repeat:
    case Glyph::RepeatOne:
        stroke();
        p.drawRoundedRect(QRectF(.14, .28, .72, .44), .14, .14);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        tri({.56, .18}, {.56, .38}, {.70, .28});  // direction of travel on the top edge
        if (g == Glyph::RepeatOne) {
            stroke();
            line({.50, .42}, {.50, .62});
            line({.44, .47}, {.50, .42});
        }
        break;
    case Glyph::Queue:
        stroke();
        line({.14, .28}, {.62, .28});
        line({.14, .48}, {.62, .48});
        line({.14, .68}, {.48, .68});
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        tri({.62, .58}, {.62, .86}, {.88, .72});
        break;
    case Glyph::MiniPlayer:
        outline(QRectF(.10, .20, .80, .60), .08);
        p.drawRoundedRect(QRectF(.48, .46, .32, .26), .04, .04);
        break;
    case Glyph::FullScreen:
        stroke();
        for (auto [x, y, dx, dy] : {std::tuple{.16, .16, 1, 1}, {.84, .16, -1, 1}, {.16, .84, 1, -1}, {.84, .84, -1, -1}}) {
            line({x, y + dy * .22}, {x, y});
            line({x, y}, {x + dx * .22, y});
        }
        break;
    case Glyph::Home: {
        stroke();
        QPainterPath h(QPointF(.18, .46));
        h.lineTo(.50, .18);
        h.lineTo(.82, .46);
        h.lineTo(.82, .84);
        h.lineTo(.18, .84);
        h.closeSubpath();
        p.drawPath(h);
        break;
    }
    case Glyph::Back:
    case Glyph::Forward: {
        QPen pen(color, .09);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        qreal tip = g == Glyph::Back ? .34 : .66, tail = g == Glyph::Back ? .62 : .38;
        p.drawPolyline(QPolygonF{{tail, .20}, {tip, .50}, {tail, .80}});
        break;
    }
    case Glyph::Search:
        stroke();
        p.drawEllipse(QPointF(.44, .44), .26, .26);
        line({.63, .63}, {.84, .84});
        break;
    case Glyph::Sort:
        stroke();
        line({.18, .30}, {.82, .30});
        line({.18, .50}, {.66, .50});
        line({.18, .70}, {.50, .70});
        break;
    case Glyph::Clock:
        stroke();
        p.drawEllipse(QPointF(.50, .50), .36, .36);
        line({.50, .50}, {.50, .28});
        line({.50, .50}, {.66, .58});
        break;
    case Glyph::More:
        for (qreal x : {.22, .50, .78}) p.drawEllipse(QPointF(x, .50), .07, .07);
        break;
    case Glyph::Close:
        stroke();
        line({.26, .26}, {.74, .74});
        line({.74, .26}, {.26, .74});
        break;
    case Glyph::Mic:
        outline(QRectF(.38, .10, .24, .46), .12);
        stroke();
        p.drawArc(QRectF(.26, .26, .48, .44), 180 * 16, 180 * 16);  // cradle under the capsule
        p.drawLine(QPointF(.50, .70), QPointF(.50, .86));
        p.drawLine(QPointF(.38, .88), QPointF(.62, .88));
        break;
    // Device icons are outlines, like Spotify's Connect picker.
    case Glyph::Computer:
        outline(QRectF(.12, .20, .76, .48), .06);
        p.drawRect(QRectF(.44, .68, .12, .10));
        p.drawRoundedRect(QRectF(.30, .78, .40, .06), .02, .02);
        break;
    case Glyph::Phone:
        outline(QRectF(.30, .10, .40, .80), .10);
        p.drawEllipse(QPointF(.50, .76), .04, .04);
        break;
    case Glyph::Speaker:
        outline(QRectF(.24, .10, .52, .80), .08);
        p.drawEllipse(QPointF(.50, .30), .05, .05);
        ring(QPointF(.50, .60), .14);
        break;
    case Glyph::Devices:  // the "Connect to a device" button: a phone beside a speaker
        outline(QRectF(.42, .10, .48, .80), .08);
        ring(QPointF(.66, .60), .12);
        p.drawEllipse(QPointF(.66, .28), .045, .045);
        outline(QRectF(.08, .36, .24, .54), .05);
        break;
    }
    p.restore();
}
} // namespace

void theme::apply(QApplication &app) {
    app.setStyle(new Style(QStyleFactory::create("Fusion")));
    QPalette pal;
    pal.setColor(QPalette::Window, black);
    pal.setColor(QPalette::WindowText, white);
    pal.setColor(QPalette::Base, base);
    pal.setColor(QPalette::AlternateBase, elevated);
    pal.setColor(QPalette::Text, white);
    pal.setColor(QPalette::Button, elevated);
    pal.setColor(QPalette::ButtonText, white);
    pal.setColor(QPalette::Highlight, green);
    pal.setColor(QPalette::HighlightedText, black);
    pal.setColor(QPalette::ToolTipBase, highlight);
    pal.setColor(QPalette::ToolTipText, white);
    pal.setColor(QPalette::PlaceholderText, subdued);
    pal.setColor(QPalette::Link, subdued);
    pal.setColor(QPalette::Disabled, QPalette::Text, subdued);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, subdued);
    app.setPalette(pal);
    app.setFont(QFont("Inter", 10));
    // Inter has no emoji; without this Qt 6.4 falls back to monochrome or missing glyphs
    // ("Prog \U0001F9DC" showed a sliver) instead of the colour emoji font fontconfig prefers.
    for (const char *family : {"Inter", "Inter Display"}) QFont::insertSubstitutions(family, {"Noto Color Emoji"});

    // Spotify's layout: black window, rounded #121212 panels, green for the main action.
    // Named tokens, not %1..%n: QString::arg() fills whichever placeholders exist in order,
    // so one unused colour silently shifts every later one.
    QString qss = R"(
#main, #bar, QStatusBar { background: @black; }
#sidebar, #pages, #panel { background: @base; border-radius: 8px; }
QStackedWidget > QWidget { background: transparent; }
#brand { font-family: "Inter Display"; font-size: 22px; font-weight: 800; }
#h1 { font-family: "Inter Display"; font-size: 40px; font-weight: 800; }
#npTitle { font-family: "Inter Display"; font-size: 22px; font-weight: 700; }
#npArtist { font-size: 14px; color: @subdued; }
#muted, #kind { color: @subdued; }
#kind { font-size: 12px; font-weight: 600; color: @white; }

QLineEdit { background: @highlight; border: 1px solid transparent; border-radius: 18px; padding: 8px 14px; }
QLineEdit:hover { border-color: #3E3E3E; }
QLineEdit:focus { border: 2px solid @white; padding: 7px 13px; }

QListWidget, QTreeWidget { background: transparent; border: none; outline: 0; }
#nav::item { padding: 6px; border-radius: 6px; }
#nav::item:hover { background: @highlight; }
#nav::item:selected { background: #3A3A3A; color: @white; }

QTreeWidget::item { height: 40px; border: none; padding-left: 4px; }
QTreeWidget::item:hover { background: @highlight; }
QTreeWidget::item:selected { background: #3E3E3E; color: @white; }
QHeaderView, QHeaderView::section { background: transparent; color: @subdued; border: none; }
QHeaderView::section { border-bottom: 1px solid @highlight; padding: 0 8px 8px; font-size: 12px; }

QScrollBar:vertical { background: transparent; width: 12px; }
QScrollBar::handle:vertical { background: rgba(255,255,255,0.28); min-height: 40px; margin: 2px 2px; }
QScrollBar::handle:vertical:hover { background: rgba(255,255,255,0.5); }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QScrollBar:horizontal { height: 0; }

QSlider::groove:horizontal { height: 4px; background: #4D4D4D; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @white; border-radius: 2px; }
QSlider::handle:horizontal { width: 12px; margin: -4px 0; border-radius: 6px; background: @white; }
QSlider::sub-page:horizontal:disabled, QSlider::handle:horizontal:disabled { background: #535353; }

QToolButton { border: none; background: transparent; border-radius: 16px; padding: 6px; }
QToolButton:hover { background: @highlight; }
QToolButton:focus { border: 1px solid @subdued; }
#play { background: @white; border-radius: 20px; }
#play:hover { background: #F0F0F0; }
#bigPlay { background: @green; border-radius: 28px; }
#bigPlay:hover { background: @greenhover; }

QStatusBar { color: @subdued; }
QToolTip { background: @highlight; color: @white; border: none; padding: 4px 8px; }
QMenu { background: #282828; border: none; border-radius: 6px; padding: 4px; }
QMenu::item { padding: 9px 22px 9px 10px; border-radius: 3px; color: @white; }
QMenu::item:selected { background: #3E3E3E; }
QMenu::item:disabled { color: @subdued; }
QMenu::icon { padding-left: 8px; }
QMenu::separator { height: 1px; background: #3E3E3E; margin: 4px 8px; }
#connectBar { background: @green; color: @black; font-weight: 700; padding: 3px 16px; }
#currentDevice { color: @green; font-weight: 700; }
#panelScroll, #panelScroll > QWidget, #panelScroll > QWidget > QWidget,
#lyricsScroll, #lyricsScroll > QWidget, #lyricsScroll > QWidget > QWidget { background: transparent; border: none; }
#artistCard { background: rgba(255,255,255,0.06); border-radius: 8px; }
#artistName { font-family: "Inter Display"; font-size: 17px; font-weight: 700; }
#aboutBadge { color: @white; font-weight: 700; background: rgba(0,0,0,0.45); border-radius: 4px; padding: 3px 7px; }
#lyric { font-family: "Inter Display"; font-size: 30px; font-weight: 800; }
#lyricsNote { color: rgba(255,255,255,0.7); font-size: 13px; }
#saveIn QListWidget::indicator { width: 16px; height: 16px; border-radius: 9px; border: 2px solid #727272; margin-right: 6px; }
#saveIn QListWidget::indicator:checked { background: @green; border-color: @green; image: url(@checkimg); }
#done { background: @green; color: @black; font-weight: 700; border: none; border-radius: 14px; padding: 6px 20px; }
#done:hover { background: @greenhover; }
#tab { color: @subdued; font-weight: 700; border: none; border-bottom: 2px solid transparent; border-radius: 0; padding: 6px 2px; margin-right: 14px; }
#tab:checked { color: @white; border-bottom-color: @green; }
#tab:hover { color: @white; background: transparent; }
#section { font-family: "Inter Display"; font-size: 22px; font-weight: 700; }
#shortcut { background: rgba(255,255,255,0.07); border-radius: 4px; }
#shortcut:hover { background: rgba(255,255,255,0.14); }
#shortcutName { font-weight: 700; }
#chip { background: @highlight; color: @white; border: none; border-radius: 15px; padding: 5px 14px; min-height: 20px; font-weight: 600; }
#chip:checked { background: @white; color: @black; }
#chip:hover { background: #3E3E3E; }
#chip:checked:hover { background: #F0F0F0; }
#ghost { background: transparent; color: @white; border: 1px solid #727272; border-radius: 15px; padding: 5px 16px; min-height: 20px; font-weight: 700; }
#ghost:hover { border-color: @white; }
#libFilter { padding: 5px 10px; border-radius: 4px; }
#homeScroll, #homeScroll > QWidget, #homeScroll > QWidget > QWidget,
#artistScroll, #artistScroll > QWidget, #artistScroll > QWidget > QWidget,
#sideStack, #sideStack > QWidget { background: transparent; border: none; }
#kbd { background: @highlight; border-radius: 4px; padding: 3px 8px; font-weight: 700; }
#listScroll, #listScroll > QWidget, #listScroll > QWidget > QWidget { background: transparent; border: none; }
#listHead { border-top-left-radius: 8px; border-top-right-radius: 8px; }
#trackTable::item { height: 56px; }
#sortText { background: transparent; color: @subdued; border: none; font-weight: 600; padding: 6px 4px; }
#sortText:hover { color: @white; }
#listTitle { font-family: "Inter Display"; font-weight: 800; color: @white; }
#noteTitle { font-family: "Inter Display"; font-size: 20px; font-weight: 700; }
)";
    const std::pair<const char *, QColor> tokens[] = {  // longest names first: @greenhover before @green
        {"@greenhover", greenHover}, {"@highlight", highlight}, {"@elevated", elevated}, {"@subdued", subdued},
        {"@black", black}, {"@white", white}, {"@green", green}, {"@base", base}};
    for (const auto &[name, color] : tokens) qss.replace(name, color.name());
    // QSS draws check marks only from an image file, so paint ours once into the cache.
    QString check = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/check.png";
    QDir().mkpath(QFileInfo(check).path());
    glyphIcon(Glyph::Check, black, 16).pixmap(QSize(16, 16), 2.0).save(check);
    qss.replace("@checkimg", check);
    // Keep each border-radius at most half its widget's height: past that, Qt drops the rounding.
    app.setStyleSheet(qss);
}

void paintGlyph(QPainter &p, Glyph g, const QColor &color, const QRectF &box) { drawGlyph(p, g, color, box); }

QIcon toggleIcon(Glyph g, bool on, int size) {
    const qreal dpr = 2;
    QPixmap pm(QSize(size, size + 6) * dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    drawGlyph(p, g, on ? theme::green : theme::white, QRectF(0, 0, size * dpr, size * dpr));
    if (on) {
        p.setPen(Qt::NoPen);
        p.setBrush(theme::green);
        p.drawEllipse(QPointF(size * dpr / 2, (size + 4) * dpr), 2 * dpr, 2 * dpr);
    }
    p.end();
    pm.setDevicePixelRatio(dpr);
    return QIcon(pm);
}

QIcon glyphIcon(Glyph g, const QColor &color, int size) {
    const qreal dpr = 2;  // drawn at 2x: crisp on HiDPI, downsampled cleanly elsewhere
    QPixmap pm(QSize(size, size) * dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    drawGlyph(p, g, color, QRectF(0, 0, size * dpr, size * dpr));
    p.end();
    pm.setDevicePixelRatio(dpr);
    return QIcon(pm);
}

// Painted in device pixels, dpr stamped on at the end, so text-sharp at any scale.
QPixmap rounded(const QPixmap &src, QSize size, qreal radius, qreal dpr) {
    QSize px = size * dpr;
    QPixmap out(px);
    out.fill(Qt::transparent);
    if (!src.isNull()) {  // cover-fit: crop the centre (biased up, where faces usually are)
        QPixmap fit = src.scaled(px, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        QBrush brush(fit);  // a textured brush keeps the rounded edge antialiased; clip paths don't
        brush.setTransform(QTransform::fromTranslate((px.width() - fit.width()) / 2.0,
                                                     (px.height() - fit.height()) / 3.0));
        p.setBrush(brush);
        p.drawRoundedRect(QRectF(QPointF(0, 0), QSizeF(px)), radius * dpr, radius * dpr);
    }
    out.setDevicePixelRatio(dpr);
    return out;
}

QPixmap tile(int size, qreal radius, qreal dpr, const QColor &bg, const QColor &fg, const Glyph *glyph,
             const QColor &bg2) {
    int px = qRound(size * dpr);
    QPixmap out(px, px);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    if (bg2.isValid()) {
        QLinearGradient grad(0, 0, px, px);
        grad.setColorAt(0, bg);
        grad.setColorAt(1, bg2);
        p.setBrush(grad);
    } else {
        p.setBrush(bg);
    }
    p.drawRoundedRect(QRectF(0, 0, px, px), radius * dpr, radius * dpr);
    if (glyph) {  // half the tile on thumbnails, capped so big tiles stay calm
        qreal g = std::min(px * .5, 56 * dpr);
        drawGlyph(p, *glyph, fg, QRectF((px - g) / 2, (px - g) / 2, g, g));
    }
    p.end();
    out.setDevicePixelRatio(dpr);
    return out;
}

// Spotify's mark: green disc, three black arcs that shorten and thin towards the bottom.
QPixmap spotifyLogo(int size, qreal dpr) {
    int px = qRound(size * dpr);
    QPixmap out(px, px);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(px, px);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::green);
    p.drawEllipse(QRectF(0, 0, 1, 1));
    p.setBrush(Qt::NoBrush);
    struct Arc { QPointF from, ctrl, to; qreal width; };
    for (const Arc &a : {Arc{{.22, .36}, {.52, .23}, {.80, .41}, .105},
                         Arc{{.26, .53}, {.51, .42}, {.74, .56}, .085},
                         Arc{{.30, .69}, {.50, .61}, {.69, .71}, .068}}) {
        QPen pen(theme::black, a.width);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        QPainterPath path(a.from);
        path.quadTo(a.ctrl, a.to);
        p.drawPath(path);
    }
    p.end();
    out.setDevicePixelRatio(dpr);
    return out;
}

// ignify's own mark, so Spotify's logo is only ever attribution (its terms forbid it as an app icon).
QPixmap appIcon(int size, qreal dpr) {
    int px = qRound(size * dpr);
    QPixmap out(px, px);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(px, px);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::base);
    p.drawRoundedRect(QRectF(0, 0, 1, 1), .22, .22);
    QPainterPath flame;
    flame.moveTo(.50, .12);
    flame.cubicTo(.60, .30, .80, .40, .78, .62);
    flame.cubicTo(.76, .80, .64, .90, .50, .90);
    flame.cubicTo(.36, .90, .22, .80, .22, .62);
    flame.cubicTo(.22, .48, .32, .40, .37, .30);
    flame.cubicTo(.40, .40, .44, .45, .49, .47);
    flame.cubicTo(.48, .35, .45, .24, .50, .12);
    QLinearGradient fire(0, .12, 0, .90);
    fire.setColorAt(0, theme::greenHover);
    fire.setColorAt(1, theme::green);
    p.setBrush(fire);
    p.drawPath(flame);
    QPainterPath core;  // the dark heart of the flame
    core.moveTo(.50, .56);
    core.cubicTo(.59, .65, .63, .72, .61, .78);
    core.cubicTo(.59, .85, .41, .85, .39, .78);
    core.cubicTo(.37, .71, .43, .64, .50, .56);
    p.setBrush(theme::base);
    p.drawPath(core);
    p.end();
    out.setDevicePixelRatio(dpr);
    return out;
}

QColor moodColor(const QPixmap &cover) {
    // An 8x8 smooth downscale averages regions; the most vivid region beats the muddy global mean.
    QImage img = cover.toImage().scaled(8, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QColor best;
    float bestScore = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            QColor c = img.pixelColor(x, y);
            float score = c.hsvSaturationF() * c.valueF();
            if (score > bestScore) bestScore = score, best = c;
        }
    return bestScore < 0.12f ? QColor() : best;  // invalid = monochrome cover
}

QColor washFrom(const QColor &mood) {
    if (!mood.isValid()) return theme::highlight;
    return QColor::fromHslF(mood.hslHueF(), std::min(mood.hslSaturationF(), 0.55f), 0.24f);
}

// Small on purpose: every view keeps its own scaled copy, so this only bridges repeat loads.
ImageCache::ImageCache(QObject *parent) : QObject(parent), memory(16 * 1024) {
    auto *disk = new QNetworkDiskCache(this);
    disk->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/images");
    disk->setMaximumCacheSize(qint64(200) << 20);
    net.setCache(disk);
}

void ImageCache::load(const QString &url, QObject *context, Done done) {
    if (url.isEmpty()) return;
    if (auto *pm = memory.object(url)) return done(*pm);
    auto &waiters = pending[url];
    waiters.append({context, std::move(done)});
    if (waiters.size() > 1) return;  // already downloading
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    auto *reply = net.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        QPixmap pm;
        // ponytail: decodes on the UI thread (a few ms per cover); move to QtConcurrent if grids stutter
        if (!pm.loadFromData(reply->readAll())) return void(pending.remove(url));
        memory.insert(url, new QPixmap(pm), pm.width() * pm.height() * 4 / 1024);
        for (auto &[ctx, cb] : pending.take(url))
            if (ctx) cb(pm);
    });
}

void CardDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(Qt::NoPen);
    QRectF cell = QRectF(opt.rect).adjusted(3, 3, -3, -3);
    if (opt.state & (QStyle::State_MouseOver | QStyle::State_Selected)) {
        p->setBrush(theme::elevated);
        p->drawRoundedRect(cell, 10, 10);
    }
    if (opt.state & QStyle::State_HasFocus) {  // keyboard users need to see where they are
        p->setPen(QPen(theme::subdued, 1));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(cell, 10, 10);
        p->setPen(Qt::NoPen);
    }

    QRect cover(opt.rect.x() + Pad, opt.rect.y() + Pad, Cover, Cover);
    auto pm = idx.data(Qt::DecorationRole).value<QPixmap>();
    if (pm.isNull()) {
        p->setBrush(theme::highlight);
        if (idx.data(Role::Kind).toInt() == Round) p->drawEllipse(cover);
        else p->drawRoundedRect(cover, 6, 6);
    } else {
        p->drawPixmap(cover, pm);
    }

    QFont f = opt.font;
    f.setWeight(QFont::DemiBold);
    QRect line(cover.left(), cover.bottom() + 10, Cover, 19);
    p->setFont(f);
    p->setPen(theme::white);
    p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(f).elidedText(idx.data().toString(), Qt::ElideRight, Cover));
    f.setWeight(QFont::Normal);
    p->setFont(f);
    p->setPen(theme::subdued);
    p->drawText(line.translated(0, 19), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(f).elidedText(idx.data(Role::Subtitle).toString(), Qt::ElideRight, Cover));
    p->restore();
}

QSize RowDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &idx) const {
    return {0, idx.data(Role::Kind).toInt() == Header ? 44 : 58};
}

void RowDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QRect r = opt.rect;
    QFont f = opt.font;
    int kind = idx.data(Role::Kind).toInt();
    if (kind == Header) {
        f.setPointSizeF(f.pointSizeF() * 1.15);
        f.setWeight(QFont::Bold);
        p->setFont(f);
        p->setPen(theme::white);
        p->drawText(r.adjusted(8, 0, -8, -6), Qt::AlignLeft | Qt::AlignBottom,
                    QFontMetrics(f).elidedText(idx.data().toString(), Qt::ElideRight, r.width() - 16));
        p->restore();
        return;
    }
    if (opt.state & (QStyle::State_MouseOver | QStyle::State_Selected)) {
        p->setPen(Qt::NoPen);
        p->setBrush(theme::highlight);
        p->drawRoundedRect(QRectF(r).adjusted(2, 1, -2, -1), 6, 6);
    }
    QRect cover(r.x() + 8, r.y() + (r.height() - 40) / 2, 40, 40);
    auto pm = idx.data(Qt::DecorationRole).value<QPixmap>();
    if (pm.isNull()) {
        p->setPen(Qt::NoPen);
        p->setBrush(theme::highlight);
        p->drawRoundedRect(cover, 4, 4);
    } else {
        p->drawPixmap(cover, pm);
    }
    int textX = cover.right() + 12, textW = r.right() - textX - 8;
    f.setWeight(QFont::Medium);
    p->setFont(f);
    p->setPen(kind == Current ? theme::green : theme::white);
    p->drawText(QRect(textX, r.y() + 9, textW, 20), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(f).elidedText(idx.data().toString(), Qt::ElideRight, textW));
    f.setWeight(QFont::Normal);
    p->setFont(f);
    p->setPen(theme::subdued);
    p->drawText(QRect(textX, r.y() + 29, textW, 20), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(f).elidedText(idx.data(Role::Subtitle).toString(), Qt::ElideRight, textW));
    p->restore();
}

void TrackTitleDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &idx) const {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, idx);
    QString title = opt.text;
    opt.text.clear();
    opt.icon = QIcon();
    const QWidget *w = option.widget;
    (w ? w->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);  // hover / selection

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QRect r = option.rect;
    int x = r.x() + 4;
    if (thumbs) {
        QRect cover(x, r.y() + (r.height() - 40) / 2, 40, 40);
        auto pm = idx.data(Qt::DecorationRole).value<QPixmap>();
        if (pm.isNull()) {
            p->setPen(Qt::NoPen);
            p->setBrush(theme::highlight);
            p->drawRoundedRect(cover, 4, 4);
        } else {
            p->drawPixmap(cover, pm);
        }
        x = cover.right() + 12;
    }
    int width = r.right() - x - 8;
    QFont f = option.font;
    f.setWeight(QFont::Medium);
    p->setFont(f);
    QVariant fg = idx.data(Qt::ForegroundRole);
    p->setPen(fg.isValid() ? fg.value<QBrush>().color() : theme::white);
    p->drawText(QRect(x, r.y() + 8, width, 20), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(f).elidedText(title, Qt::ElideRight, width));
    f.setWeight(QFont::Normal);
    p->setFont(f);
    p->setPen(theme::subdued);
    p->drawText(QRect(x, r.y() + 28, width, 20), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(f).elidedText(idx.data(Role::Subtitle).toString(), Qt::ElideRight, width));
    p->restore();
}
