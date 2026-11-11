// `VaxMaps --selftest [outdir] [--bench]`: assert-based checks of the core logic and of real mouse
// interaction on an offscreen window, plus screenshots in outdir.
#include "calibration.h"
#include "canvas.h"
#include "items.h"
#include "mainwindow.h"
#include "mapitem.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsScene>
#include <QKeyEvent>
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
    say(QString(ok ? "ok    %1" : "FALHA %1").arg(what));
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

double offBy(double v, double target, double cell) {
    const double r = std::fmod(std::fmod(v - target, cell) + cell, cell);
    return std::min(r, cell - r);
}

void key(Canvas *c, int k, const QString &text = {}) {
    QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier, text);
    QApplication::sendEvent(c, &e);
}

void mouse(Canvas *c, QEvent::Type type, QPointF scenePos, Qt::MouseButton b = Qt::LeftButton, Qt::KeyboardModifiers mods = {}) {
    const QPointF vp = c->mapFromScene(scenePos);
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(b);
    QMouseEvent e(type, vp, c->viewport()->mapToGlobal(vp), type == QEvent::MouseMove ? Qt::NoButton : b, held, mods);
    QApplication::sendEvent(c->viewport(), &e);
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
    *got = QString("%1,%2,%3 esperado %4,%5,%6").arg(g.red()).arg(g.green()).arg(g.blue())
               .arg(expected.red()).arg(expected.green()).arg(expected.blue());
    return std::abs(g.red() - expected.red()) <= tol && std::abs(g.green() - expected.green()) <= tol
           && std::abs(g.blue() - expected.blue()) <= tol;
}

