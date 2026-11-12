// `VaxMaps --selftest [outdir] [--bench]`: assert-based checks of the core logic and of real mouse
// interaction on an offscreen window, plus screenshots in outdir.
#include "calibration.h"
#include "canvas.h"
#include "items.h"
#include "lang.h"
#include "mainwindow.h"
#include "mapitem.h"
#include "project.h"
#include "ui.h"

#include <private/qzipwriter_p.h>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsScene>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLockFile>
#include <QMessageBox>
#include <QTimer>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextStream>
#include <QThread>
#include <QTreeWidget>
#include <QWheelEvent>
#include <cmath>
#include <functional>

namespace {

int failures = 0;
QFile *logFile = nullptr;

void say(const QString &s) {
    QTextStream(stdout) << s << Qt::endl;
    if (logFile) QTextStream(logFile) << s << Qt::endl;
}

void check(bool ok, const QString &what) {
    say(QString(ok ? "ok    %1" : "FAIL  %1").arg(what));
    if (!ok) ++failures;
}

bool waitFor(const std::function<bool()> &cond, int ms) {
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

// Repaint until no tile is waiting to load.
void settle(Canvas *c) {
    for (int i = 0; i < 6; ++i) {
        c->viewport()->repaint();
        waitFor([] { return tileLoadsBusy() == 0; }, 20000);
    }
}

QColor bg(int x, int y, int w, int h) { return QColor(40 + 160 * x / w, 90, 40 + 160 * y / h); }

quint32 rnd() { static quint32 seed = 12345; seed = seed * 1664525u + 1013904223u; return seed >> 8; }

QString makeMap(const QString &dir, const QString &name, int w, int h, int cell, QPoint origin, bool alpha, int noise = 0) {
    QImage img(w, h, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int n = noise ? int(rnd() % (2 * noise + 1)) - noise : 0;
            const QColor c = bg(x, y, w, h);
            line[x] = qRgb(qBound(0, c.red() + n, 255), qBound(0, c.green() + n, 255), qBound(0, c.blue() + n, 255));
        }
    }
    QPainter p(&img);
    p.setPen(QPen(QColor(20, 20, 20), 3));
    for (int x = origin.x(); x < w; x += cell) p.drawLine(x, 0, x, h);
    for (int y = origin.y(); y < h; y += cell) p.drawLine(0, y, w, y);
    if (alpha) {
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(0, 0, w / 4, h / 4, Qt::transparent);
    }
    p.end();
    const QString path = dir + "/" + name;
    img.save(path, nullptr, 95);
    return path;
}

// Textured floor, walls at random places and (optionally) a grid of `cell` px starting at `origin`.
QImage gridImage(int w, int h, double cell, QPointF origin, int lineAlpha, int noise) {
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int v = 150 + int(rnd() % (2 * noise + 1)) - noise;
            line[x] = qRgb(v, v * 9 / 10, v * 7 / 10);
        }
    }
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(50, 35, 25), 7));
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < 14; ++i) p.drawRect(QRectF(rnd() % w, rnd() % h, 80 + rnd() % (w / 3), 80 + rnd() % (h / 3)));
    if (lineAlpha > 0) {
        p.setPen(QPen(QColor(0, 0, 0, lineAlpha), 2));
        for (double x = std::fmod(origin.x(), cell); x < w; x += cell) p.drawLine(QPointF(x, 0), QPointF(x, h));
        for (double y = std::fmod(origin.y(), cell); y < h; y += cell) p.drawLine(QPointF(0, y), QPointF(w, y));
    }
    return img;
}

// Painted art without a grid: strokes at any angle, round blotches, shading.
QImage paintedImage(int w, int h, bool dark) {
    QImage img = dark ? QImage(w, h, QImage::Format_RGB32) : gridImage(w, h, 1, {}, 0, 40);
    if (dark) img.fill(QColor(12, 12, 14));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; dark ? i < 0 : i < 500; ++i) {
        const QPointF a(rnd() % w, rnd() % h);
        const double ang = (rnd() % 3600) / 10.0 * M_PI / 180, len = 40 + rnd() % 400;
        p.setPen(QPen(QColor(rnd() % 120, rnd() % 120, rnd() % 120), 1 + rnd() % 6));
        if (i % 3) p.drawLine(a, a + QPointF(std::cos(ang), std::sin(ang)) * len);
        else {
            p.setBrush(QColor(60 + rnd() % 120, 80 + rnd() % 120, 40 + rnd() % 80));
            p.drawEllipse(a, 10 + rnd() % 60, 10 + rnd() % 60);
            p.setBrush(Qt::NoBrush);
        }
    }
    return img;
}

// A map whose squares cover only `area`: elsewhere painted art (or black, as around a dungeon's rooms).
QImage partialGridImage(int w, int h, double cell, QPointF origin, const QPainterPath &area, bool dark) {
    QImage img = paintedImage(w, h, dark);
    const QImage grid = gridImage(w, h, cell, origin, 200, 25);
    QPainter p(&img);
    p.setClipPath(area);
    p.drawImage(0, 0, grid);
    return img;
}

// A dungeon on black, in cells of `cell` px: rooms A (cells 2-9 x 2-9) and B (13-20 x 3-10) joined by a corridor one
// cell wide (row 5), walls a fifth of a cell thick with doorways into the corridor, a faint grid on the floor.
QImage dungeonImage(int w, int h, int cell) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(QColor(18, 18, 20));
    QPainterPath floor;
    for (QRect r : {QRect(2, 2, 8, 8), QRect(13, 3, 8, 8), QRect(10, 5, 3, 1)}) floor.addRect(r.x() * cell, r.y() * cell, r.width() * cell, r.height() * cell);
    QPainter p(&img);
    p.fillPath(floor, QColor(178, 166, 146));
    p.setClipPath(floor);
    p.setPen(QPen(QColor(120, 110, 96), 1));
    for (int x = 0; x < w; x += cell) p.drawLine(x, 0, x, h);
    for (int y = 0; y < h; y += cell) p.drawLine(0, y, w, y);
    p.setClipping(false);
    p.setPen(QPen(QColor(45, 38, 32), cell * 0.2, Qt::SolidLine, Qt::SquareCap));
    const int walls[][4] = {{2, 2, 10, 2}, {2, 2, 2, 10}, {2, 10, 10, 10}, {10, 2, 10, 5}, {10, 6, 10, 10},      // A, doorway at row 5
                            {10, 5, 13, 5}, {10, 6, 13, 6},                                                      // corridor
                            {13, 3, 21, 3}, {21, 3, 21, 11}, {13, 11, 21, 11}, {13, 3, 13, 5}, {13, 6, 13, 11}};  // B
    for (const auto &l : walls) p.drawLine(l[0] * cell, l[1] * cell, l[2] * cell, l[3] * cell);
    p.end();
    for (int y = 0; y < h; ++y) {   // stone: noise on the floor
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x)
            if (qRed(line[x]) > 100) {
                const int n = int(rnd() % 25) - 12;
                line[x] = qRgb(qBound(0, qRed(line[x]) + n, 255), qBound(0, qGreen(line[x]) + n, 255), qBound(0, qBlue(line[x]) + n, 255));
            }
    }
    return img;
}

// Square grid with lines at the given positions; horizontal lines tilt by `slope` (y grows with x).
QImage linesImage(int w, int h, const QVector<double> &xs, const QVector<double> &ys, double slope) {
    QImage img = gridImage(w, h, 1, {}, 0, 25);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0, 0, 0, 200), 2));
    for (double x : xs) p.drawLine(QPointF(x, 0), QPointF(x, h));
    for (double y : ys) p.drawLine(QPointF(0, y), QPointF(w, y + slope * w));
    return img;
}

// Hex grid edges for a calibration (any orientation).
QImage hexImage(int w, int h, const Calibration &c) {
    QImage img = gridImage(w, h, 1, {}, 0, 25);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    drawGrid(&p, c, QRectF(0, 0, w, h), 1, QColor(0, 0, 0, 200));
    return img;
}

constexpr double SQRT3_TEST = 1.7320508075688772;

// A printed hex map: each hex filled with a terrain colour, thick edges on top.
QImage terrainHexImage(int w, int h, const Calibration &c, double pen = 3, int alpha = 255, int noise = 20) {
    QImage img = gridImage(w, h, 1, {}, 0, noise);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor terrain[] = {{96, 140, 70}, {60, 100, 50}, {170, 150, 100}, {120, 110, 100}, {70, 110, 160}, {200, 190, 140}};
    QPolygonF hexagon;
    for (int k = 0; k < 6; ++k) {   // the hex's vertices, from the lattice: vertex = centre + (a + b) / 3 rotated in 60° steps
        const double ang = (c.grid == Calibration::HexPointy ? 30 : 0) + 60 * k;
        hexagon << QPointF(std::cos(ang * M_PI / 180), std::sin(ang * M_PI / 180)) * c.cellSize() / SQRT3_TEST;
    }
    p.setPen(QPen(QColor(40, 35, 30, alpha), pen));
    for (int i = -60; i <= 60; ++i)
        for (int j = -60; j <= 60; ++j) {
            const QPointF q = c.origin + i * c.a + j * c.b;
            if (q.x() < -c.cellSize() || q.y() < -c.cellSize() || q.x() > w + c.cellSize() || q.y() > h + c.cellSize()) continue;
            p.setBrush(terrain[rnd() % 6]);
            p.drawPolygon(hexagon.translated(q));
        }
    return img;
}

// Worst distance, in cells, between where the calibration puts lattice points and where they really are.
double latticeError(const Calibration &c, const QVector<QPointF> &truth) {
    double worst = 0;
    for (const QPointF &p : truth) {
        const QPointF uv = c.toGrid(p);
        worst = std::max({worst, std::abs(uv.x() - std::round(uv.x())), std::abs(uv.y() - std::round(uv.y()))});
    }
    return worst;
}

QVector<QPointF> latticePoints(const Calibration &c, int w, int h, int margin) {
    QVector<QPointF> pts;
    for (int i = -200; i <= 200; ++i)
        for (int j = -200; j <= 200; ++j) {
            const QPointF p = c.origin + i * c.a + j * c.b;
            if (p.x() > margin && p.y() > margin && p.x() < w - margin && p.y() < h - margin) pts << p;
        }
    return pts;
}

double offBy(double v, double target, double cell) {
    const double r = std::fmod(std::fmod(v - target, cell) + cell, cell);
    return std::min(r, cell - r);
}

void key(Canvas *c, int k, const QString &text = {}, Qt::KeyboardModifiers mods = {}) {
    QKeyEvent e(QEvent::KeyPress, k, mods, text);
    QApplication::sendEvent(c, &e);
}

void mouse(Canvas *c, QEvent::Type type, QPointF scenePos, Qt::MouseButton b = Qt::LeftButton, Qt::KeyboardModifiers mods = {}) {
    const QPointF vp = c->mapFromScene(scenePos);
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(b);
    QMouseEvent e(type, vp, c->viewport()->mapToGlobal(vp), type == QEvent::MouseMove ? Qt::NoButton : b, held, mods);
    QApplication::sendEvent(c->viewport(), &e);
}

void click(Canvas *c, QPointF scenePos) {
    mouse(c, QEvent::MouseButtonPress, scenePos);
    mouse(c, QEvent::MouseButtonRelease, scenePos);
}

// Drag in view pixels (handles are hit by their on-screen position).
void dragView(Canvas *c, QPoint a, QPoint b, Qt::KeyboardModifiers mods = {}) {
    auto send = [&](QEvent::Type t, QPoint p, Qt::MouseButton btn, Qt::MouseButtons held) {
        QMouseEvent e(t, p, c->viewport()->mapToGlobal(p), btn, held, mods);
        QApplication::sendEvent(c->viewport(), &e);
    };
    send(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 8; ++i) send(QEvent::MouseMove, a + (b - a) * i / 8, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
}

void rightClick(Canvas *c, QPointF scenePos) {
    const QPoint vp = c->mapFromScene(scenePos);
    QContextMenuEvent e(QContextMenuEvent::Mouse, vp, c->viewport()->mapToGlobal(vp));
    QApplication::sendEvent(c->viewport(), &e);
}

void pause(int ms) { QElapsedTimer t; t.start(); while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(5); } }

template <class T> T *findIn(QWidget *w, const QString &text) {
    for (T *b : w->findChildren<T *>())
        if (b->text() == text) return b;
    return nullptr;
}

void drag(Canvas *c, QPointF a, QPointF b, int steps = 10) {
    mouse(c, QEvent::MouseButtonPress, a);
    for (int i = 1; i <= steps; ++i) {
        const QPointF p = a + (b - a) * (double(i) / steps);
        const QPointF vp = c->mapFromScene(p);
        QMouseEvent e(QEvent::MouseMove, vp, c->viewport()->mapToGlobal(vp), Qt::NoButton, Qt::LeftButton, {});
        QApplication::sendEvent(c->viewport(), &e);
    }
    mouse(c, QEvent::MouseButtonRelease, b);
}

template <class T> int countIn(QGraphicsItem *layer) {
    int n = 0;
    for (QGraphicsItem *c : layer->childItems()) n += qgraphicsitem_cast<T *>(c) != nullptr;
    return n;
}

// Pixel of the rendered viewport at a map's local point vs. the colour the source image has there.
bool pixelMatches(Canvas *c, MapItem *m, QPointF local, QColor expected, int tol, QString *got) {
    const QImage shot = c->viewport()->grab().toImage();
    const QPoint vp = c->mapFromScene(m->mapToScene(local));
    const QColor g = shot.pixelColor(vp);
    *got = QString("%1,%2,%3 expected %4,%5,%6").arg(g.red()).arg(g.green()).arg(g.blue())
               .arg(expected.red()).arg(expected.green()).arg(expected.blue());
    return std::abs(g.red() - expected.red()) <= tol && std::abs(g.green() - expected.green()) <= tol
           && std::abs(g.blue() - expected.blue()) <= tol;
}

// Paints the window once per tab of the open popups: buttons and labels take their text when painted.
void grabEveryTab(MainWindow &w) {
    w.grab();
    for (Panel *p : {w.menu, w.props})
        if (p->isShown())
            for (Tabs *t : p->findChildren<Tabs *>()) {
                const int was = t->current();
                for (int i = 0; i < t->count(); ++i) { t->setCurrent(i); w.grab(); }
                t->setCurrent(was);
            }
}

void wheel(Canvas *c, QPoint vp, int delta) {
    QWheelEvent e(QPointF(vp), QPointF(c->viewport()->mapToGlobal(vp)), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                  Qt::NoScrollPhase, false);
    QApplication::sendEvent(c->viewport(), &e);
}

