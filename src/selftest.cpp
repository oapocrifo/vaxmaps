// `VaxMaps --selftest [outdir] [--bench]`: assert-based checks of the core logic and of real mouse
// interaction on an offscreen window, plus screenshots in outdir.
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

QString makeMap(const QString &dir, const QString &name, int w, int h, int cell, QPoint origin, bool alpha) {
    QImage img(w, h, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) line[x] = bg(x, y, w, h).rgb();
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
    for (int i = 0; i < 8; ++i) paths << makeMap(dir, QString("grande%1.jpg").arg(i), 6000, 4000, 70, QPoint(0, 0), false);
    w.newProject();
    QElapsedTimer t;
    t.start();
    QList<MapItem *> maps;
    for (int i = 0; i < paths.size(); ++i) maps << w.importMap(paths[i], QPointF((i % 4) * 6200, (i / 4) * 4200));
    waitFor([&] { for (auto *m : maps) if (!m->src->ready) return false; return true; }, 600000);
    say(QString("pirâmides de tiles geradas: %1 s (primeira vez; depois vem do cache em disco)").arg(t.elapsed() / 1000.0, 0, 'f', 1));
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
        const Calibration c = Calibration::fromPoints(QPointF(50, 50), QPointF(1050, 53), 10, 1.5, "m");
        check(qFuzzyCompare(c.cellPx, 100.0) && c.origin == QPointF(50, 50), "calibração por dois pontos (eixo dominante)");
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
    check(m1->src->levels == 4 && m1->src->ext == "jpg", "mapa opaco: 4 níveis, tiles JPEG");
    check(m2->src->ext == "png", "mapa com transparência: tiles PNG");
    const int tiles = QDir(m1->src->dir).entryList({"*.jpg"}).size();
    check(tiles == 24 + 6 + 2, QString("tiles em disco: %1 (esperado 32)").arg(tiles));

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

    // calibration + ruler + snap
    m1->cal = Calibration::fromPoints(QPointF(50, 50), QPointF(1050, 50), 10, 1.5, "m");
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