void bench(MainWindow &w, const QString &dir) {
    say("\n== benchmark: 8 mapas de 6000 x 4000 px ==");
    QStringList paths;
    for (int i = 0; i < 8; ++i) paths << makeMap(dir, QString("grande%1.png").arg(i), 6000, 4000, 70, QPoint(0, 0), false, 12);
    w.newProject();
    QElapsedTimer t;
    t.start();
    QList<MapItem *> maps;
    for (int i = 0; i < paths.size(); ++i) maps << w.importMap(paths[i], QPointF((i % 4) * 6200, (i / 4) * 4200));
    waitFor([&] { for (auto *m : maps) if (!m->src->ready) return false; return true; }, 600000);
    qint64 bytes = 0;
    for (auto *m : maps)
        for (const QFileInfo &f : QDir(m->src->dir).entryInfoList(QDir::Files)) bytes += f.size();
    say(QString("pirâmides de tiles geradas: %1 s, %2 MB em disco (primeira vez; depois vem do cache)")
            .arg(t.elapsed() / 1000.0, 0, 'f', 1).arg(bytes / 1048576.0, 0, 'f', 0));
    auto frames = [&](const QString &label) {
        settle(w.canvas);
        QElapsedTimer f;
        f.start();
        for (int i = 0; i < 20; ++i) w.canvas->viewport()->repaint();
        say(QString("%1: %2 ms/quadro, cache de tiles %3 MB").arg(label).arg(f.elapsed() / 20.0, 0, 'f', 1)
                .arg(tileCacheBytes() / 1048576.0, 0, 'f', 0));
    };
    w.fitAll();
    frames(QString("todos os mapas visíveis (zoom %1%)").arg(w.canvas->zoom() * 100, 0, 'f', 1));
    w.canvas->resetTransform();
    w.canvas->scale(0.35, 0.35);
    w.canvas->centerOn(6200, 2000);
    frames("zoom 35%");
    w.canvas->resetTransform();
    w.canvas->centerOn(9000, 2000);
    frames("zoom 100%");
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
    check(MapItem::levelFor(1.0, 4) == 0 && MapItem::levelFor(2.0, 4) == 0, "nível 0 com zoom >= 100%");
    check(MapItem::levelFor(0.5, 4) == 1 && MapItem::levelFor(0.3, 4) == 1 && MapItem::levelFor(0.2, 4) == 2, "nível da pirâmide pelo zoom");
    check(MapItem::levelFor(0.001, 4) == 3, "zoom mínimo usa a visão geral");
    {
        QVector<QPointF> line;
        for (int x = 0; x <= 100; ++x) line << QPointF(x, 0);
        const auto runs = eraseRuns(line, QPointF(50, 0), 10);
        check(runs.size() == 2 && runs[0].size() == 41 && runs[1].size() == 41
                  && runs[0].last() == QPointF(40, 0) && runs[1].first() == QPointF(60, 0),
              "borracha divide traço denso em dois");
        const auto seg = eraseRuns({QPointF(0, 0), QPointF(100, 0)}, QPointF(50, 0), 10);
        check(seg.size() == 2 && seg[0] == QVector<QPointF>{QPointF(0, 0), QPointF(40, 0)}
                  && seg[1] == QVector<QPointF>{QPointF(60, 0), QPointF(100, 0)},
              "borracha corta segmento longo no círculo");
        check(eraseRuns(line, QPointF(50, 30), 10).size() == 1, "borracha longe não altera o traço");
        check(eraseRuns(line, QPointF(50, 0), 200).isEmpty(), "borracha grande apaga o traço todo");
    }
    {
        const GridFit one = gridFromMeasurements({{QPointF(148, 152), QPointF(249, 152)}});
        check(one.cell == 101 && offBy(one.origin.x(), 47, 101) < 1e-6 && offBy(one.origin.y(), 51, 101) < 1e-6,
              "medição de 1 quadrado: tamanho e canto");
        const GridFit wrap = gridFromMeasurements({{QPointF(0.5, 10), QPointF(100.5, 10)}, {QPointF(299.5, 10), QPointF(399.5, 10)}});
        check(wrap.cell == 100 && offBy(wrap.origin.x(), 0, 100) < 0.01, "média circular dos cantos (0,5 e 99,5 dão 0)");

        struct Case { const char *name; int w, h; double cell; QPointF origin; int alpha, noise; double hint; };
        for (const Case &k : {Case{"grade nítida", 3000, 2000, 70.37, QPointF(23.4, 41.9), 200, 25, 66},
                              Case{"grade fraca em textura", 3000, 2000, 88.6, QPointF(40.2, 12.7), 70, 45, 93},
                              Case{"mapa grande reduzido para análise", 9000, 6000, 141.3, QPointF(60.2, 17.7), 200, 25, 135}}) {
            const QImage img = gridImage(k.w, k.h, k.cell, k.origin, k.alpha, k.noise);
            const QImage small = std::max(k.w, k.h) > 4096 ? img.scaled(4096, 4096, Qt::KeepAspectRatio, Qt::SmoothTransformation) : img;
            const LineProfiles prof = lineProfiles(small, img.size());
            const double tolCell = 0.03 * k.cell / 70, tolPos = 0.6 / prof.sx;
            for (auto [how, g] : {std::pair{"ajuste à medição", fitGrid(prof, k.hint)}, std::pair{"detecção automática", detectGrid(prof)}}) {
                const bool ok = g && std::abs(g->cell - k.cell) < tolCell && offBy(g->origin.x(), k.origin.x(), k.cell) < tolPos
                                && offBy(g->origin.y(), k.origin.y(), k.cell) < tolPos;
                check(ok, QString("calibração, %1, %2: %3").arg(k.name, how,
                      g ? QString("quadrado %1 px (real %2), canto %3,%4 (real %5,%6), força %7")
                              .arg(g->cell, 0, 'f', 3).arg(k.cell).arg(g->origin.x(), 0, 'f', 2).arg(g->origin.y(), 0, 'f', 2)
                              .arg(k.origin.x()).arg(k.origin.y()).arg(g->strength, 0, 'f', 2)
                        : QString("nada encontrado")));
            }
        }
        const LineProfiles none = lineProfiles(gridImage(3000, 2000, 1, {}, 0, 45), QSize(3000, 2000));
        const auto g = detectGrid(none);
        check(!g, QString("mapa sem grade: detecção não inventa grade%1").arg(g ? QString(" (achou %1 px, força %2)").arg(g->cell).arg(g->strength) : ""));
    }

    // ---- app ----
    const QString map1 = makeMap(tmp.path(), "masmorra.jpg", 3000, 2000, 100, QPoint(50, 50), false);
    const QString map2 = makeMap(tmp.path(), "torre.png", 1600, 1200, 64, QPoint(0, 0), true);
    MainWindow w;
    w.show();
    Canvas *c = w.canvas;
    MapItem *m1 = w.importMap(map1, QPointF(1500, 1000));
    MapItem *m2 = w.importMap(map2, QPointF(4500, 600));
    check(m1 && m2, "importar mapas JPG e PNG");
    if (!m1 || !m2) return 1;
    check(waitFor([&] { return m1->src->ready && m2->src->ready; }, 120000), "pirâmide de tiles gerada em segundo plano");
    check(m1->pos() == QPointF(0, 0), "mapa centralizado no ponto de importação");
    check(m1->src->levels == 4 && m1->src->ext == "png" && m2->src->ext == "png", "4 níveis, tiles PNG");
    const int tiles = QDir(m1->src->dir).entryList({"?_*_*.png"}).size();
    check(tiles == 24 + 6 + 2, QString("tiles em disco: %1 (esperado 32)").arg(tiles));
    {
        const QImage source(map1), tile(m1->src->dir + "/0_1_1.png");   // content 510..1019 plus 1 px border from 509
        bool same = tile.size() == QSize(TILE + 2, TILE + 2);
        for (int y = 0; same && y < tile.height(); ++y)
            for (int x = 0; same && x < tile.width(); ++x) same = tile.pixel(x, y) == source.pixel(509 + x, 509 + y);
        check(same, "tile idêntico à imagem original pixel a pixel (sem perdas)");
    }

    // rendering: tile placement compared with the source image at several zoom levels
    for (double z : {1.0, 0.73, 0.3, 0.12}) {
        c->resetTransform();
        c->scale(z, z);
        c->centerOn(m1->mapToScene(QPointF(1020, 1020)));   // a tile corner at level 0 and 1
        settle(c);
        QString got;
        bool ok = true;
        for (QPointF p : {QPointF(1000, 1000), QPointF(1021, 1018), QPointF(510, 1000), QPointF(1100, 1100)}) {
            // sample cell centres, away from the drawn grid lines
            const QPointF centre(std::floor((p.x() - 50) / 100) * 100 + 100, std::floor((p.y() - 50) / 100) * 100 + 100);
            ok &= pixelMatches(c, m1, centre, bg(int(centre.x()), int(centre.y()), 3000, 2000), 14, &got);
        }
        check(ok, QString("render no zoom %1%: cor confere com a imagem original (%2)").arg(z * 100).arg(got));
    }

    // calibration through the tool, as a user does it
    w.scene->clearSelection();
    m1->setSelected(true);
    c->setTool(Canvas::Calibrate);
    Calibrator *cal = c->calibrator;
    check(cal->target() == m1 && cal->isVisible(), "Calibrar com o mapa selecionado abre o painel nele");
    mouse(c, QEvent::MouseButtonPress, m1->mapToScene(QPointF(2400, 300)));   // outside the top-left quarter
    mouse(c, QEvent::MouseButtonRelease, m1->mapToScene(QPointF(2400, 300)));
    mouse(c, QEvent::MouseButtonPress, m1->mapToScene(QPointF(152, 148)));    // ~2 px off the corner (150, 150)
    mouse(c, QEvent::MouseButtonRelease, m1->mapToScene(QPointF(152, 148)));
    mouse(c, QEvent::MouseButtonPress, m1->mapToScene(QPointF(247, 153)));
    mouse(c, QEvent::MouseButtonRelease, m1->mapToScene(QPointF(247, 153)));
    waitFor([&] { return !cal->busy(); }, 30000);
    const auto r = cal->current();
    check(r && std::abs(r->cell - 100) < 0.05 && offBy(r->origin.x(), 50.5, 100) < 1 && offBy(r->origin.y(), 50.5, 100) < 1 && m1->preview,
          r ? QString("1 quadrado medido com 2 px de erro, ajustado às linhas: %1 px, canto %2,%3").arg(r->cell, 0, 'f', 3)
                  .arg(r->origin.x(), 0, 'f', 2).arg(r->origin.y(), 0, 'f', 2)
            : QString("medição não gerou grade"));
    settle(c);
    w.grab().save(out + "/vaxmaps-calibracao.png");
    key(c, Qt::Key_Return);
    check(m1->cal.valid() && std::abs(m1->cal.cellPx - 100) < 0.05 && !m1->preview && !m1->cal.showGrid && c->tool() == Canvas::Select
              && !cal->isVisible(),
          "Enter aplica: calibração salva, grade some, painel fecha");
    m1->setSelected(true);
    c->setTool(Canvas::Calibrate);
    key(c, Qt::Key_F, "f");
    waitFor([&] { return !cal->busy(); }, 60000);
    const auto d = cal->current();
    check(d && std::abs(d->cell - 100) < 0.05, d ? QString("detecção automática no mapa: %1 px").arg(d->cell, 0, 'f', 3) : QString("detecção automática falhou"));
    const double before = m1->cal.cellPx;
    key(c, Qt::Key_Escape);
    check(m1->cal.cellPx == before && !m1->preview && c->tool() == Canvas::Select, "Esc cancela sem mudar a calibração");

    // ruler + snap on a known calibration
    m1->cal = Calibration{100, QPointF(50, 50), 1.5, "m"};
    const QString t = measureText(w.scene, m1->mapToScene(QPointF(50, 50)), m1->mapToScene(QPointF(350, 450)));
    check(t == "7,5 m · 5 quadrados", "régua: " + t);
    check(snapPoint(w.scene, m1->mapToScene(QPointF(148, 61))) == m1->mapToScene(QPointF(150, 50)), "snap na interseção mais próxima");
    check(measureText(w.scene, QPointF(-500, -500), QPointF(-200, -100)).startsWith("500 px"), "régua fora de mapa calibrado mede em px");

    // mouse interaction through the real event path
    c->resetTransform();
    c->centerOn(1000, 600);
    LayerItem *maps = w.layers[0], *zones = w.layers[1], *drawings = w.layers[2], *texts = w.layers[3];
    c->activeLayer = drawings;
    c->setTool(Canvas::Pen);
    drag(c, QPointF(600, 300), QPointF(1000, 300), 40);
    check(countIn<StrokeItem>(drawings) == 1, "caneta cria um traço");
    c->setTool(Canvas::Eraser);
    drag(c, QPointF(800, 290), QPointF(800, 310), 4);
    check(countIn<StrokeItem>(drawings) == 2, "borracha no meio divide o traço em dois");

    c->activeLayer = zones;
    c->setTool(Canvas::ZoneRect);
    drag(c, QPointF(200, 600), QPointF(400, 700));
    c->setTool(Canvas::ZoneEllipse);
    drag(c, QPointF(500, 600), QPointF(650, 680), 6);
    snapEnabled = true;
    c->setTool(Canvas::ZonePoly);
    for (QPointF p : {QPointF(752, 548), QPointF(948, 553), QPointF(851, 748)}) {
        mouse(c, QEvent::MouseButtonPress, p);
        mouse(c, QEvent::MouseButtonRelease, p);
    }
    mouse(c, QEvent::MouseButtonDblClick, QPointF(851, 748));
    mouse(c, QEvent::MouseButtonRelease, QPointF(851, 748));
    snapEnabled = false;
    QList<ZoneItem *> zs;
    for (QGraphicsItem *it : zones->childItems()) if (auto *z = qgraphicsitem_cast<ZoneItem *>(it)) zs << z;
    check(zs.size() == 3, QString("zonas criadas: %1 (retângulo, círculo, vértices)").arg(zs.size()));
    if (zs.size() == 3) {
        check(zs[2]->shapeKind == ZoneItem::Poly && zs[2]->pts.size() == 3
                  && zs[2]->pts[0] == m1->mapToScene(QPointF(750, 550)) && zs[2]->pts[2] == m1->mapToScene(QPointF(850, 750)),
              "polígono com 3 vértices presos na grade");
        check(zs[2]->isSelected() && c->tool() == Canvas::Select, "zona nova fica selecionada");
        zs[0]->name = "Sala do trono";
        zs[0]->setDesc("Dois guardas.\nPorta secreta ao norte.");
        zs[1]->name = "Poço";
        zs[1]->color = QColor(40, 120, 255);
    }
    c->setTool(Canvas::Measure);
    drag(c, m1->mapToScene(QPointF(50, 50)), m1->mapToScene(QPointF(650, 50)), 3);
    settle(c);
    c->viewport()->grab().save(out + "/vaxmaps-ferramentas.png");

    c->activeLayer = texts;
    auto *note = new TextItem(true, QColor(246, 229, 141));
    note->setParentItem(texts);
    note->setPlainText("Armadilha: CD 15");
    note->setPos(1200, 300);
    auto *label = new TextItem(false, Qt::white);
    label->setParentItem(texts);
    label->setPlainText("Entrada");
    label->setPos(120, 120);
    {
        w.activateWindow();
        c->setFocus();
        QKeyEvent r(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier, "r");
        QApplication::sendEvent(c, &r);
        check(c->tool() == Canvas::ZoneRect, "tecla R escolhe a zona retângulo");
        c->setTool(Canvas::Select);
        label->startEditing();
        QKeyEvent k(QEvent::KeyPress, Qt::Key_E, Qt::NoModifier, "e");
        QApplication::sendEvent(c, &k);
        check(label->toPlainText() == "Entradae" && c->tool() == Canvas::Select, "tecla E durante a edição escreve no texto e não troca a ferramenta");
        label->clearFocus();
        label->setPlainText("Entrada");
    }
    maps->setLocked(true);
    drawings->setVisible(false);
    check(!(m1->flags() & QGraphicsItem::ItemIsSelectable), "camada travada impede seleção");

    // save and reload
    const QString proj = tmp.path() + "/teste.vaxmap";
    check(w.saveTo(proj) && !w.dirty, "salvar projeto");
    w.newProject();
    check(w.layers.size() == 4 && w.layers[0]->childItems().isEmpty(), "novo projeto limpo");
    check(w.openProject(proj), "abrir projeto");
    check(w.layers.size() == 4 && w.layers[0]->locked && !w.layers[2]->isVisible(), "camadas: travada e oculta preservadas");
    check(countIn<MapItem>(w.layers[0]) == 2 && countIn<ZoneItem>(w.layers[1]) == 3 && countIn<StrokeItem>(w.layers[2]) == 2
              && countIn<TextItem>(w.layers[3]) == 2,
          "itens por camada preservados");
    MapItem *r1 = nullptr;
    for (QGraphicsItem *it : w.layers[0]->childItems())
        if (auto *m = qgraphicsitem_cast<MapItem *>(it); m && m->src->path.endsWith("masmorra.jpg")) r1 = m;
    check(r1 && qFuzzyCompare(r1->cal.cellPx, 100.0) && r1->cal.unit == "m" && r1->src->ready, "calibração preservada, pirâmide reaproveitada do cache");
    auto *rz = qgraphicsitem_cast<ZoneItem *>(w.layers[1]->childItems().value(0));
    check(rz && rz->name == "Sala do trono" && rz->desc.contains("Porta secreta"), "nome e descrição da zona preservados");
    w.fitAll();
    c->resetTransform();
    c->scale(0.6, 0.6);
    c->centerOn(700, 500);
    if (rz) rz->setSelected(true);
    settle(c);
    w.grab().save(out + "/vaxmaps-janela.png");

    if (args.contains("--bench")) bench(w, tmp.path());
    w.dirty = false;
    say(failures ? QString("\n%1 falha(s)").arg(failures) : "\nTodos os testes passaram.");
    logFile = nullptr;
    return failures ? 1 : 0;
}