// Bugs fixed in 0.4.1, each with the steps that showed it.
void regressions(MainWindow &w, const QString &dir, const QString &image) {
    Canvas *c = w.canvas;
    c->setTool(Canvas::Measure);
    drag(c, c->mapToScene(QPoint(200, 200)), c->mapToScene(QPoint(400, 300)), 4);
    w.newProject();
    check(c->rulerCount() == 0, "New project clears the rulers");
    w.activateWindow();
    c->setFocus();

    c->activeLayer = w.layers[1];
    c->scale(3, 3);
    c->setTool(Canvas::ZoneRect);
    click(c, QPointF(10, 10));
    c->setTool(Canvas::Link);
    click(c, QPointF(40, 10));
    check(w.layers[1]->childItems().isEmpty() && c->linksLayer->childItems().isEmpty(), "a click without a drag makes no zone or link, also at zoom 300%");
    c->resetTransform();
    c->centerOn(0, 0);

    drag(c, QPointF(0, 0), QPointF(60, 60), 4);   // one end of a link; the project is saved before its pair exists
    const QString file = dir + "/regressions.vaxmap";
    w.saveTo(file);
    w.openProject(file);
    c->setTool(Canvas::Link);
    drag(c, QPointF(200, 0), QPointF(260, 60), 4);
    QList<LinkItem *> links;
    for (QGraphicsItem *it : c->linksLayer->childItems())
        if (it->type() == LinkItem::Type) links << static_cast<LinkItem *>(it);
    check(links.size() == 2 && links[0]->partner == links[1], "a link saved without its pair gets it after the project is reopened");
    if (links.size() == 2) {
        links[0]->setSelected(true);
        c->activeLayer = w.layers[1];
        w.moveSelectionToActiveLayer();
        check(links[0]->parentItem() == c->linksLayer, "Ctrl+M leaves a selected link in the Links layer");
        const QPointF before = c->mapToScene(c->viewport()->rect().center());
        links[0]->setPoints({QPointF(0, 0), QPointF(0, 0)});
        c->travel(links[0], QPointF(0, 0));
        waitFor([&] { return !c->cameraMoving(); }, 3000);
        check(QLineF(c->mapToScene(c->viewport()->rect().center()) - before, QPointF(230, 30)).length() < 2,
              "a link without area (older projects have them) still jumps to its pair");
    }
    c->showLinks(false);
    c->setTool(Canvas::Select);
    c->resetTransform();
    c->centerOn(0, 0);

    auto *poly = new ZoneItem(ZoneItem::Poly, QPolygonF({QPointF(-300, -300), QPointF(-100, -300), QPointF(-100, -100), QPointF(-300, -100)}), Qt::red);
    poly->setParentItem(w.layers[1]);
    w.scene->clearSelection();
    poly->setSelected(true);
    pause(250);   // handles fade in
    mouse(c, QEvent::MouseButtonPress, QPointF(-100, -300), Qt::RightButton);
    mouse(c, QEvent::MouseMove, QPointF(-97, -298), Qt::RightButton);
    mouse(c, QEvent::MouseButtonRelease, QPointF(-97, -298), Qt::RightButton);
    check(poly->pts == QPolygonF({QPointF(-300, -300), QPointF(-100, -100), QPointF(-300, -100)}),
          "right click removes the vertex under it, also when the mouse moves a little");
    auto doubleClick = [&](QPointF p) {
        mouse(c, QEvent::MouseMove, p, Qt::NoButton);   // a real pointer moves there first: the scene looks up the item under it again
        click(c, p);
        mouse(c, QEvent::MouseButtonDblClick, p);
        mouse(c, QEvent::MouseButtonRelease, p);
    };
    doubleClick(QPointF(-233, -167));
    const int inside = int(poly->pts.size());
    doubleClick(QPointF(-299, -200));
    check(inside == 3 && poly->pts.size() == 4, "double click adds a vertex on an edge, not in the middle of the zone");
    mouse(c, QEvent::MouseButtonPress, QPointF(500, 300), Qt::RightButton);
    mouse(c, QEvent::MouseButtonRelease, QPointF(500, 300), Qt::RightButton);
    check(poly->isSelected(), "right click on empty canvas keeps the selection for the menu's commands");

    key(c, Qt::Key_Space, " ");
    { QFocusEvent away(QEvent::FocusOut, Qt::ActiveWindowFocusReason); QApplication::sendEvent(c, &away); }   // Alt+Tab: the release goes elsewhere
    { QFocusEvent back(QEvent::FocusIn, Qt::ActiveWindowFocusReason); QApplication::sendEvent(c, &back); }
    c->activeLayer = w.layers[2];
    c->setTool(Canvas::Pen);
    drag(c, QPointF(300, 300), QPointF(400, 300), 4);
    check(countIn<StrokeItem>(w.layers[2]) == 1, "Space held while the window loses the focus does not leave pan mode on");
    { QKeyEvent up(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, " "); QApplication::sendEvent(c, &up); }
    c->setTool(Canvas::Select);

    auto *far = new ZoneItem(ZoneItem::Rect, {QPointF(3e6, 0), QPointF(3e6 + 100, 100)}, Qt::red);
    far->setParentItem(w.layers[1]);
    w.fitAll();
    const double fitted = c->zoom();
    wheel(c, QPoint(300, 200), 240);
    check(c->zoom() > fitted, "the wheel zooms in after Fit all on content wider than the zoom range");
    delete far;
    c->resetTransform();
    c->centerOn(0, 0);
    const QPointF under = c->mapToScene(QPoint(300, 200));
    wheel(c, QPoint(300, 200), 240);
    check(c->zoom() > 1 && QLineF(c->mapFromScene(under), QPoint(300, 200)).length() < 2, "the wheel zooms at the cursor");
    c->resetTransform();

    c->activeLayer = w.layers[0];
    MapItem *m = w.importMap(image, QPointF(0, -3000));
    if (m) {
        m->cal = Calibration::square(70.3712, QPointF(10, 10));
        m->cal.fieldCols = m->cal.fieldRows = 1;
        m->cal.fieldStep = QPointF(100, 100);
        m->cal.field = {QPointF(1, 1)};
        w.scene->clearSelection();
        m->setSelected(true);
        w.openProperties(m, QPoint(60, 60));
        QDoubleSpinBox *cell = nullptr;
        for (auto *sp : w.props->findChildren<QDoubleSpinBox *>())
            if (sp->suffix() == " px") cell = sp;
        if (cell) emit cell->editingFinished();   // what Qt sends when the field just loses the focus
        const bool kept = cell && m->cal.field.size() == 1 && m->cal.cellSize() == 70.3712;
        if (cell) {
            cell->setValue(80);
            emit cell->editingFinished();
        }
        check(kept && std::abs(m->cal.cellSize() - 80) < 1e-9 && m->cal.field.isEmpty(), "the cell size field changes the grid only when its value changes");
        key(c, Qt::Key_Escape);
        auto *over = new LinkItem(QRectF(m->pos() + QPointF(100, 100), QSizeF(100, 100)), Qt::red);
        over->setParentItem(c->linksLayer);
        c->showLinks(false);
        c->centerOn(m->sceneBoundingRect().center());
        rightClick(c, m->pos() + QPointF(150, 150));
        check(m->isSelected() && w.props->isShown(), "right click on a selected map opens its properties also where a hidden link lies over it");
        key(c, Qt::Key_Escape);
    }

    {   // handled after the drop event returns: the handler may ask to save
        const int before = countIn<MapItem>(w.layers[0]);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(image)});
        QDragEnterEvent enter(QPoint(300, 300), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);   // Qt drops only on the widget that took the drag
        QApplication::sendEvent(c->viewport(), &enter);
        QDropEvent drop(QPointF(300, 300), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(c->viewport(), &drop);
        pause(50);
        check(countIn<MapItem>(w.layers[0]) == before + 1, "an image dropped on the canvas is imported");
    }

    const QString late = dir + "/late.png";
    const bool missing = !MapData::get(late)->error.isEmpty();
    QFile::copy(image, late);
    check(missing && MapData::get(late)->error.isEmpty(), "a map whose file was missing loads once the file is back and the project is reopened");

    QWidget other;   // stands for the colour dialog
    w.openMenu(QPoint(80, 80));
    { QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(&other, &esc); }
    check(w.menu->isShown(), "Esc typed in another window leaves the popup open");
    key(c, Qt::Key_Escape);

    key(c, Qt::Key_L, "l");
    pause(250);
    auto *tree = w.layersPanel->findChild<QTreeWidget *>();
    tree->topLevelItem(0)->setCheckState(0, Qt::Checked);   // the Links row
    pause(50);
    check(linksVisible && tree->topLevelItemCount() == w.layers.size() && tree->topLevelItem(0)->checkState(0) == Qt::Checked,
          "the Show box of the Links layer shows the links");
    tree->editItem(tree->topLevelItem(1), 2);
    pause(50);
    if (auto *editor = tree->findChild<QLineEdit *>()) {
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(editor, &enter);
    }
    pause(50);
    check(QApplication::focusWidget() == c, "after a layer is renamed the keys go to the canvas again");
    key(c, Qt::Key_L, "l");
    c->showLinks(false);

    auto *t = new TextItem(false, Qt::white);
    t->setParentItem(w.layers[3]);
    t->setPlainText("Door");
    t->startEditing();
    QTextCursor tc = t->textCursor();
    tc.select(QTextCursor::Document);
    t->setTextCursor(tc);
    t->clearFocus();
    check(!t->editing() && !t->textCursor().hasSelection(), "ending a text edit clears the text selected in it");
    c->activeLayer = w.layers[3];
    c->setTool(Canvas::Text);
    click(c, c->mapToScene(QPoint(700, 700)));   // a new text, nothing typed, then Ctrl+S
    w.saveTo(file);
    w.openProject(file);
    check(countIn<TextItem>(w.layers[3]) == 1, QString("an empty text is not written to the project (%1 text item kept)").arg(countIn<TextItem>(w.layers[3])));
}

