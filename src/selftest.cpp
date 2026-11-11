// `VaxMaps --selftest [outdir] [--bench]`: assert-based checks of the core logic and of real mouse
// interaction on an offscreen window, plus screenshots in outdir.
#include "calibration.h"
#include "canvas.h"
#include "items.h"
#include "mainwindow.h"
#include "mapitem.h"
#include "ui.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsScene>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
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
        check(!none, QString("map without a grid: detection invents none%1").arg(none ? QString(" (found %1 px)").arg(none->cal.cellSize()) : ""));
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

    if (args.contains("--bench")) bench(w, tmp.path());
    w.dirty = false;
    say(failures ? QString("\n%1 failure(s)").arg(failures) : "\nAll tests passed.");
    logFile = nullptr;
    return failures ? 1 : 0;
}