// What 0.5 added.
void additions(MainWindow &w, const QString &dir, const QString &image) {
    Canvas *c = w.canvas;
    auto firstMap = [&]() -> MapItem * {
        for (QGraphicsItem *it : w.layers[0]->childItems())
            if (auto *m = qgraphicsitem_cast<MapItem *>(it)) return m;
        return nullptr;
    };

    // undo and redo
    w.newProject();
    c->activeLayer = w.layers[1];
    c->setTool(Canvas::ZoneRect);
    drag(c, QPointF(0, 0), QPointF(200, 100));
    w.undo();
    const int afterUndo = countIn<ZoneItem>(w.layers[1]);
    w.redo();
    check(afterUndo == 0 && countIn<ZoneItem>(w.layers[1]) == 1, "undo removes the zone just drawn, redo brings it back");
    c->activeLayer = w.layers[0];
    MapItem *m = w.importMap(image, QPointF(0, -3000));
    m->cal = Calibration::hex(Calibration::HexFlat, 64, QPointF(3, 4));
    m->cal.unit = "km";
    sceneEdited();
    pause(500);   // edits less than 0.4 s apart are one undo step
    m->setSelected(true);
    c->deleteSelected();
    const int afterDelete = countIn<MapItem>(w.layers[0]);
    w.undo();
    m = firstMap();
    check(afterDelete == 0 && m && m->cal.grid == Calibration::HexFlat && m->cal.unit == "km" && countIn<ZoneItem>(w.layers[1]) == 1,
          "undo restores a deleted map with its calibration");
    c->activeLayer = w.layers[2];
    c->setTool(Canvas::Pen);
    drag(c, QPointF(300, 300), QPointF(400, 300), 4);
    w.undo();
    check(c->tool() == Canvas::Pen && countIn<StrokeItem>(w.layers[2]) == 0 && c->activeLayer == w.layers[2], "undo keeps the tool and the active layer");
    drag(c, QPointF(300, 300), QPointF(400, 300), 4);
    pause(500);
    auto *drawn = qgraphicsitem_cast<StrokeItem *>(w.layers[2]->childItems().value(0));
    if (drawn) {
        drawn->pts.last() += QPointF(7, 9);   // changed without setPoints: the points kept for undo must not be used
        sceneEdited();
        pause(500);
    }
    const QVector<QPointF> changed = drawn ? drawn->pts : QVector<QPointF>();
    c->activeLayer = w.layers[1];
    c->setTool(Canvas::ZoneRect);
    drag(c, QPointF(600, 0), QPointF(700, 100));
    w.undo();
    drawn = qgraphicsitem_cast<StrokeItem *>(w.layers[2]->childItems().value(0));
    check(drawn && !changed.isEmpty() && drawn->pts == changed, "undo brings back a stroke's points as they were, also when they changed without setPoints");
    {
        auto *link = new LinkItem(QRectF(0, 500, 50, 50), Qt::red), *pair = new LinkItem(QRectF(900, 500, 50, 50), Qt::red);
        for (LinkItem *l : {link, pair}) l->setParentItem(c->linksLayer);
        link->partner = pair;
        pair->partner = link;
        auto *note = new TextItem(true, Qt::yellow);
        note->setParentItem(w.layers[3]);
        note->setPlainText("Trap \"x\"");
        auto *empty = new StrokeItem(Qt::red, 2);   // no points at all
        empty->setParentItem(w.layers[2]);
        const QJsonObject whole = layersToJson(w.layers, QDir::root());
        check(layersFromParts(layersToParts(w.layers)) == whole && layersFromParts(layersToParts(w.layers)) == whole,
              "an undo step holds the same project as a save, with every kind of item");
        const QList<QByteArray> step = layersToParts(w.layers);
        QGraphicsScene other;
        const QList<LayerItem *> rebuilt = layersFromJson(layersFromParts(step), &other, QDir::root());
        reuseParts(rebuilt, step);
        const QList<QByteArray> again = layersToParts(rebuilt);
        bool shared = again == step;
        for (int i = 0; i < again.size(); ++i)
            if (again[i].startsWith('P')) shared = shared && again[i].constData() == step[i].constData();
        check(shared, "after an undo the strokes reuse the pieces of the step they came from");
        w.undo();
    }

    // recovery copy
    QFile::remove(MainWindow::recoveryFile());
    const bool nothingLeft = !w.offerRecovery();   // also takes the recovery lock, as at every start
    c->activeLayer = w.layers[1];
    c->setTool(Canvas::ZoneRect);
    drag(c, QPointF(300, 0), QPointF(500, 100));   // unsaved: a second zone
    w.autosave();
    QLockFile otherWindow(MainWindow::recoveryFile() + ".lock");
    check(nothingLeft && w.dirty && QFile::exists(MainWindow::recoveryFile()) && !otherWindow.tryLock(),
          "unsaved work is copied to the recovery file, which a second window of the program cannot take");
    w.openProject(MainWindow::recoveryFile());   // what Restore does at the next start
    check(w.dirty && countIn<ZoneItem>(w.layers[1]) == 2 && countIn<MapItem>(w.layers[0]) == 1 && QFile::exists(MainWindow::recoveryFile())
              && !w.recentFiles().contains(MainWindow::recoveryFile()),
          "the recovery file brings the unsaved work back, still marked as unsaved");
    check(w.saveTo(dir + "/recovered.vaxmap") && !QFile::exists(MainWindow::recoveryFile()), "saving removes the recovery file");

    // an image that moved
    QDir().mkpath(dir + "/moved");
    QFile::copy(image, dir + "/moved/gone.png");
    w.newProject();
    c->activeLayer = w.layers[0];
    m = w.importMap(dir + "/moved/gone.png", QPointF(0, 0));
    m->cal = Calibration::square(64, QPointF(0, 0));
    waitFor([&] { return m->src->ready; }, 60000);
    const QString lost = dir + "/lost.vaxmap";
    w.saveTo(lost);
    QByteArray json;
    {
        QFile f(lost);
        f.open(QIODevice::ReadOnly);
        json = f.readAll().replace("moved/gone.png", "gone.png");   // the project as its author left it, before the folder was renamed
    }
    {
        QFile f(lost);
        f.open(QIODevice::WriteOnly);
        f.write(json);
    }
    w.openProject(lost);
    m = firstMap();
    const bool wasMissing = m && !m->src->error.isEmpty();
    const int located = m ? w.locateImage(m, dir + "/moved/gone.png") : 0;
    check(wasMissing && located == 1 && m->src->error.isEmpty() && m->cal.valid() && w.dirty, "Locate image points a map at its moved file and keeps the calibration");

    // tile cache
    QDir cache(MapData::cacheRoot());
    cache.mkpath("0123456789abcdef0123");
    {
        QFile f(cache.filePath("0123456789abcdef0123/0_0_0.png"));
        f.open(QIODevice::WriteOnly);
        f.write(QByteArray(3 << 20, 'x'));
    }
    qint64 freed = -1;
    w.clearCache([&](qint64 bytes) { freed = bytes; });
    waitFor([&] { return freed >= 0; }, 30000);
    check(freed >= (3 << 20) && !cache.exists("0123456789abcdef0123") && !cache.exists("0123456789abcdef0123.del") && m && QDir(m->src->dir).exists(),
          QString("Clear tile cache deletes the cache of maps not opened since the program started, and only that (%1 MB freed)").arg(freed / 1048576));

    // text size, copy and paste, arrows, find, export
    w.newProject();
    auto *z = new ZoneItem(ZoneItem::Rect, {QPointF(0, 0), QPointF(200, 100)}, Qt::red);
    z->setParentItem(w.layers[1]);
    z->name = "Throne room";
    auto *well = new ZoneItem(ZoneItem::Ellipse, {QPointF(3000, 2000), QPointF(3100, 2100)}, Qt::blue);
    well->setParentItem(w.layers[1]);
    well->name = "Well";
    auto *t = new TextItem(false, Qt::white);
    t->setParentItem(w.layers[3]);
    t->setPlainText("Door");
    t->setPos(40, 300);
    t->setSelected(true);
    w.openProperties(t, QPoint(60, 60));
    auto *size = w.props->findChild<QSlider *>();
    if (size) size->setValue(20);
    check(size && std::abs(t->scale() - 4) < 1e-9, "the Size slider in a text's properties sets its size");
    const int popupHeight = w.props->height();
    w.props->dismiss();   // a second right click: closed and opened again at once, while it still fades out
    w.openProperties(t, QPoint(60, 60));
    check(w.props->height() == popupHeight, "the properties popup opened again at once is not empty");
    key(c, Qt::Key_Escape);

    w.scene->clearSelection();
    z->setSelected(true);
    t->setSelected(true);
    c->activeLayer = w.layers[1];
    w.copySelection();
    w.paste();
    const QList<QGraphicsItem *> pasted = w.scene->selectedItems();
    check(pasted.size() == 2 && !pasted.contains(z) && !pasted.contains(t) && w.findNamed("throne").size() == 2 && countIn<TextItem>(w.layers[1]) == 1,
          "Ctrl+C and Ctrl+V put copies of the selection in the active layer and select them");
    w.duplicate();
    check(w.findNamed("throne").size() == 3 && countIn<TextItem>(w.layers[1]) == 2 && w.scene->selectedItems().size() == 2, "Ctrl+D duplicates the selection");

    w.scene->clearSelection();
    z->setSelected(true);
    const QPointF before = z->pos();
    key(c, Qt::Key_Right);
    key(c, Qt::Key_Down, {}, Qt::ShiftModifier);
    const bool moved = z->pos() == before + QPointF(1, 10) / c->zoom();
    w.scene->clearSelection();
    const QPointF view = c->mapToScene(c->viewport()->rect().center());
    key(c, Qt::Key_Right);
    check(moved && c->mapToScene(c->viewport()->rect().center()) != view, "arrows move the selection 1 px (Shift: 10 px); with nothing selected they scroll the view");

    const QList<QGraphicsItem *> found = w.findNamed("  WELL ");
    if (found.size() == 1) w.goTo(found[0]);
    waitFor([&] { return !c->cameraMoving(); }, 3000);
    check(found.size() == 1 && found[0] == well && well->isSelected()
              && QLineF(c->mapToScene(c->viewport()->rect().center()), well->sceneBoundingRect().center()).length() < 2,
          "Find goes to the zone whose name contains the text and selects it");

    const QString png = dir + "/view.png";
    const bool exported = w.exportView(png);
    const QImage shot(png);
    check(exported && shot.width() >= c->viewport()->width() && w.scene->selectedItems().isEmpty()
              && shot.pixelColor(shot.width() / 2, shot.height() / 2) != QColor(36, 37, 41),
          "Export view writes the canvas as a picture, without the selection marks");

    // several maps at once
    w.newProject();
    c->activeLayer = w.layers[0];
    w.importMaps({image, image, image, image, image}, QPointF(0, 0));
    QList<QRectF> boxes;
    QSet<int> rows;
    for (QGraphicsItem *it : w.layers[0]->childItems()) {
        boxes << it->sceneBoundingRect();
        rows << int(it->pos().y());
    }
    bool apart = boxes.size() == 5;
    for (int i = 0; i < boxes.size(); ++i)
        for (int j = i + 1; j < boxes.size(); ++j) apart = apart && !boxes[i].intersects(boxes[j]);
    check(apart && rows.size() == 2, "five maps imported together land in rows and columns without overlapping");

    // recent projects
    const QString first = QFileInfo(dir + "/first.vaxmap").absoluteFilePath(), second = QFileInfo(dir + "/second.vaxmap").absoluteFilePath();
    w.saveTo(first);
    w.saveTo(second);
    w.openProject(first);
    check(w.recentFiles().mid(0, 2) == QStringList({first, second}), "recent projects: the last one opened or saved comes first");

    // Portuguese
    w.setLanguage(true);
    w.newProject();
    c->activeLayer = w.layers[0];
    m = w.importMap(image, QPointF(0, 0));
    m->cal = Calibration::square(100, QPointF(0, 0));
    check(w.windowTitle().contains("Sem título") && w.layers[0]->name == "Mapas" && T("Save") == "Salvar"
              && measureText(w.scene, m->mapToScene(QPointF(50, 50)), m->mapToScene(QPointF(350, 50))) == "3 quadrados · 4,5 m"
              && QCoreApplication::translate("QPlatformTheme", "Cancel") == "Cancelar"
              && QCoreApplication::translate("QColorDialog", "&Basic colors") == "Cores &básicas",
          "Portuguese: window title, layer names, ruler with decimal comma, Qt's own dialog texts");
    // every tool hint, panel and properties popup is shown once; none may ask for a text the table lacks
    for (int tool = Canvas::Select; tool <= Canvas::Link; ++tool) c->setTool(Canvas::Tool(tool));
    c->setTool(Canvas::Select);
    auto *zone = new ZoneItem(ZoneItem::Poly, QPolygonF({QPointF(2000, 0), QPointF(2200, 0), QPointF(2100, 200)}), Qt::red);
    zone->setParentItem(w.layers[1]);
    auto *link = new LinkItem(QRectF(2300, 0, 100, 100), Qt::red);
    link->setParentItem(c->linksLayer);
    c->showLinks(true);
    auto *note = new TextItem(true, QColor(246, 229, 141));
    note->setParentItem(w.layers[3]);
    note->setPlainText("Armadilha");
    auto *stroke = new StrokeItem(Qt::red, 4);
    stroke->setParentItem(w.layers[2]);
    stroke->setPoints({QPointF(2500, 0), QPointF(2600, 50)});
    auto *lostMap = new MapItem(dir + "/nowhere.png");
    lostMap->setParentItem(w.layers[0]);
    lostMap->setPos(2000, 400);
    w.fitAll();
    w.openMenu(QPoint(80, 80));
    grabEveryTab(w);   // buttons take their text when painted
    key(c, Qt::Key_Escape);
    key(c, Qt::Key_L, "l");
    pause(250);
    w.grab();
    key(c, Qt::Key_L, "l");
    for (QGraphicsItem *it : QList<QGraphicsItem *>{zone, link, note, stroke, lostMap, m}) {
        w.scene->clearSelection();
        it->setSelected(true);
        w.openProperties(it, QPoint(60, 60));
        grabEveryTab(w);
    }
    zone->setSelected(true);   // two items selected
    w.openProperties(m, QPoint(60, 60));
    w.grab();
    w.scene->clearSelection();
    m->setSelected(true);
    c->setTool(Canvas::Calibrate);
    click(c, m->mapToScene(QPointF(100, 100)));
    click(c, m->mapToScene(QPointF(200, 100)));   // a measurement the map's lines do not confirm
    waitFor([&] { return !c->calibrator->busy(); }, 30000);
    w.grab();
    key(c, Qt::Key_F, "f");                       // detection
    waitFor([&] { return !c->calibrator->busy(); }, 60000);
    w.grab();
    key(c, Qt::Key_Return);                       // applied
    w.undo();
    w.redo();
    w.grab();
    check(untranslated().isEmpty(), "Portuguese: every text the interface asks for is in the table" + (untranslated().isEmpty() ? QString() : ": " + untranslated().join(" | ")));
    w.setLanguage(false);
    check(w.windowTitle().contains("Untitled") && T("Save") == "Save" && QCoreApplication::translate("QPlatformTheme", "Cancel") == "Cancel",
          "back to English");
}

// What 0.6 added.
void version06(MainWindow &w, const QString &dir, const QString &map1, const QString &map2) {
    Canvas *c = w.canvas;
    auto maps = [&] {
        QList<MapItem *> r;
        for (QGraphicsItem *it : w.layers[0]->childItems())
            if (auto *m = qgraphicsitem_cast<MapItem *>(it)) r << m;
        return r;
    };
    auto altDrag = [&](QPointF a, QPointF b) {
        mouse(c, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::AltModifier);
        for (int i = 1; i <= 8; ++i) {
            const QPointF vp = c->mapFromScene(a + (b - a) * (i / 8.0));
            QMouseEvent e(QEvent::MouseMove, vp, c->viewport()->mapToGlobal(vp), Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
            QApplication::sendEvent(c->viewport(), &e);
        }
        mouse(c, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::AltModifier);
    };

    // portable copy
    QDir().mkpath(dir + "/portable/VaxMaps-data");
    check(portableDataDir(dir + "/portable") == dir + "/portable/VaxMaps-data" && portableDataDir(dir).isEmpty(),
          "the portable copy is recognised by the VaxMaps-data folder beside the program");

    // diagonal rules
    Calibration sq = Calibration::square(100, QPointF(0, 0));
    const double one = sq.steps(QPoint(0, 0), QPoint(3, 3));
    sq.diagonal = Calibration::Alternating;
    const double alternating = sq.steps(QPoint(0, 0), QPoint(3, 3)), alternating2 = sq.steps(QPoint(0, 0), QPoint(2, 2));
    sq.diagonal = Calibration::Straight;
    const double straight = sq.steps(QPoint(0, 0), QPoint(3, 4));
    check(one == 3 && alternating == 4 && alternating2 == 3 && straight == 5, "diagonal rules: 3 diagonal steps count 3, 4 (1-2-1) or the straight line");

    w.newProject();
    c->activeLayer = w.layers[0];
    MapItem *m = w.importMap(map1, QPointF(1500, 1000));   // top-left at 0,0
    waitFor([&] { return m->src->ready; }, 60000);
    m->cal = Calibration::square(100, QPointF(50, 50));
    m->cal.diagonal = Calibration::Alternating;
    const QString ruler = measureText(w.scene, QPointF(100, 100), QPointF(400, 400));
    check(ruler == "4 squares · 6 m", "the ruler follows the map's diagonal rule: " + ruler);

    // tokens
    c->activeLayer = w.layers[1];
    c->resetTransform();
    c->centerOn(500, 500);
    c->setTool(Canvas::Token);
    click(c, QPointF(530, 470));
    QList<TokenItem *> tokens;
    for (QGraphicsItem *it : w.layers[1]->childItems())
        if (auto *k = qgraphicsitem_cast<TokenItem *>(it)) tokens << k;
    TokenItem *k = tokens.value(0);
    check(tokens.size() == 1 && k->name == "1" && QLineF(k->centre(), QPointF(500, 500)).length() < 0.01 && std::abs(k->diameter - 90) < 0.01,
          "a token lands on the centre of the square clicked, 90% of a square wide");
    if (!k) return;
    c->setTool(Canvas::Select);
    drag(c, QPointF(500, 500), QPointF(737, 822));
    check(QLineF(k->centre(), QPointF(700, 800)).length() < 0.01, "a dragged token lands on the centre of a square");
    k->cells = 2;
    k->moveBy(-20, -20);   // nearer the top-left corner of its square
    snapToken(k);
    check(QLineF(k->centre(), QPointF(650, 750)).length() < 0.01 && std::abs(k->diameter - 180) < 0.01, "a 2x2 token sits on a corner and is 2 squares wide");
    k->name = "Goblin chief";
    c->setTool(Canvas::Measure);
    drag(c, QPointF(660, 760), QPointF(1050, 750));   // started on the token, off its centre
    check(c->rulerText(0) == "4 squares · 6 m", "a ruler started on a token measures from its centre: " + c->rulerText(0));
    c->setTool(Canvas::Select);

    // find in text
    auto *z = new ZoneItem(ZoneItem::Rect, {QPointF(1200, 100), QPointF(1400, 300)}, Qt::red);
    z->setParentItem(w.layers[1]);
    z->setDesc("A chest with 40 gold pieces");
    auto *note = new TextItem(true, Qt::yellow);
    note->setParentItem(w.layers[3]);
    note->setPlainText("Trap: DC 15\nPoison needle");
    check(w.findNamed("gold").value(0) == z && w.findNamed("needle").value(0) == note && w.findNamed("chief").value(0) == k,
          "Find looks in zone descriptions, note texts and token names");

    // fog of war
    m->enableFog(true);
    const QImage fog0 = m->fog;
    c->resetTransform();
    c->centerOn(1500, 1000);
    c->setTool(Canvas::Fog);
    drag(c, QPointF(1000, 1000), QPointF(2000, 1000));   // 400 px brush at 100% zoom: penWidth 4 x 10 / zoom... (40 px)
    auto fogAt = [&](MapItem *mm, QPointF p) {
        const QImage &f = mm->fog;
        return qAlpha(f.pixel(int(p.x() * f.width() / mm->boundingRect().width()), int(p.y() * f.height() / mm->boundingRect().height())));
    };
    check(fog0.width() == 2048 && fogAt(m, QPointF(1500, 1000)) == 0 && fogAt(m, QPointF(1500, 1100)) == 255 && fogAt(m, QPointF(500, 1000)) == 255,
          "the fog brush reveals where it passes and nothing else");
    altDrag(QPointF(1400, 1000), QPointF(1600, 1000));
    check(fogAt(m, QPointF(1500, 1000)) == 255 && fogAt(m, QPointF(1800, 1000)) == 0, "Alt + drag covers again");
    pause(500);
    m->update();
    fogOpaque = true;
    const QString fogFile = dir + "/fog.vaxmap";
    w.saveTo(fogFile);
    fogOpaque = false;
    w.openProject(fogFile);
    m = maps().value(0);
    check(m && fogOpaque && fogAt(m, QPointF(1800, 1000)) == 0 && fogAt(m, QPointF(1500, 1000)) == 255,
          "the fog and the opaque setting are saved with the project");
    if (!m) return;
    c->setTool(Canvas::Fog);
    drag(c, QPointF(1000, 500), QPointF(2000, 500));
    pause(500);
    w.undo();
    m = maps().value(0);
    check(m && fogAt(m, QPointF(1500, 500)) == 255, "undo covers again what the fog brush revealed");
    c->setTool(Canvas::Select);
    MapItem *m2 = w.importMap(map2, QPointF(-2000, 0));
    waitFor([&] { return m2->src->ready; }, 60000);
    c->centerOn(m2->sceneBoundingRect().center());
    c->setTool(Canvas::Fog);
    altDrag(m2->sceneBoundingRect().center() - QPointF(100, 0), m2->sceneBoundingRect().center() + QPointF(100, 0));
    check(!m2->fog.isNull() && fogAt(m2, m2->boundingRect().center()) == 255 && fogAt(m2, QPointF(10, 10)) == 0,
          "Alt + drag on a map without fog covers only where it passes");
    c->setTool(Canvas::Select);
    fogOpaque = false;

    // cell size typed in
    w.scene->clearSelection();
    m2->setSelected(true);
    c->setTool(Canvas::Calibrate);
    c->calibrator->useCell(64);
    key(c, Qt::Key_Right);
    key(c, Qt::Key_Return);
    check(std::abs(m2->cal.cellSize() - 64) < 1e-9 && m2->cal.origin == QPointF(1, 0), "a typed cell size makes the grid; the arrows move it before it is applied");

    // the fit to the map's lines runs on a worker thread
    m2->cal = Calibration();
    w.scene->clearSelection();
    m2->setSelected(true);
    c->setTool(Canvas::Calibrate);
    waitFor([&] { return !c->calibrator->busy(); }, 30000);   // the map's lines are read
    QElapsedTimer t;
    t.start();
    click(c, m2->mapToScene(QPointF(64 * 3, 64 * 3)));
    click(c, m2->mapToScene(QPointF(64 * 4, 64 * 3)));
    const qint64 clickMs = t.elapsed();
    const bool wasBusy = c->calibrator->busy();
    waitFor([&] { return !c->calibrator->busy(); }, 30000);
    check(wasBusy && clickMs < 100 && c->calibrator->confirmed(), QString("a measurement returns at once (%1 ms) and the fit to the lines follows").arg(clickMs));
    c->setTool(Canvas::Select);

    // full size export: the map's own pixels, also where the tiles were not loaded yet
    m->setFog(QImage());   // the export shows the fog as the canvas does
    c->clearRulers();
    const QString big = dir + "/full.png";
    const QSize size = w.exportFull(big, m->sceneBoundingRect());
    const QImage out(big), src(map1);
    bool same = size == QSize(3000, 2000) && out.size() == size;
    for (QPoint p : {QPoint(5, 1990), QPoint(1234, 777), QPoint(2990, 1990), QPoint(2500, 100)}) {
        const QColor a = out.pixelColor(p), b = src.pixelColor(p);
        same = same && std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) < 12;
    }
    QString diff;
    for (QPoint p : {QPoint(5, 1990), QPoint(1234, 777), QPoint(2990, 1990), QPoint(2500, 100)})
        diff += QString(" %1,%2: %3/%4").arg(p.x()).arg(p.y()).arg(out.pixelColor(p).name(), src.pixelColor(p).name());
    check(same, QString("Export full size writes the map at its own resolution, with its pixels (%1x%2%3)").arg(size.width()).arg(size.height()).arg(diff));

    // package
    const QString zip = dir + "/campaign.zip";
    w.saveTo(dir + "/campaign.vaxmap");
    check(w.savePackage(zip), "Save package writes a .zip");
    QDir(dir + "/campaign").removeRecursively();
    w.newProject();
    const bool opened = w.openPackage(zip);
    QStringList found;
    for (MapItem *x : maps()) found << (x->src->error.isEmpty() && x->src->path.startsWith(QFileInfo(dir + "/campaign").absoluteFilePath()) ? "ok" : x->src->path);
    bool fogKept = false;
    for (MapItem *x : maps()) fogKept = fogKept || !x->fog.isNull();
    check(opened && found == QStringList({"ok", "ok"}) && fogKept,
          "a package opens from a new folder beside it, with its images and fog: " + found.join(", "));
    {   // a package whose names point outside its folder
        QFile f(dir + "/evil.zip");
        f.open(QIODevice::WriteOnly);
        QZipWriter zw(&f);
        zw.addFile("evil.vaxmap", QByteArray(R"({"format":"vaxmaps","version":3,"layers":[]})"));
        zw.addFile("../escaped.txt", QByteArray("x"));
        zw.addFile("images/../../escaped2.txt", QByteArray("x"));
        zw.close();
    }
    w.openPackage(dir + "/evil.zip");
    check(!QFile::exists(dir + "/escaped.txt") && !QFile::exists(QFileInfo(dir).absolutePath() + "/escaped2.txt") && !QFile::exists(dir + "/escaped2.txt"),
          "a package cannot write outside its own folder");

    // session
    c->color = QColor(10, 200, 30);
    c->setTool(Canvas::Pen);
    w.saveSession();
    {
        MainWindow other;
        other.restoreSession();
        check(other.canvas->color == QColor(10, 200, 30) && other.canvas->tool() == Canvas::Pen && other.canvas->penWidth == c->penWidth,
              "the next window starts with the tool, colour and width of the last one");
        other.dirty = false;
    }
    c->setTool(Canvas::Select);

    // minimap and shortcuts
    w.newProject();
    c->activeLayer = w.layers[0];
    w.importMap(map1, QPointF(0, 0));
    pause(1300);
    QWidget *mini = nullptr;
    for (QWidget *x : c->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
        if (x->width() == 220 && x->height() == 150) mini = x;
    if (mini) {
        c->centerOn(5000, 5000);
        const QPoint centre = mini->rect().center();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(centre), mini->mapToGlobal(QPointF(centre)), Qt::LeftButton, Qt::LeftButton, {});
        QApplication::sendEvent(mini, &press);
    }
    check(mini && mini->isVisible() && QLineF(c->mapToScene(c->viewport()->rect().center()), QPointF(0, 0)).length() < 40,
          "the minimap shows the project and a click on it moves the view there");
    key(c, Qt::Key_Escape);
    w.showShortcuts();
    check(w.props->isShown() && w.props->width() == 720, "F1 shows the list of shortcuts");
    key(c, Qt::Key_Escape);

    // every new text in Portuguese
    w.setLanguage(true);
    c->setTool(Canvas::Token);
    c->setTool(Canvas::Fog);
    c->setTool(Canvas::Select);
    w.showShortcuts();
    w.grab();
    m = maps().value(0);
    if (m) {
        m->cal = Calibration::square(100, QPointF(50, 50));
        w.scene->clearSelection();
        m->setSelected(true);
        w.openProperties(m, QPoint(60, 60));
        grabEveryTab(w);
    }
    auto *tok = new TokenItem("Orc", Qt::red, 90, 1);
    tok->setParentItem(w.layers[1]);
    w.scene->clearSelection();
    tok->setSelected(true);
    w.openProperties(tok, QPoint(60, 60));
    grabEveryTab(w);
    w.openMenu(QPoint(80, 80));
    grabEveryTab(w);
    key(c, Qt::Key_Escape);
    w.scene->clearSelection();
    c->setTool(Canvas::Calibrate);
    w.grab();
    c->setTool(Canvas::Select);
    check(untranslated().isEmpty(), "Portuguese: the texts added in 0.6 are in the table" + (untranslated().isEmpty() ? QString() : ": " + untranslated().join(" | ")));
    w.setLanguage(false);
    w.dirty = false;
}

void version07(MainWindow &w, const QString &dir, const QString &map1) {
    say("\n== 0.7 ==");
    Canvas *c = w.canvas;
    w.newProject();
    c->activeLayer = w.layers[0];
    MapItem *m = w.importMap(map1, QPointF(1500, 1000));   // top-left at 0,0, 3000 x 2000
    waitFor([&] { return m->src->ready; }, 60000);
    m->cal = Calibration::square(100, QPointF(50, 50));   // 1 square = 1.5 m
    m->enableFog(true);
    auto fogAt = [&](QPointF p) {
        const QImage &f = m->fog;
        return qAlpha(f.pixel(int(p.x() * f.width() / m->boundingRect().width()), int(p.y() * f.height() / m->boundingRect().height())));
    };
    c->activeLayer = w.layers[1];
    c->resetTransform();
    c->centerOn(600, 600);
    c->setTool(Canvas::Token);
    click(c, QPointF(600, 600));   // lands on the centre of its square, (600, 600)
    TokenItem *k = nullptr;
    for (QGraphicsItem *it : w.layers[1]->childItems()) k = k ? k : qgraphicsitem_cast<TokenItem *>(it);
    if (!k) { check(false, "token placed"); return; }
    check(fogAt(QPointF(600, 600)) == 255, "a token without vision clears no fog");
    k->sight = 4.5;   // 3 squares: 300 px
    revealSight(k);
    check(fogAt(QPointF(880, 600)) == 0 && fogAt(QPointF(600, 330)) == 0 && fogAt(QPointF(940, 600)) == 255 && fogAt(QPointF(830, 830)) == 255,
          "a circle of vision clears the fog within its distance, in the map's units");
    c->setTool(Canvas::Select);
    drag(c, QPointF(600, 600), QPointF(1500, 1300));   // lands on (1500, 1300)
    check(fogAt(QPointF(1500, 1050)) == 0 && fogAt(QPointF(600, 600)) == 0, "a dragged token clears the fog where it lands, the old area stays clear");
    k->cone = true;
    k->coneAngle = 90;
    k->facing = 0;   // right
    k->setPos(k->pos() + QPointF(800, 0));   // (2300, 1300)
    revealSight(k);
    check(fogAt(QPointF(2550, 1300)) == 0 && fogAt(QPointF(2050, 1300)) == 255 && fogAt(QPointF(2300, 1550)) == 255,
          "a cone clears only the side it faces");
    w.scene->clearSelection();
    k->setSelected(true);
    c->setFocus();
    for (int i = 0; i < 6; ++i) key(c, Qt::Key_BracketRight, "]");   // 6 x 15° clockwise: down
    check(std::abs(k->facing - 90) < 1e-9 && fogAt(QPointF(2300, 1550)) == 0 && fogAt(QPointF(2300, 1050)) == 255,
          "] turns the selected cone clockwise and clears where it now faces");
    auto *wall = new StrokeItem(Qt::black, 4);
    wall->setParentItem(w.layers[1]);
    wall->setPoints({QPointF(650, 1500), QPointF(650, 1900)});
    wall->wall = true;
    auto *guard = new TokenItem("G", Qt::blue, 90, 1);
    guard->setParentItem(w.layers[1]);
    guard->setPos(500, 1700);
    guard->sight = 4.5;
    revealSight(guard);
    check(fogAt(QPointF(600, 1700)) == 0 && fogAt(QPointF(500, 1450)) == 0 && fogAt(QPointF(700, 1700)) == 255 && fogAt(QPointF(780, 1450)) == 255,
          "a stroke marked as a wall stops the vision behind it");
    auto *scout = new TokenItem("S", Qt::green, 90, 1);
    scout->setParentItem(w.layers[1]);
    scout->setPos(2100, 300);
    scout->sight = 3;   // 200 px
    c->centerOn(2450, 300);
    drag(c, QPointF(2100, 300), QPointF(2800, 300));
    check(QLineF(scout->centre(), QPointF(2800, 300)).length() < 0.01 && fogAt(QPointF(2450, 300)) == 0 && fogAt(QPointF(2450, 600)) == 255,
          "a dragged token clears the fog along its way");
    pause(500);
    const QString file = dir + "/vision.vaxmap";
    w.saveTo(file);
    w.openProject(file);
    TokenItem *back = nullptr;
    bool wallBack = false;
    for (QGraphicsItem *it : w.layers[1]->childItems()) {
        back = back ? back : qgraphicsitem_cast<TokenItem *>(it);
        if (auto *st = qgraphicsitem_cast<StrokeItem *>(it)) wallBack = st->wall;
    }
    check(wallBack && back && back->sight == 4.5 && back->cone && back->coneAngle == 90 && back->facing == 90 && std::abs(back->sightRadius - 300) < 0.01,
          "vision is saved with the token, walls with their strokes");
    w.newProject();
}

void version062(MainWindow &w, const QString &dir, const QString &image) {
    say("\n== 0.6.2 ==");
    {   // a project from a newer version: what is known loads, Save asks for a new name
        QFile f(dir + "/newer.vaxmap");
        f.open(QIODevice::WriteOnly);
        f.write(R"({"format":"vaxmaps","version":99,"layers":[{"name":"A","items":[
            {"type":"zone","shape":"rect","pts":[0,0,100,100],"name":"Known","x":0,"y":0},
            {"type":"hologram","x":5,"y":5}]}]})");
        f.close();
        QString seen;
        QTimer closer;   // accepts the warning shown inside openProject
        QObject::connect(&closer, &QTimer::timeout, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) { seen = box->text(); box->accept(); }
        });
        closer.start(20);
        w.dirty = false;
        const bool opened = w.openProject(f.fileName());
        closer.stop();
        int zones = 0;
        for (LayerItem *l : w.layers)
            for (QGraphicsItem *it : l->childItems()) zones += it->type() == ZoneItem::Type;
        check(opened && zones == 1 && seen.contains("newer") && !w.windowTitle().contains("newer.vaxmap"),
              QString("a project from a newer version warns, loads what it knows and does not keep its name for Save (%1 %2 '%3' %4)").arg(opened).arg(zones).arg(seen, w.windowTitle()));
        w.newProject();
    }
    {   // the clean-up at start: pyramids unused for a long time go, recent and unfinished ones stay
        const QString root = MapData::cacheRoot();
        auto make = [&](const QString &name, bool meta, int daysAgo) {
            QDir().mkpath(root + "/" + name);
            QFile tile(root + "/" + name + "/0_0_0.png");
            tile.open(QIODevice::WriteOnly);
            tile.write("x");
            if (!meta) return;
            QFile m(root + "/" + name + "/meta.json");
            m.open(QIODevice::WriteOnly);
            m.write("{}");
            m.flush();   // else closing writes it and the time becomes now
            m.setFileTime(QDateTime::currentDateTime().addDays(-daysAgo), QFileDevice::FileModificationTime);
        };
        make("selftest-old", true, 200);
        make("selftest-recent", true, 3);
        make("selftest-building", false, 0);
        qint64 freed = -1;
        MapData::clearCache([&](qint64 b) { freed = b; }, QDateTime::currentDateTime().addDays(-90));
        waitFor([&] { return freed >= 0; }, 5000);
        check(freed > 0 && !QDir(root + "/selftest-old").exists() && !QDir(root + "/selftest-old.del").exists()
                  && QDir(root + "/selftest-recent").exists() && QDir(root + "/selftest-building").exists(),
              QString("the cache drops pyramids unused for 90 days and keeps recent and unfinished ones (%1 %2)").arg(freed).arg(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).join(",")));
        QDir(root + "/selftest-recent").removeRecursively();
        QDir(root + "/selftest-building").removeRecursively();
    }
    {   // command line: an image becomes a map in the view
        auto count = [&] { const auto all = w.scene->items(); return int(std::count_if(all.begin(), all.end(), [](QGraphicsItem *it) { return it->type() == MapItem::Type; })); };
        const int before = count();
        w.openFiles({image});
        const int after = count();
        check(after == before + 1, "an image given on the command line is imported");
        w.dirty = false;
        w.newProject();
    }
}

void bench(MainWindow &w, const QString &dir) {
    say("\n== benchmark: 8 maps of 6000 x 4000 px ==");
    QStringList paths;
    for (int i = 0; i < 8; ++i) paths << makeMap(dir, QString("big%1.png").arg(i), 6000, 4000, 70, QPoint(0, 0), false, 12);
    w.newProject();
    QElapsedTimer t;
    t.start();
    QList<MapItem *> maps;
    for (int i = 0; i < paths.size(); ++i) maps << w.importMap(paths[i], QPointF((i % 4) * 6200, (i / 4) * 4200));
    waitFor([&] { for (auto *m : maps) if (!m->src->ready) return false; return true; }, 600000);
    qint64 bytes = 0;
    for (auto *m : maps)
        for (const QFileInfo &f : QDir(m->src->dir).entryInfoList(QDir::Files)) bytes += f.size();
    say(QString("tile pyramids built: %1 s, %2 MB on disk (first time only; then from the cache)")
            .arg(t.elapsed() / 1000.0, 0, 'f', 1).arg(bytes / 1048576.0, 0, 'f', 0));
    auto frames = [&](const QString &label) {
        settle(w.canvas);
        QElapsedTimer f;
        f.start();
        for (int i = 0; i < 20; ++i) w.canvas->viewport()->repaint();
        say(QString("%1: %2 ms/frame, tile cache %3 MB").arg(label).arg(f.elapsed() / 20.0, 0, 'f', 1)
                .arg(tileCacheBytes() / 1048576.0, 0, 'f', 0));
    };
    w.fitAll();
    frames(QString("every map visible (zoom %1%)").arg(w.canvas->zoom() * 100, 0, 'f', 1));
    w.canvas->resetTransform();
    w.canvas->scale(0.35, 0.35);
    w.canvas->centerOn(6200, 2000);
    frames("zoom 35%");
    w.canvas->resetTransform();
    w.canvas->centerOn(9000, 2000);
    frames("zoom 100%");
    QElapsedTimer d;
    d.start();
    const auto g = detectGrid(loadAnalysis(paths[0]));
    say(QString("grid detection on one map: %1 s (%2)").arg(d.elapsed() / 1000.0, 0, 'f', 2).arg(g ? QString::number(g->cal.cellSize(), 'f', 2) + " px" : "none"));
}

// What 0.8 added.
void version08(MainWindow &w, const QString &dir, const QString &out) {
    say("\n== 0.8 ==");
    Canvas *c = w.canvas;
    w.newProject();
    c->activeLayer = w.layers[0];
    // calibration, step by step, on a map whose top-left quarter is art without squares
    QPainterPath area, art;
    area.addRect(0, 0, 3000, 2000);
    art.addRect(0, 0, 1500, 1000);
    const QString path = dir + "/partial.png";
    partialGridImage(3000, 2000, 100, QPointF(50, 50), area.subtracted(art), false).save(path);
    MapItem *m = w.importMap(path, QPointF(1500, 1000));   // top-left at 0,0
    waitFor([&] { return m->src->ready; }, 60000);
    w.scene->clearSelection();
    m->setSelected(true);
    c->setTool(Canvas::Calibrate);
    Calibrator *cal = c->calibrator;
    waitFor([&] { return !cal->busy(); }, 30000);
    key(c, Qt::Key_S, "s");
    check(cal->step() == 1 && cal->measured() == 0 && !cal->current(), "S skips a quarter without squares");
    click(c, m->mapToScene(QPointF(1752, 352)));   // top-right quarter
    click(c, m->mapToScene(QPointF(1849, 350)));
    waitFor([&] { return !cal->busy(); }, 30000);
    auto r = cal->current();
    check(r && cal->confirmed() && std::abs(r->cellSize() - 100) < 0.05 && offBy(r->origin.x(), 50, 100) < 1 && offBy(r->origin.y(), 50, 100) < 1,
          r ? QString("partial map: the measurement after a skipped quarter is matched to the lines (%1 px)").arg(r->cellSize(), 0, 'f', 3)
            : QString("partial map: no grid after the first measurement"));
    click(c, m->mapToScene(QPointF(260, 1430)));   // bottom-left: a pair 130 px apart, off the corners
    click(c, m->mapToScene(QPointF(390, 1430)));
    waitFor([&] { return !cal->busy(); }, 30000);
    r = cal->current();
    check(r && cal->confirmed() && cal->measured() == 1 && cal->step() == 2 && std::abs(r->cellSize() - 100) < 0.05,
          "a measurement the lines do not confirm is left out and the confirmed grid stays");
    key(c, Qt::Key_S, "s");
    click(c, m->mapToScene(QPointF(2052, 1549)));   // bottom-right
    click(c, m->mapToScene(QPointF(2151, 1552)));
    waitFor([&] { return !cal->busy(); }, 30000);
    r = cal->current();
    check(r && cal->confirmed() && cal->measured() == 2 && cal->step() == 4 && std::abs(r->cellSize() - 100) < 0.05,
          "measured, left out, skipped, measured: 4 steps and the grid matched");
    key(c, Qt::Key_Return);
    check(std::abs(m->cal.cellSize() - 100) < 0.05 && c->tool() == Canvas::Select, "Enter applies it");
    m->setSelected(true);
    c->setTool(Canvas::Calibrate);
    for (int i = 0; i < 5; ++i) key(c, Qt::Key_S, "s");
    check(cal->step() == 4 && !cal->current(), "every quarter skipped: no grid, the panel says what to do");
    key(c, Qt::Key_Escape);
    // squares on the right half only: once the lines confirm the grid, the bottom-left quarter is skipped by itself
    QPainterPath rightHalf;
    rightHalf.addRect(1500, 0, 1500, 2000);
    const QString right = dir + "/right.png";
    partialGridImage(3000, 2000, 100, QPointF(50, 50), rightHalf, false).save(right);
    MapItem *m2 = w.importMap(right, QPointF(1500, 4000));   // top-left at 0,3000
    waitFor([&] { return m2->src->ready; }, 60000);
    w.scene->clearSelection();
    m2->setSelected(true);
    c->setTool(Canvas::Calibrate);
    waitFor([&] { return !cal->busy(); }, 30000);
    key(c, Qt::Key_S, "s");
    click(c, m2->mapToScene(QPointF(1752, 352)));
    click(c, m2->mapToScene(QPointF(1849, 350)));
    waitFor([&] { return !cal->busy(); }, 30000);
    check(cal->confirmed() && cal->step() == 3, QString("the quarter where the lines show no grid is skipped by itself (step %1)").arg(cal->step()));
    key(c, Qt::Key_Escape);

    // the menu and the longer popups in tabs: the popup is as tall as the tab shown
    c->resetTransform();
    c->centerOn(m->sceneBoundingRect().center());
    w.openMenu(QPoint(60, 40));
    pause(300);
    QList<int> heights;
    for (int i = 0; i < w.menuTabs->count(); ++i) {
        w.menuTabs->setCurrent(i);
        heights << w.menu->height();
    }
    w.menuTabs->setCurrent(0);
    pause(100);
    w.grab().save(out + "/vaxmaps-menu-tabs.png");
    check(w.menuTabs->count() == 4 && heights[0] < 560 && *std::max_element(heights.begin() + 1, heights.end()) < heights[0],
          QString("menu in 4 tabs, heights %1 px (0.7: one list of about 800 px)").arg([&] { QStringList l; for (int h : heights) l << QString::number(h); return l.join(", "); }()));
    if (auto *find = findIn<MenuButton>(w.menu, "Find…")) find->click();
    check(!w.menu->isShown() && w.props->isShown(), "a command in another tab works from the menu");
    key(c, Qt::Key_Escape);
    w.menuTabs->setCurrent(2);
    w.openMenu(QPoint(60, 40));
    check(w.menuTabs->current() == 2, "the menu opens on the tab used last");
    key(c, Qt::Key_Escape);
    w.scene->clearSelection();
    m->setSelected(true);
    w.openProperties(m, QPoint(60, 40));
    pause(300);
    auto *tabs = w.props->findChild<Tabs *>();
    const int gridHeight = w.props->height();
    if (tabs) tabs->setCurrent(1);
    const int fogHeight = w.props->height();
    pause(250);   // the tab's highlight glides over
    w.grab().save(out + "/vaxmaps-map-properties.png");
    check(tabs && tabs->count() == 2 && fogHeight < gridHeight, QString("map properties: Grid and Fog of war tabs (%1 and %2 px tall)").arg(gridHeight).arg(fogHeight));
    key(c, Qt::Key_Escape);
    w.openProperties(m, QPoint(60, 40));
    tabs = w.props->findChild<Tabs *>();
    check(tabs && tabs->current() == 1, "the map popup opens on its tab used last");
    if (tabs) tabs->setCurrent(0);
    key(c, Qt::Key_Escape);
    w.setLanguage(true);   // the longer Portuguese names fit in the tabs and the calibration panel
    w.menuTabs->setCurrent(0);
    w.openMenu(QPoint(60, 40));
    pause(300);
    w.grab().save(out + "/vaxmaps-menu-pt.png");
    key(c, Qt::Key_Escape);
    m->setSelected(true);
    c->setTool(Canvas::Calibrate);
    key(c, Qt::Key_S, "s");
    pause(300);
    w.grab().save(out + "/vaxmaps-calibration-pt.png");
    key(c, Qt::Key_Escape);
    w.setLanguage(false);
    w.dirty = false;
}

// What 0.9 added.
void version09(MainWindow &w, const QString &dir, const QString &map1, const QString &out) {
    say("\n== 0.9 ==");
    Canvas *c = w.canvas;
    w.newProject();
    c->activeLayer = w.layers[0];
    auto mapsNow = [&] {
        QList<MapItem *> r;
        for (QGraphicsItem *it : w.layers[0]->childItems())
            if (auto *m = qgraphicsitem_cast<MapItem *>(it)) r << m;
        return r;
    };

    // grids found on import, one map after the other
    const QString plain = dir + "/plain.png", a = dir + "/gridA.png", b = dir + "/gridB.png";
    gridImage(2000, 1500, 1, {}, 0, 30).save(plain);
    gridImage(2400, 1600, 70.37, QPointF(23.4, 41.9), 200, 25).save(a);
    partialGridImage(3000, 2000, 88.6, QPointF(40.2, 12.7), [] { QPainterPath p; p.addRect(0, 0, 1500, 2000); return p; }(), true).save(b);
    w.importMaps({plain, a, b}, QPointF(0, 0));
    QList<MapItem *> imported = mapsNow();
    auto calibrated = [&] { int n = 0; for (MapItem *m : imported) n += m->cal.valid(); return n; };
    waitFor([&] { return calibrated() == 2; }, 90000);
    pause(500);
    QStringList cells;
    for (MapItem *m : imported) cells << (m->cal.valid() ? QString::number(m->cal.cellSize(), 'f', 2) : "-");
    check(imported.size() == 3 && !imported[0]->cal.valid() && std::abs(imported[1]->cal.cellSize() - 70.37) < 0.3 && std::abs(imported[2]->cal.cellSize() - 88.6) < 0.35,
          "importing 3 maps finds the grid of the 2 that have one: " + cells.join(" "));

    // magnifier, and the point taken where the button is released
    MapItem *m = w.importMap(map1, QPointF(-1500, 5000));   // top-left at -3000,4000, squares of 100 px from 50,50
    waitFor([&] { return m->src->ready; }, 60000);
    w.scene->clearSelection();
    m->setSelected(true);
    c->setTool(Canvas::Calibrate);
    Calibrator *cal = c->calibrator;
    waitFor([&] { return !cal->busy(); }, 30000);
    mouse(c, QEvent::MouseButtonPress, m->mapToScene(QPointF(170, 130)));
    const bool shownWhilePressed = c->loupeShown();
    mouse(c, QEvent::MouseMove, m->mapToScene(QPointF(152, 151)), Qt::LeftButton);
    w.grab().save(out + "/vaxmaps-loupe.png");
    mouse(c, QEvent::MouseButtonRelease, m->mapToScene(QPointF(152, 151)));
    click(c, m->mapToScene(QPointF(249, 150)));
    waitFor([&] { return !cal->busy(); }, 30000);
    auto r = cal->current();
    check(shownWhilePressed && !c->loupeShown() && cal->measured() == 1 && r && std::abs(r->cellSize() - 100) < 0.1,
          QString("the loupe shows while the button is held, the corner is taken where it is released (%1 px)").arg(r ? r->cellSize() : 0, 0, 'f', 2));

    // hexes typed in: pointy or flat
    cal->setHex(true);
    if (auto *flat = findIn<MenuButton>(cal, "Flat")) flat->click();
    cal->useCell(80);
    r = cal->current();
    check(r && r->grid == Calibration::HexFlat && std::abs(r->cellSize() - 80) < 1e-9, "a typed hex size can be flat-top");
    key(c, Qt::Key_Escape);
    check(cachedAnalyses() >= 1 && cachedAnalyses() <= 3, QString("the lines of at most 3 maps stay in memory (%1)").arg(cachedAnalyses()));

    // spell areas on the 100 px squares: drag from a corner to the last square
    m->cal = Calibration::square(100, QPointF(50, 50));
    c->resetTransform();
    c->centerOn(m->mapToScene(QPointF(400, 400)));
    key(c, Qt::Key_A, "a");
    check(c->tool() == Canvas::AreaSphere, "A picks the sphere");
    drag(c, m->mapToScene(QPointF(253, 247)), m->mapToScene(QPointF(395, 390)), 6);
    check(c->areaCount() == 1 && c->areaText(0) == "Sphere 2 squares · 3 m: 16 squares", "sphere: " + c->areaText(0));
    key(c, Qt::Key_A, "a");
    key(c, Qt::Key_A, "a");
    drag(c, m->mapToScene(QPointF(240, 240)), m->mapToScene(QPointF(640, 250)), 6);
    check(c->tool() == Canvas::AreaLine && c->areaCount() == 2 && c->areaText(1) == "Line 4 squares · 6 m: 4 squares", "line: " + c->areaText(1));
    pause(200);
    w.grab().save(out + "/vaxmaps-areas.png");
    key(c, Qt::Key_Escape);
    check(c->areaCount() == 0, "Esc clears the areas with the rulers");

    // walls on the corners, doors that open
    c->activeLayer = w.layers[2];
    key(c, Qt::Key_W, "w");
    click(c, m->mapToScene(QPointF(148, 153)));
    click(c, m->mapToScene(QPointF(455, 147)));
    mouse(c, QEvent::MouseMove, m->mapToScene(QPointF(452, 446)), Qt::NoButton);
    click(c, m->mapToScene(QPointF(452, 446)));
    mouse(c, QEvent::MouseButtonDblClick, m->mapToScene(QPointF(452, 446)));
    mouse(c, QEvent::MouseButtonRelease, m->mapToScene(QPointF(452, 446)));
    QList<StrokeItem *> strokes;
    for (QGraphicsItem *it : w.layers[2]->childItems())
        if (auto *st = qgraphicsitem_cast<StrokeItem *>(it)) strokes << st;
    check(strokes.size() == 1 && strokes[0]->wall && !strokes[0]->door
              && strokes[0]->pts == QVector<QPointF>({m->mapToScene(QPointF(150, 150)), m->mapToScene(QPointF(450, 150)), m->mapToScene(QPointF(450, 450))}),
          QString("a wall clicked corner by corner: %1 points on the corners").arg(strokes.isEmpty() ? 0 : strokes[0]->pts.size()));
    key(c, Qt::Key_D, "d");
    drag(c, m->mapToScene(QPointF(147, 452)), m->mapToScene(QPointF(253, 449)), 5);
    StrokeItem *door = nullptr;
    for (QGraphicsItem *it : w.layers[2]->childItems())
        if (auto *st = qgraphicsitem_cast<StrokeItem *>(it); st && st->door) door = st;
    const QPointF eye = m->mapToScene(QPointF(200, 520));   // only the door is within its reach
    const bool closedBlocks = door && !visibleFrom(w.scene, eye, 80).isEmpty();
    c->setTool(Canvas::Select);
    if (door) {
        mouse(c, QEvent::MouseMove, m->mapToScene(QPointF(200, 450)), Qt::NoButton);
        click(c, m->mapToScene(QPointF(200, 450)));
        mouse(c, QEvent::MouseButtonDblClick, m->mapToScene(QPointF(200, 450)));
        mouse(c, QEvent::MouseButtonRelease, m->mapToScene(QPointF(200, 450)));
    }
    check(door && door->wall && door->pts == QVector<QPointF>({m->mapToScene(QPointF(150, 450)), m->mapToScene(QPointF(250, 450))}) && closedBlocks
              && door->open && visibleFrom(w.scene, eye, 80).isEmpty(),
          "a door dragged corner to corner blocks vision; a double click opens it and vision passes");
    const QString proj = dir + "/doors.vaxmap";
    w.saveTo(proj);
    w.openProject(proj);
    int walls = 0, openDoors = 0;
    for (QGraphicsItem *it : w.layers[2]->childItems())
        if (auto *st = qgraphicsitem_cast<StrokeItem *>(it)) { walls += st->wall; openDoors += st->door && st->open; }
    check(walls == 2 && openDoors == 1, "walls and an open door are saved with the project");

    // fog by room: Shift + click reveals the room, not the one past the corridor
    for (auto [cell, size] : {std::pair{50, QSize(1500, 1000)}, std::pair{30, QSize(3000, 2000)}}) {   // the overview, then the file read again
        const QString path = dir + QString("/dungeon%1.png").arg(cell);
        dungeonImage(size.width(), size.height(), cell).save(path);
        w.newProject();
        c->activeLayer = w.layers[0];
        MapItem *d = w.importMap(path, QPointF(size.width() / 2.0, size.height() / 2.0));
        waitFor([&] { return d->src->ready; }, 60000);
        d->cal = Calibration::square(cell, QPointF(0, 0));
        d->enableFog(true);
        auto fogAt = [&](QPointF p) {
            return qAlpha(d->fog.pixel(int(p.x() * d->fog.width() / d->boundingRect().width()), int(p.y() * d->fog.height() / d->boundingRect().height())));
        };
        auto clearIn = [&](QRect cells) {
            int n = 0;
            for (int i = cells.left(); i <= cells.right(); ++i)
                for (int j = cells.top(); j <= cells.bottom(); ++j) n += fogAt(QPointF((i + 0.5) * cell, (j + 0.5) * cell)) == 0;
            return n;
        };
        c->setTool(Canvas::Fog);
        c->resetTransform();
        c->centerOn(d->mapToScene(QPointF(6 * cell, 6 * cell)));
        QElapsedTimer t;
        t.start();
        mouse(c, QEvent::MouseButtonPress, d->mapToScene(QPointF(4.3 * cell, 7.6 * cell)), Qt::LeftButton, Qt::ShiftModifier);
        mouse(c, QEvent::MouseButtonRelease, d->mapToScene(QPointF(4.3 * cell, 7.6 * cell)), Qt::LeftButton, Qt::ShiftModifier);
        const qint64 ms = t.elapsed();
        const int roomA = clearIn(QRect(2, 2, 8, 8)), roomB = clearIn(QRect(13, 3, 8, 8)), outside = clearIn(QRect(22, 12, 6, 6));
        check(roomA >= 62 && roomB == 0 && outside == 0,
              QString("Shift + click reveals the room (cells %1 px): %2 of its 64 squares clear, %3 of the next room's, %4 outside, %5 ms")
                  .arg(cell).arg(roomA).arg(roomB).arg(outside).arg(ms));
        if (cell == 50) {
            pause(200);
            w.grab().save(out + "/vaxmaps-room-fog.png");
        }
        mouse(c, QEvent::MouseButtonPress, d->mapToScene(QPointF(4.3 * cell, 7.6 * cell)), Qt::LeftButton, Qt::ShiftModifier | Qt::AltModifier);
        mouse(c, QEvent::MouseButtonRelease, d->mapToScene(QPointF(4.3 * cell, 7.6 * cell)), Qt::LeftButton, Qt::ShiftModifier | Qt::AltModifier);
        check(clearIn(QRect(2, 2, 8, 8)) == 0, "Shift + Alt + click covers the room again");
    }
    c->setTool(Canvas::Select);

    // command palette and the menu's number keys
    w.showPalette();
    auto *edit = w.props->findChild<QLineEdit *>();
    if (edit) {
        edit->setText("fog");
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(edit, &down);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        const bool was = fogOpaque;
        QApplication::sendEvent(edit, &enter);
        check(fogOpaque != was && !w.props->isShown(), "Ctrl+K: typing \"fog\", the arrow down and Enter run the second command found (Opaque fog)");
        fogOpaque = was;
    } else check(false, "the command palette has a text field");
    w.setLanguage(true);
    w.showPalette();
    if ((edit = w.props->findChild<QLineEdit *>())) {
        edit->setText("pincel de nevoa");   // without the accent
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(edit, &enter);
    }
    check(c->tool() == Canvas::Fog, "the palette finds commands in Portuguese without accents");
    c->setTool(Canvas::Select);
    w.openMenu(QPoint(60, 40));
    key(c, Qt::Key_3, "3");
    const int third = w.menuTabs->current();
    key(c, Qt::Key_1, "1");
    pause(250);
    w.grab().save(out + "/vaxmaps-menu-0.9-pt.png");
    check(third == 2 && w.menuTabs->current() == 0, "keys 1 to 4 switch the tabs of the open menu");
    key(c, Qt::Key_Escape);
    for (int tool = Canvas::Wall; tool <= Canvas::AreaLine; ++tool) c->setTool(Canvas::Tool(tool));   // every new hint
    c->setTool(Canvas::Select);
    w.showShortcuts();
    w.grab();
    key(c, Qt::Key_Escape);
    w.setLanguage(false);
    w.dirty = false;
}

} // namespace

int runSelfTest(const QStringList &args) {
    const int at = args.indexOf("--selftest");
    const QString out = (at + 1 < args.size() && !args[at + 1].startsWith("--")) ? args[at + 1] : QDir::currentPath();
    QDir().mkpath(out);
    QFile log(out + "/vaxmaps-selftest.txt");
    if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) logFile = &log;
    QTemporaryDir tmp;

    // ---- pure logic ----
    check(MapItem::levelFor(1.0, 4) == 0 && MapItem::levelFor(2.0, 4) == 0, "level 0 at zoom >= 100%");
    check(MapItem::levelFor(0.5, 4) == 1 && MapItem::levelFor(0.3, 4) == 1 && MapItem::levelFor(0.2, 4) == 2, "pyramid level follows the zoom");
    check(MapItem::levelFor(0.001, 4) == 3, "lowest zoom uses the overview");
    {
        QVector<QPointF> line;
        for (int x = 0; x <= 100; ++x) line << QPointF(x, 0);
        const auto runs = eraseRuns(line, QPointF(50, 0), 10);
        check(runs.size() == 2 && runs[0].size() == 41 && runs[1].size() == 41
                  && runs[0].last() == QPointF(40, 0) && runs[1].first() == QPointF(60, 0),
              "eraser splits a dense stroke in two");
        const auto seg = eraseRuns({QPointF(0, 0), QPointF(100, 0)}, QPointF(50, 0), 10);
        check(seg.size() == 2 && seg[0] == QVector<QPointF>{QPointF(0, 0), QPointF(40, 0)}
                  && seg[1] == QVector<QPointF>{QPointF(60, 0), QPointF(100, 0)},
              "eraser cuts a long segment at the circle");
        check(eraseRuns(line, QPointF(50, 30), 10).size() == 1, "eraser far away leaves the stroke");
        check(eraseRuns(line, QPointF(50, 0), 200).isEmpty(), "big eraser removes the whole stroke");
    }
    {   // grid model
        const Calibration sq = Calibration::square(100, QPointF(50, 50));
        check(sq.cellAt(QPointF(160, 260)) == QPoint(1, 2) && sq.steps(QPoint(0, 0), QPoint(3, 4)) == 4 && sq.cellCentre(QPoint(0, 0)) == QPointF(100, 100),
              "squares: cell under a point, diagonal counts 1 (D&D 5e)");
        const Calibration hx = Calibration::hex(Calibration::HexPointy, 80, QPointF(100, 100));
        const QPointF far = hx.origin + 2 * hx.a - hx.b + QPointF(10, -8);
        check(hx.cellAt(far) == QPoint(2, -1) && hx.steps(QPoint(0, 0), QPoint(2, -1)) == 2 && hx.steps(QPoint(0, 0), QPoint(3, 0)) == 3,
              "hexes: hex under a point and hex distance");
        Calibration warped = sq;
        warped.fieldCols = warped.fieldRows = 2;
        warped.fieldStep = QPointF(1000, 1000);
        warped.field = {QPointF(0, 0), QPointF(10, 0), QPointF(0, 0), QPointF(10, 0)};
        const QPointF p = warped.fromGrid(QPointF(5.5, 2));
        check(QLineF(warped.toGrid(p), QPointF(5.5, 2)).length() < 1e-3 && std::abs(p.x() - (50 + 550 + 6.05)) < 0.1,
              "local corrections: fromGrid and toGrid invert each other");
    }
    {   // spell areas, counted by the diagonal rule
        Calibration sq = Calibration::square(100, QPointF(0, 0));
        int len = 0;
        const int sphere = int(areaCells(sq, AreaShape::Sphere, QPointF(200, 200), QPointF(350, 350), &len).size());
        check(len == 2 && sphere == 16, QString("sphere of 2 squares from a corner, diagonal 1: %1 squares, length %2 (4 x 4 expected)").arg(sphere).arg(len));
        sq.diagonal = Calibration::Alternating;
        len = 3;
        const int alternating = int(areaCells(sq, AreaShape::Sphere, QPointF(200, 200), QPointF(300, 300), &len).size());
        sq.diagonal = Calibration::Straight;
        len = 2;
        const int straight = int(areaCells(sq, AreaShape::Sphere, QPointF(200, 200), QPointF(300, 300), &len).size());
        check(alternating == 32 && straight == 12, QString("sphere by the 1-2-1 rule (3 squares): %1, straight line (2): %2 (32 and 12)").arg(alternating).arg(straight));
        sq.diagonal = Calibration::One;
        len = 3;
        const QVector<QPoint> cone = areaCells(sq, AreaShape::Cone, QPointF(200, 200), QPointF(600, 200), &len);
        len = 3;
        const QVector<QPoint> diagonalCone = areaCells(sq, AreaShape::Cone, QPointF(200, 200), QPointF(600, 600), &len);
        check(cone.size() == 4 && cone.contains(QPoint(3, 1)) && cone.contains(QPoint(4, 2)) && !cone.contains(QPoint(2, 2)) && diagonalCone.contains(QPoint(4, 4))
                  && !diagonalCone.contains(QPoint(5, 5)),
              QString("cone of 3 squares: %1 squares along the grid, reaches 3 squares along the diagonal").arg(cone.size()));
        len = 4;
        const QVector<QPoint> line = areaCells(sq, AreaShape::Line, QPointF(250, 250), QPointF(750, 260), &len);
        len = 0;
        const QVector<QPoint> diagonal = areaCells(sq, AreaShape::Line, QPointF(250, 250), QPointF(650, 650), &len);
        check(line == QVector<QPoint>({{3, 2}, {4, 2}, {5, 2}, {6, 2}}) && len == 4 && diagonal.size() == 4 && diagonal.contains(QPoint(6, 6)),
              QString("line: 4 squares along a row, 4 along the diagonal (%1)").arg(diagonal.size()));
        const Calibration hx = Calibration::hex(Calibration::HexPointy, 80, QPointF(400, 400));
        len = 2;
        check(areaCells(hx, AreaShape::Sphere, QPointF(400, 400), QPointF(560, 400), &len).size() == 19, "hex sphere of 2: 1 + 6 + 12 hexes");
    }
    {   // measurements
        const Calibration one = gridFromMeasurements({{QPointF(148, 152), QPointF(249, 152)}}, false);
        check(one.cellSize() == 101 && offBy(one.origin.x(), 47, 101) < 1e-6 && offBy(one.origin.y(), 51, 101) < 1e-6,
              "one measured square: size and corner");
        const Calibration wrap = gridFromMeasurements({{QPointF(0.5, 10), QPointF(100.5, 10)}, {QPointF(299.5, 10), QPointF(399.5, 10)}}, false);
        check(wrap.cellSize() == 100 && offBy(wrap.origin.x(), 0, 100) < 0.01, "circular mean of corners (0.5 and 99.5 give 0)");
        const Calibration pointy = gridFromMeasurements({{QPointF(100, 100), QPointF(180, 101)}}, true);
        const Calibration flat = gridFromMeasurements({{QPointF(100, 100), QPointF(100 + 80 * 0.866, 100 - 40)}}, true);
        check(pointy.grid == Calibration::HexPointy && std::abs(pointy.cellSize() - 80) < 0.1 && flat.grid == Calibration::HexFlat
                  && std::abs(flat.cellSize() - 80) < 0.1,
              "hex measurement tells pointy-top from flat-top by the angle");
    }
    {   // detection over the whole map
        struct Case { const char *name; int w, h; Calibration truth; QImage img; };
        QVector<Case> cases;
        const Calibration sharp = Calibration::square(70.37, QPointF(23.4, 41.9));
        cases.push_back({"sharp squares", 3000, 2000, sharp, gridImage(3000, 2000, 70.37, QPointF(23.4, 41.9), 200, 25)});
        cases.push_back({"faint squares on texture", 3000, 2000, Calibration::square(88.6, QPointF(40.2, 12.7)),
                         gridImage(3000, 2000, 88.6, QPointF(40.2, 12.7), 70, 45)});
        cases.push_back({"big map, analysed reduced", 9000, 6000, Calibration::square(141.3, QPointF(60.2, 17.7)),
                         gridImage(9000, 6000, 141.3, QPointF(60.2, 17.7), 200, 25)});
        const Calibration hp = Calibration::hex(Calibration::HexPointy, 90.4, QPointF(61.3, 47.9));
        cases.push_back({"pointy-top hexes", 3000, 2000, hp, hexImage(3000, 2000, hp)});
        const Calibration hf = Calibration::hex(Calibration::HexFlat, 76.2, QPointF(33.1, 80.6));
        cases.push_back({"flat-top hexes", 3000, 2000, hf, hexImage(3000, 2000, hf)});
        const Calibration tp = Calibration::hex(Calibration::HexPointy, 84.7, QPointF(52.2, 31.6));
        cases.push_back({"pointy-top hexes filled with terrain", 3000, 2000, tp, terrainHexImage(3000, 2000, tp)});
        const Calibration tf = Calibration::hex(Calibration::HexFlat, 121.3, QPointF(70.4, 44.8));
        cases.push_back({"flat-top hexes filled with terrain", 3000, 2000, tf, terrainHexImage(3000, 2000, tf)});
        // thick edges: their slanted sides used to line up as flat-top hexes √3 times wider, or as squares
        const Calibration th = Calibration::hex(Calibration::HexPointy, 40, QPointF(17.3, 29.1));
        cases.push_back({"small pointy-top hexes with thick edges", 3000, 2000, th, terrainHexImage(3000, 2000, th, 3, 255, 40)});
        const Calibration big = Calibration::hex(Calibration::HexFlat, 150, QPointF(17.3, 29.1));
        cases.push_back({"big flat-top hexes", 3000, 2000, big, terrainHexImage(3000, 2000, big, 5, 255, 40)});
        for (const Case &k : cases) {
            const QImage small = std::max(k.w, k.h) > 4096 ? k.img.scaled(4096, 4096, Qt::KeepAspectRatio, Qt::SmoothTransformation) : k.img;
            const Analysis a = analyse(small, k.img.size());
            const QVector<QPointF> truth = latticePoints(k.truth, k.w, k.h, 40);
            for (auto [how, g] : {std::pair{"fit to a measurement", fitGrid(a, k.truth.grid, k.truth.cellSize() * 0.95)},
                                  std::pair{"automatic detection", detectGrid(a)}}) {
                const double err = g ? latticeError(g->cal, truth) : 1;
                check(g && g->cal.grid == k.truth.grid && std::abs(g->cal.cellSize() - k.truth.cellSize()) < 0.004 * k.truth.cellSize() && err < 0.05,
                      QString("%1, %2: %3").arg(k.name, how,
                          g ? QString("%1, cell %2 px (true %3), worst lattice error %4 cells, strength %5")
                                  .arg(g->cal.isHex() ? "hexes" : "squares").arg(g->cal.cellSize(), 0, 'f', 3).arg(k.truth.cellSize())
                                  .arg(err, 0, 'f', 3).arg(g->strength, 0, 'f', 2)
                            : QString("nothing found")));
            }
        }
        // Maps whose squares do not cover the whole map: each quarter of the map is no longer a sample of the grid.
        struct Partial { const char *name; QPainterPath area; bool dark; int quarters = -1; };   // quarters: expected GridFit::quarters
        QVector<Partial> partial;
        auto rect = [](double x, double y, double w, double h) { QPainterPath p; p.addRect(x, y, w, h); return p; };
        partial.push_back({"squares on the left half only", rect(0, 0, 1500, 2000), false, 0b0101});
        partial.push_back({"squares in the centre, painted border", rect(750, 500, 1500, 1000), false});
        partial.push_back({"squares under a title band", rect(0, 650, 3000, 1350), false});
        partial.push_back({"squares in one quarter only", rect(1500, 1000, 1500, 1000), false, 0b1000});
        partial.push_back({"squares on the top half, black below", rect(0, 0, 3000, 1000), true, 0b0011});   // the edge to black lined up as hexes
        QPainterPath rooms;   // a dungeon: squares in the rooms, black around them
        for (QRect r : {QRect(2, 2, 8, 6), QRect(14, 3, 6, 9), QRect(26, 1, 12, 7), QRect(5, 14, 10, 10), QRect(22, 15, 7, 6), QRect(33, 13, 6, 12)})
            rooms.addRect(23.4 + r.x() * 70.37, 41.9 + r.y() * 70.37, r.width() * 70.37, r.height() * 70.37);
        partial.push_back({"squares only in the rooms, black around", rooms, true});
        for (const Partial &k : partial) {
            const Calibration truth = Calibration::square(70.37, QPointF(23.4, 41.9));
            const Analysis a = analyse(partialGridImage(3000, 2000, 70.37, truth.origin, k.area, k.dark), QSize(3000, 2000));
            QVector<QPointF> inside;
            for (const QPointF &p : latticePoints(truth, 3000, 2000, 40))
                if (k.area.contains(p) && k.area.contains(p + QPointF(35, 35)) && k.area.contains(p - QPointF(35, 35))) inside << p;
            for (auto [how, g] : {std::pair{"fit to a measurement", fitGrid(a, Calibration::Square, 70.37 * 1.04)},
                                  std::pair{"automatic detection", detectGrid(a)}}) {
                const double err = g ? latticeError(g->cal, inside) : 1;
                check(g && !g->cal.isHex() && std::abs(g->cal.cellSize() - 70.37) < 0.004 * 70.37 && err < 0.05 && (k.quarters < 0 || g->quarters == k.quarters),
                      QString("%1, %2: %3").arg(k.name, how,
                          g ? QString("%1, cell %2 px (true 70.37), worst error %3 cells inside the grid, strength %4, quarters %5")
                                  .arg(g->cal.isHex() ? "hexes" : "squares").arg(g->cal.cellSize(), 0, 'f', 3).arg(err, 0, 'f', 3).arg(g->strength, 0, 'f', 2)
                                  .arg(g->quarters, 4, 2, QChar('0'))
                            : QString("nothing found")));
            }
        }
        {   // hexes on part of the map
            const Calibration hx = Calibration::hex(Calibration::HexPointy, 84.7, QPointF(52.2, 31.6));
            QImage img = paintedImage(3000, 2000, false);
            QPainter p(&img);
            p.setClipRect(0, 0, 1500, 2000);
            p.drawImage(0, 0, terrainHexImage(3000, 2000, hx));
            p.end();
            QVector<QPointF> inside;
            for (const QPointF &q : latticePoints(hx, 1460, 2000, 40)) inside << q;
            const Analysis a = analyse(img, img.size());
            for (auto [how, g] : {std::pair{"fit to a measurement", fitGrid(a, Calibration::HexPointy, 84.7 * 0.96)},
                                  std::pair{"automatic detection", detectGrid(a)}}) {
                const double err = g ? latticeError(g->cal, inside) : 1;
                check(g && g->cal.grid == Calibration::HexPointy && std::abs(g->cal.cellSize() - 84.7) < 0.004 * 84.7 && err < 0.05,
                      QString("hexes on the left half only, %1: %2").arg(how,
                          g ? QString("cell %1 px (true 84.7), worst error %2 cells inside the grid").arg(g->cal.cellSize(), 0, 'f', 3).arg(err, 0, 'f', 3)
                            : QString("nothing found")));
            }
        }
        QStringList invented;   // art, walls and noise: no grid may come out of them
        for (int i = 0; i < 6; ++i) {
            const QImage img = i % 2 ? paintedImage(3000, 2000, false) : gridImage(3000, 2000, 1, {}, 0, 25 + 10 * i);
            if (const auto g = detectGrid(analyse(img, img.size())))
                invented << QString("%1 px, strength %2").arg(g->cal.cellSize(), 0, 'f', 1).arg(g->strength, 0, 'f', 2);
        }
        check(invented.isEmpty(), "6 maps without a grid (painted art, walls): detection invents none " + invented.join("; "));
        // A grid that is not regular: cells grow from 66 to 74 px across the map and rows tilt by 0.25°.
        QVector<double> xs, ys;
        for (double x = 30, c = 66; x < 3000; x += c, c += 8.0 / 42) xs << x;
        for (double y = 25; y < 2000; y += 70) ys << y;
        const double slope = 0.0044;
        const Analysis wa = analyse(linesImage(3000, 2000, xs, ys, slope), QSize(3000, 2000));
        QVector<QPointF> truth;
        for (double x : xs)
            for (double y : ys)
                if (x > 60 && x < 2940 && y + slope * x > 60 && y + slope * x < 1940) truth << QPointF(x, y + slope * x);
        const auto g = detectGrid(wa);
        double regular = 1, local = 1;
        if (g) {
            local = latticeError(g->cal, truth);
            Calibration flatGrid = g->cal;
            flatGrid.field.clear();
            regular = latticeError(flatGrid, truth);
        }
        check(g && local < 0.08 && local < regular / 4,
              QString("uneven grid: local corrections follow it (worst error %1 cells with them, %2 without, %3 regions)")
                  .arg(local, 0, 'f', 3).arg(regular, 0, 'f', 3).arg(g ? g->regions : 0));
        const auto none = detectGrid(analyse(gridImage(3000, 2000, 1, {}, 0, 45), QSize(3000, 2000)));
        check(!none, QString("map without a grid: detection invents none%1").arg(none ? QString(" (found %1 px, strength %2)").arg(none->cal.cellSize()).arg(none->strength) : ""));
    }

    // ---- app ----
    const QString map1 = makeMap(tmp.path(), "dungeon.jpg", 3000, 2000, 100, QPoint(50, 50), false);
    const QString map2 = makeMap(tmp.path(), "tower.png", 1600, 1200, 64, QPoint(0, 0), true);
    const Calibration hexTruth = Calibration::hex(Calibration::HexPointy, 90, QPointF(45, 52));
    const QString map3 = tmp.path() + "/surface.png";
    hexImage(2400, 1600, hexTruth).save(map3);
    MainWindow w;
    w.show();
    Canvas *c = w.canvas;
    MapItem *m1 = w.importMap(map1, QPointF(1500, 1000));
    MapItem *m2 = w.importMap(map2, QPointF(4500, 600));
    MapItem *m3 = w.importMap(map3, QPointF(1200, 3400));
    check(m1 && m2 && m3, "import JPG and PNG maps");
    if (!m1 || !m2 || !m3) return 1;
    check(waitFor([&] { return m1->src->ready && m2->src->ready && m3->src->ready; }, 120000), "tile pyramids built in the background");
    check(m1->pos() == QPointF(0, 0), "map centred on the import point");
    check(m1->src->levels == 4 && m1->src->ext == "png" && m2->src->ext == "png", "4 levels, PNG tiles");
    const int tiles = QDir(m1->src->dir).entryList({"?_*_*.png"}).size();
    check(tiles == 24 + 6 + 2, QString("tiles on disk: %1 (expected 32)").arg(tiles));
    {
        const QImage source(map1), tile(m1->src->dir + "/0_1_1.png");   // content 510..1019 plus 1 px border from 509
        bool same = tile.size() == QSize(TILE + 2, TILE + 2);
        for (int y = 0; same && y < tile.height(); ++y)
            for (int x = 0; same && x < tile.width(); ++x) same = tile.pixel(x, y) == source.pixel(509 + x, 509 + y);
        check(same, "tile identical to the source image, pixel for pixel (lossless)");
    }
    check(w.layers.size() == 5 && w.layers.last()->links && w.layers.last()->name == "Links" && c->linksLayer == w.layers.last(),
          "new project has a Links layer on top");

    // rendering: tile placement compared with the source image at several zoom levels
    for (double z : {1.0, 0.73, 0.3, 0.12}) {
        c->resetTransform();
        c->scale(z, z);
        c->centerOn(m1->mapToScene(QPointF(1020, 1020)));   // a tile corner at level 0 and 1
        settle(c);
        QString got;
        bool ok = true;
        for (QPointF p : {QPointF(1000, 1000), QPointF(1021, 1018), QPointF(510, 1000), QPointF(1100, 1100)}) {
            const QPointF centre(std::floor((p.x() - 50) / 100) * 100 + 100, std::floor((p.y() - 50) / 100) * 100 + 100);
            ok &= pixelMatches(c, m1, centre, bg(int(centre.x()), int(centre.y()), 3000, 2000), 14, &got);
        }
        check(ok, QString("render at zoom %1%: colour matches the source image (%2)").arg(z * 100).arg(got));
    }

    // calibration, step by step, as a user does it
    w.scene->clearSelection();
    m1->setSelected(true);
    c->setTool(Canvas::Calibrate);
    Calibrator *cal = c->calibrator;
    check(cal->target() == m1 && cal->isShown(), "Calibrate with a map selected opens the panel on it");
    click(c, m1->mapToScene(QPointF(2400, 300)));   // outside the top-left quarter: ignored
    click(c, m1->mapToScene(QPointF(152, 148)));    // ~2 px off the corner (150, 150)
    click(c, m1->mapToScene(QPointF(247, 153)));
    check(cal->measured() == 1 && m1->preview, "step 1 shows a grid at once, even before the map's lines are read");
    waitFor([&] { return !cal->busy(); }, 30000);
    auto r = cal->current();
    check(r && cal->confirmed() && std::abs(r->cellSize() - 100) < 0.05 && offBy(r->origin.x(), 50.5, 100) < 1 && offBy(r->origin.y(), 50.5, 100) < 1,
          r ? QString("step 1 matched to the lines: %1 px, corner %2,%3").arg(r->cellSize(), 0, 'f', 3).arg(r->origin.x(), 0, 'f', 2).arg(r->origin.y(), 0, 'f', 2)
            : QString("step 1 gave no grid"));
    click(c, m1->mapToScene(QPointF(1752, 352)));   // step 2, in the top-right quarter
    click(c, m1->mapToScene(QPointF(1849, 350)));
    click(c, m1->mapToScene(QPointF(249, 1449)));   // step 3, bottom-left
    click(c, m1->mapToScene(QPointF(351, 1452)));
    waitFor([&] { return !cal->busy(); }, 30000);
    r = cal->current();
    check(cal->measured() == 3 && r && cal->confirmed() && std::abs(r->cellSize() - 100) < 0.05, "steps 2 and 3 are taken and the grid stays matched");
    settle(c);
    pause(300);
    w.grab().save(out + "/vaxmaps-calibration.png");
    key(c, Qt::Key_Return);
    check(m1->cal.valid() && std::abs(m1->cal.cellSize() - 100) < 0.05 && !m1->preview && !m1->cal.showGrid && c->tool() == Canvas::Select
              && !cal->isShown(),
          "Enter applies: calibration kept, grid hidden, panel closed");
    m1->setSelected(true);
    c->setTool(Canvas::Calibrate);
    key(c, Qt::Key_F, "f");
    waitFor([&] { return !cal->busy(); }, 60000);
    auto d = cal->current();
    check(d && !d->isHex() && std::abs(d->cellSize() - 100) < 0.05, d ? QString("automatic detection on the map: %1 px").arg(d->cellSize(), 0, 'f', 3) : QString("automatic detection failed"));
    const double before = m1->cal.cellSize();
    key(c, Qt::Key_Escape);
    check(m1->cal.cellSize() == before && !m1->preview && c->tool() == Canvas::Select, "Esc cancels without changing the calibration");

    // the surface map is hexes: manual hex measurement, then detection
    w.scene->clearSelection();
    m3->setSelected(true);
    c->setTool(Canvas::Calibrate);
    cal->setHex(true);
    const QPointF h0 = hexTruth.origin + 3 * hexTruth.a + 2 * hexTruth.b;   // a hex centre in the top-left quarter
    click(c, m3->mapToScene(h0 + QPointF(2, -2)));
    click(c, m3->mapToScene(h0 + hexTruth.a + QPointF(-1, 2)));
    waitFor([&] { return !cal->busy(); }, 30000);
    r = cal->current();
    const QVector<QPointF> hexCentres = latticePoints(hexTruth, 2400, 1600, 40);
    check(r && r->grid == Calibration::HexPointy && cal->confirmed() && latticeError(*r, hexCentres) < 0.05,
          r ? QString("hex map: one measured pair matched to the lines (%1 px, error %2 cells)").arg(r->cellSize(), 0, 'f', 2).arg(latticeError(*r, hexCentres), 0, 'f', 3)
            : QString("hex measurement gave no grid"));
    key(c, Qt::Key_Return);
    check(m3->cal.isHex() && m1->cal.grid == Calibration::Square, "squares and hexes on the same canvas, one calibration per map");

    // rulers: squares or hexes, several at once, with turns
    m1->cal = Calibration::square(100, QPointF(50, 50));
    m1->cal.unitsPerCell = 1.5;
    m1->cal.unit = "m";
    m3->cal.unitsPerCell = 1;
    m3->cal.unit = "km";
    check(measureText(w.scene, m1->mapToScene(QPointF(100, 100)), m1->mapToScene(QPointF(400, 500))) == "4 squares · 6 m", "ruler counts squares");
    check(snapPoint(w.scene, m1->mapToScene(QPointF(148, 61))) == m1->mapToScene(QPointF(150, 50)), "snap to the nearest corner");
    check(snapPoint(w.scene, m1->mapToScene(QPointF(148, 61)), true) == m1->mapToScene(QPointF(100, 100)), "ruler snap to the cell centre");
    check(measureText(w.scene, QPointF(-500, -500), QPointF(-200, -100)) == "Map not calibrated (K)", "ruler off a calibrated map says so");
    check(measureText(w.scene, m3->mapToScene(hexTruth.origin + 2 * hexTruth.a), m3->mapToScene(hexTruth.origin + 5 * hexTruth.a + hexTruth.b)) == "4 hexes · 4 km",
          "ruler counts hexes on the hex map");
    c->resetTransform();
    c->centerOn(1000, 600);
    c->setTool(Canvas::Measure);
    drag(c, QPointF(100, 100), QPointF(400, 100), 6);
    check(c->rulerCount() == 1 && c->rulerText(0) == "3 squares · 4.5 m", "ruler: " + c->rulerText(0));
    drag(c, QPointF(400, 100), QPointF(400, 500), 6);   // from its end: a turn
    check(c->rulerCount() == 1 && c->rulerText(0) == "Total: 7 squares · 10.5 m", "turn added to the same ruler: " + c->rulerText(0));
    drag(c, QPointF(1200, 800), QPointF(1500, 1100), 6);
    check(c->rulerCount() == 2, "a second ruler at the same time");
    pause(300);
    settle(c);
    w.grab().save(out + "/vaxmaps-rulers.png");
    key(c, Qt::Key_Escape);
    check(c->rulerCount() == 0, "Esc clears every ruler");
    check(totalText({measureSpan(w.scene, m1->mapToScene(QPointF(100, 100)), m1->mapToScene(QPointF(300, 100))),
                     measureSpan(w.scene, m3->mapToScene(hexTruth.origin), m3->mapToScene(hexTruth.origin + 2 * hexTruth.a))})
              == "2 squares · 3 m + 2 hexes · 2 km",
          "a path over a square map and a hex map adds each grid apart");

    // links between maps
    c->setTool(Canvas::Link);
    check(linksVisible, "the Link tool shows the links");
    drag(c, QPointF(200, 200), QPointF(300, 300), 4);        // stairs on the dungeon
    c->resetTransform();
    c->centerOn(4500, 600);
    drag(c, QPointF(4300, 400), QPointF(4400, 500), 4);      // their pair on the tower
    QList<LinkItem *> links;
    for (QGraphicsItem *it : c->linksLayer->childItems())
        if (it->type() == LinkItem::Type) links << static_cast<LinkItem *>(it);
    check(links.size() == 2 && links[0]->partner == links[1] && links[1]->partner == links[0] && c->tool() == Canvas::Select,
          "two dragged areas become a linked pair in the Links layer");
    key(c, Qt::Key_J, "J", Qt::ShiftModifier);
    check(!linksVisible && links.size() == 2 && !(links[0]->flags() & QGraphicsItem::ItemIsSelectable), "Shift+J hides the links");
    c->resetTransform();
    c->scale(0.8, 0.8);
    c->centerOn(250, 250);
    const QPointF clickAt(220, 280);
    const QPoint screenAt = c->mapFromScene(clickAt);
    click(c, clickAt);
    waitFor([&] { return !c->cameraMoving(); }, 3000);
    check(QLineF(c->mapToScene(screenAt), QPointF(4320, 480)).length() < 2 && std::abs(c->zoom() - 0.8) < 1e-9,
          "click on a hidden link: the camera moves to the pair, same spot on screen, same zoom");
    c->resetTransform();
    c->centerOn(1000, 600);

    // mouse interaction through the real event path
    LayerItem *maps = w.layers[0], *zones = w.layers[1], *drawings = w.layers[2], *texts = w.layers[3];
    c->activeLayer = drawings;
    c->setTool(Canvas::Pen);
    drag(c, QPointF(600, 300), QPointF(1000, 300), 40);
    check(countIn<StrokeItem>(drawings) == 1, "pen draws a stroke");
    c->setTool(Canvas::Eraser);
    drag(c, QPointF(800, 290), QPointF(800, 310), 4);
    check(countIn<StrokeItem>(drawings) == 2, "eraser in the middle splits the stroke");

    c->activeLayer = zones;
    c->setTool(Canvas::ZoneRect);
    drag(c, QPointF(200, 600), QPointF(400, 700));
    c->setTool(Canvas::ZoneEllipse);
    drag(c, QPointF(500, 600), QPointF(650, 680), 6);
    snapEnabled = true;
    c->setTool(Canvas::ZonePoly);
    for (QPointF p : {QPointF(752, 548), QPointF(948, 553), QPointF(851, 748)}) click(c, p);
    mouse(c, QEvent::MouseButtonDblClick, QPointF(851, 748));
    mouse(c, QEvent::MouseButtonRelease, QPointF(851, 748));
    snapEnabled = false;
    QList<ZoneItem *> zs;
    for (QGraphicsItem *it : zones->childItems()) if (auto *z = qgraphicsitem_cast<ZoneItem *>(it)) zs << z;
    check(zs.size() == 3, QString("zones made: %1 (rectangle, circle, vertices)").arg(zs.size()));
    if (zs.size() == 3) {
        check(zs[2]->shapeKind == ZoneItem::Poly && zs[2]->pts.size() == 3
                  && zs[2]->pts[0] == m1->mapToScene(QPointF(750, 550)) && zs[2]->pts[2] == m1->mapToScene(QPointF(850, 750)),
              "polygon with 3 vertices snapped to the grid");
        check(zs[2]->isSelected() && c->tool() == Canvas::Select, "a new zone is selected");
        zs[0]->name = "Throne room";
        zs[0]->setDesc("Two guards.\nSecret door to the north.");
        zs[1]->name = "Well";
        zs[1]->color = QColor(40, 120, 255);

        // resize after creation: box handles sit 11 px outside the box
        w.scene->clearSelection();
        zs[0]->setSelected(true);
        pause(250);   // handles fade in; fully transparent items take no clicks
        const QPoint br = c->mapFromScene(zs[0]->mapToScene(QPointF(400, 700))) + QPoint(11, 11);
        dragView(c, br, br + QPoint(50, 60));
        check(QRectF(zs[0]->pts[0], zs[0]->pts[1]).normalized() == QRectF(QPointF(200, 600), QPointF(450, 760)), "rectangle zone resized by its corner handle");
        const QPoint right = c->mapFromScene(zs[0]->mapToScene(QPointF(450, 680))) + QPoint(11, 0);
        dragView(c, right, right + QPoint(-30, 0));
        check(zs[0]->box() == QRectF(QPointF(200, 600), QPointF(420, 760)), "zone resized by its side handle");
        const QPoint tl = c->mapFromScene(zs[0]->mapToScene(QPointF(200, 600))) + QPoint(-11, -11);
        dragView(c, tl, tl + QPoint(-40, 0), Qt::ShiftModifier);
        check(std::abs(zs[0]->box().width() / zs[0]->box().height() - 220.0 / 160) < 0.01 && zs[0]->box().bottomRight() == QPointF(420, 760),
              "Shift on a corner keeps the proportions");
        w.scene->clearSelection();
        zs[2]->setSelected(true);
        pause(250);
        const QPoint ptl = c->mapFromScene(zs[2]->mapToScene(QPointF(750, 550))) + QPoint(-11, -11);
        dragView(c, ptl, ptl + QPoint(-50, -50));
        check(zs[2]->box() == QRectF(QPointF(700, 500), QPointF(950, 750)) && zs[2]->pts[2] == QPointF(825, 750), "polygon resized by its box, vertices scale");
        const QPoint v0 = c->mapFromScene(zs[2]->mapToScene(QPointF(700, 500)));
        dragView(c, v0, v0 + QPoint(20, 20));
        check(zs[2]->pts[0] == QPointF(720, 520), "polygon vertex still draggable");

        // right click: properties of the selected zone
        w.scene->clearSelection();
        zs[0]->setSelected(true);
        rightClick(c, zs[0]->mapToScene(zs[0]->box().center()));
        pause(300);
        auto *nameEdit = w.props->findChild<QLineEdit *>();
        check(w.props->isShown() && w.props->isVisible() && !w.menu->isShown() && nameEdit && nameEdit->text() == "Throne room",
              "right click on the selected zone opens its properties");
        if (nameEdit) nameEdit->setText("Throne room!");
        check(zs[0]->name == "Throne room!", "name edited in the popup changes the zone");
        if (nameEdit) nameEdit->setText("Throne room");
        w.grab().save(out + "/vaxmaps-properties.png");
        key(c, Qt::Key_Escape);
        pause(300);
        check(!w.props->isShown() && !w.props->isVisible(), "Esc closes the popup with its exit animation");

        // right click elsewhere: the menu with every command
        rightClick(c, QPointF(1300, 900));
        pause(300);
        check(w.menu->isShown() && w.menu->isVisible() && !w.props->isShown(), "right click away from the selection opens the menu");
        w.grab().save(out + "/vaxmaps-menu.png");
        if (auto *pen = findIn<MenuButton>(w.menu, "Pen")) pen->click();
        check(c->tool() == Canvas::Pen && !w.menu->isShown(), "tool picked in the menu, menu closes");
        rightClick(c, QPointF(1300, 900));
        pause(250);
        const int strokes = countIn<StrokeItem>(zones);
        c->activeLayer = zones;
        drag(c, QPointF(1300, 850), QPointF(1350, 850));
        check(!w.menu->isShown(), "a click outside closes the menu");
        check(countIn<StrokeItem>(zones) == strokes, "the click that closes the menu draws nothing");
        c->setTool(Canvas::Select);

        key(c, Qt::Key_L, "l");
        pause(300);
        check(w.layersPanel->isShown() && w.layersPanel->isVisible(), "L shows the layers panel");
        w.grab().save(out + "/vaxmaps-layers.png");
        key(c, Qt::Key_L, "l");
        pause(300);
        check(!w.layersPanel->isVisible(), "L again hides the layers");
        w.fullScreenAction->trigger();
        check(w.isFullScreen(), "F11 turns full screen on");
        w.fullScreenAction->trigger();
        check(!w.isFullScreen(), "F11 again goes back to the window");
    }

    c->activeLayer = texts;
    auto *note = new TextItem(true, QColor(246, 229, 141));
    note->setParentItem(texts);
    note->setPlainText("Trap: DC 15");
    note->setPos(1200, 300);
    auto *label = new TextItem(false, Qt::white);
    label->setParentItem(texts);
    label->setPlainText("Entrance");
    label->setPos(120, 120);
    {
        w.activateWindow();
        c->setFocus();
        key(c, Qt::Key_R, "r");
        check(c->tool() == Canvas::ZoneRect, "R picks the rectangle zone");
        c->setTool(Canvas::Select);
        label->startEditing();
        key(c, Qt::Key_E, "e");
        check(label->toPlainText() == "Entrancee" && c->tool() == Canvas::Select, "E while editing types into the text and keeps the tool");
        label->clearFocus();
        label->setPlainText("Entrance");
    }
    maps->setLocked(true);
    drawings->setVisible(false);
    check(!(m1->flags() & QGraphicsItem::ItemIsSelectable), "a locked layer blocks selection");

    // save and reload
    const QString proj = tmp.path() + "/test.vaxmap";
    check(w.saveTo(proj) && !w.dirty, "save project");
    w.newProject();
    check(w.layers.size() == 5 && w.layers[0]->childItems().isEmpty(), "new project is empty");
    check(w.openProject(proj), "open project");
    check(w.layers.size() == 5 && w.layers[0]->locked && !w.layers[2]->isVisible() && w.layers[4]->links, "layers: locked, hidden and Links kept");
    check(countIn<MapItem>(w.layers[0]) == 3 && countIn<ZoneItem>(w.layers[1]) == 3 && countIn<StrokeItem>(w.layers[2]) == 2
              && countIn<TextItem>(w.layers[3]) == 2,
          "items per layer kept");
    MapItem *r1 = nullptr, *r3 = nullptr;
    for (QGraphicsItem *it : w.layers[0]->childItems())
        if (auto *m = qgraphicsitem_cast<MapItem *>(it)) {
            if (m->src->path.endsWith("dungeon.jpg")) r1 = m;
            if (m->src->path.endsWith("surface.png")) r3 = m;
        }
    check(r1 && r3 && std::abs(r1->cal.cellSize() - 100) < 1e-9 && r1->cal.unit == "m" && r3->cal.isHex() && r3->cal.unit == "km" && r1->src->ready,
          "square and hex calibrations kept, tile pyramid reused from the cache");
    QList<LinkItem *> loaded;
    for (QGraphicsItem *it : w.layers[4]->childItems())
        if (it->type() == LinkItem::Type) loaded << static_cast<LinkItem *>(it);
    check(loaded.size() == 2 && loaded[0]->partner == loaded[1] && !linksVisible, "link pair kept, hidden on open");
    auto *rz = qgraphicsitem_cast<ZoneItem *>(w.layers[1]->childItems().value(0));
    check(rz && rz->name == "Throne room" && rz->desc.contains("Secret door"), "zone name and description kept");
    if (loaded.size() == 2 && r3) {
        c->showLinks(true);
        r3->cal.showGrid = true;
        w.layers[2]->setVisible(true);
        w.fitAll();
        pause(300);
        settle(c);
        w.grab().save(out + "/vaxmaps-links.png");
        r3->cal.showGrid = false;
        w.scene->clearSelection();
        loaded[0]->setSelected(true);
        c->deleteSelected();
        int left = 0;
        for (QGraphicsItem *it : w.layers[4]->childItems()) left += it->type() == LinkItem::Type;
        check(left == 0, "deleting one end of a link deletes its pair");
        c->showLinks(false);
    }
    w.fitAll();
    c->resetTransform();
    c->scale(0.6, 0.6);
    c->centerOn(700, 500);
    if (rz) rz->setSelected(true);
    pause(300);
    settle(c);
    w.grab().save(out + "/vaxmaps-window.png");

    regressions(w, tmp.path(), map2);
    additions(w, tmp.path(), map2);
    version06(w, tmp.path(), map1, map2);
    version062(w, tmp.path(), map2);
    version07(w, tmp.path(), map1);
    version08(w, tmp.path(), out);
    version09(w, tmp.path(), map1, out);
    if (args.contains("--bench")) bench(w, tmp.path());
    w.dirty = false;
    say(failures ? QString("\n%1 failure(s)").arg(failures) : "\nAll tests passed.");
    logFile = nullptr;
    return failures ? 1 : 0;
}
