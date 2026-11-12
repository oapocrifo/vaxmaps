#include "canvas.h"
#include "calibration.h"
#include "items.h"
#include "lang.h"
#include "mapitem.h"

#include <QContextMenuEvent>
#include <QMap>
#include <QSet>
#include <QMimeData>
#include <QMouseEvent>
#include <QImageReader>
#include <QGuiApplication>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QUuid>
#include <QWheelEvent>
#include <cmath>
#include <queue>

bool snapEnabled = false;

namespace {

QString fmt(double v) {
    QString s = QString::number(v, 'f', 2);
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s.chop(1);
    return portuguese() ? s.replace('.', ',') : s;
}

QString toolHint(Canvas::Tool t) {
    switch (t) {
    case Canvas::Select: return T("Select: click or drag to select, drag to move. Del deletes. Lock the maps layer so maps don't move by accident.");
    case Canvas::Pan: return T("Pan: drag. The middle button or Space + drag also pan.");
    case Canvas::Pen: return T("Pen: drag to draw.");
    case Canvas::Eraser: return T("Eraser: drag over strokes.");
    case Canvas::Text: return T("Text: click to type. Double click edits existing text.");
    case Canvas::Note: return T("Note: click to place a note.");
    case Canvas::Measure: return T("Ruler: drag to measure. Drag again from a ruler's end to add a turn, or elsewhere for another ruler. Esc clears them.");
    case Canvas::Calibrate: return T("Calibrate: follow the panel. Esc cancels.");
    case Canvas::ZoneRect: return T("Rectangle zone: drag. Shift keeps it square.");
    case Canvas::ZoneEllipse: return T("Circle zone: drag. Shift keeps it round.");
    case Canvas::ZonePoly: return T("Vertex zone: click each vertex. Double click, Enter or a click on the first vertex closes it.");
    case Canvas::Link: return T("Link: drag an area, then drag its pair (it can be on another map).");
    case Canvas::Token: return T("Token: click to place one. Dragged, it lands on the grid. Right click it for its name and size.");
    case Canvas::Fog: return T("Fog brush: drag to reveal, Alt + drag to cover. The width slider sets its size. Shift + click reveals a whole room.");
    case Canvas::Wall: return T("Wall: click corner after corner, double click or Enter ends it. Alt places a point off the corners. It blocks token vision.");
    case Canvas::Door: return T("Door: drag from corner to corner. Double click a door to open or close it.");
    case Canvas::AreaSphere: return T("Sphere: drag from a corner to its last square. A switches to cone and line. Esc clears the areas.");
    case Canvas::AreaCone: return T("Cone: drag from a corner to its last square. A switches to line and sphere. Esc clears the areas.");
    case Canvas::AreaLine: return T("Line: drag from your square to its last square. A switches to sphere and cone. Esc clears the areas.");
    }
    return {};
}

// Calibration magnifier: the map under the pointer 4 times closer than the view, with a crosshair, beside the pointer.
class Loupe : public QWidget {
public:
    explicit Loupe(Canvas *c) : QWidget(c->viewport()), canvas(c) {
        setFixedSize(176, 176);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }
    void showAt(QPointF scenePos, QPoint vp) {
        at = scenePos;
        QPoint pos = vp - QPoint(width() + 24, height() + 24);   // above and left of the pointer, so the hand does not cover it
        if (pos.x() < 0) pos.setX(vp.x() + 24);
        if (pos.y() < 0) pos.setY(vp.y() + 24);
        move(pos);
        show();
        raise();
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
        QPainterPath round;
        round.addEllipse(r);
        p.setClipPath(round);
        p.fillRect(r, QColor(36, 37, 41));
        const double z = canvas->zoom() * 4;
        const bool was = syncTiles;
        syncTiles = true;   // the finer tiles it needs load now: it is drawn once per mouse move
        canvas->scene()->render(&p, r, QRectF(at - QPointF(r.width(), r.height()) / (2 * z), r.size() / z));
        syncTiles = was;
        p.setClipping(false);
        const QPointF c = r.center();
        p.setPen(QPen(QColor(255, 210, 60), 1.5));
        for (QPointF d : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)}) p.drawLine(c + 4 * d, c + 16 * d);
        p.setPen(QPen(QColor(255, 255, 255, 110), 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(r);
    }
private:
    Canvas *canvas;
    QPointF at;
};

// Chamfer distance (3 a step, 4 a diagonal step) of each pixel of a w x h grid to the nearest one where `zero` is set.
std::vector<int> chamfer(const std::vector<uchar> &zero, int w, int h) {
    std::vector<int> d(size_t(w) * h);
    for (size_t i = 0; i < d.size(); ++i) d[i] = zero[i] ? 0 : 1 << 28;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int &v = d[size_t(y) * w + x];
            if (x > 0) v = std::min(v, d[size_t(y) * w + x - 1] + 3);
            if (y > 0) {
                v = std::min(v, d[size_t(y - 1) * w + x] + 3);
                if (x > 0) v = std::min(v, d[size_t(y - 1) * w + x - 1] + 4);
                if (x < w - 1) v = std::min(v, d[size_t(y - 1) * w + x + 1] + 4);
            }
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            int &v = d[size_t(y) * w + x];
            if (x < w - 1) v = std::min(v, d[size_t(y) * w + x + 1] + 3);
            if (y < h - 1) {
                v = std::min(v, d[size_t(y + 1) * w + x] + 3);
                if (x < w - 1) v = std::min(v, d[size_t(y + 1) * w + x + 1] + 4);
                if (x > 0) v = std::min(v, d[size_t(y + 1) * w + x - 1] + 4);
            }
        }
    return d;
}

// The map's picture at about 10 px a cell: the overview when it is fine enough, else read again (the last one is kept).
QImage roomPicture(MapItem *m, double *scale) {
    static QString cachedPath;
    static QImage cached;
    const QSize full = m->src->size;
    if (full.isEmpty()) return {};
    const double want = std::min({1.0, 10 / m->cal.cellSize(), 2048.0 / std::max(full.width(), full.height())});
    const QImage &ov = m->src->overview;
    if (!ov.isNull() && ov.width() >= full.width() * want * 0.7) {
        *scale = double(ov.width()) / full.width();
        return ov.convertToFormat(QImage::Format_RGB32);
    }
    const QSize size = (QSizeF(full) * want).toSize();
    if (cachedPath != m->src->path || cached.size() != size) {
        QImageReader r(m->src->path);
        r.setScaledSize(size);
        cached = r.read().convertToFormat(QImage::Format_RGB32);
        cachedPath = m->src->path;
    }
    *scale = cached.isNull() ? 1 : double(cached.width()) / full.width();
    return cached;
}

const QColor linkColours[] = {QColor(60, 200, 255), QColor(255, 150, 40), QColor(170, 110, 255), QColor(80, 220, 120), QColor(255, 90, 160)};

} // namespace

MapItem *mapAt(QGraphicsScene *s, QPointF scenePos, bool calibratedOnly) {
    for (QGraphicsItem *it : s->items(scenePos))
        if (auto *m = qgraphicsitem_cast<MapItem *>(it); m && m->isVisible() && (!calibratedOnly || m->cal.valid()))
            return m;
    return nullptr;
}

QPointF snapPoint(QGraphicsScene *s, QPointF scenePos, bool centre) {
    MapItem *m = mapAt(s, scenePos, true);
    if (!m) return scenePos;
    const QPointF l = m->mapFromScene(scenePos);
    return m->mapToScene(centre ? m->cal.cellCentre(m->cal.cellAt(l)) : m->cal.nearestCorner(l));
}

Span measureSpan(QGraphicsScene *s, QPointF a, QPointF b) {
    MapItem *m = mapAt(s, a, true);
    if (!m) return {};
    const Calibration &c = m->cal;
    Span sp;
    sp.ok = true;
    sp.hex = c.isHex();
    sp.steps = c.steps(c.cellAt(m->mapFromScene(a)), c.cellAt(m->mapFromScene(b)));
    sp.distance = sp.steps * c.unitsPerCell;
    sp.unit = c.unit;
    return sp;
}

QString spanText(const Span &s) {
    if (!s.ok) return T("Map not calibrated (K)");
    const QString noun = T(s.hex ? (s.steps == 1 ? "hex" : "hexes") : (s.steps == 1 ? "square" : "squares"));
    return QString("%1 %2 · %3 %4").arg(fmt(s.steps), noun, fmt(s.distance), s.unit);
}

QString totalText(const QVector<Span> &spans) {
    QMap<QPair<bool, QString>, Span> sums;   // a path may cross a square map and a hex map
    for (const Span &s : spans) {
        if (!s.ok) continue;
        Span &t = sums[{s.hex, s.unit}];
        t.ok = true;
        t.hex = s.hex;
        t.unit = s.unit;
        t.steps += s.steps;
        t.distance += s.distance;
    }
    QStringList parts;
    for (const Span &s : sums) parts << spanText(s);
    return parts.isEmpty() ? spanText(Span{}) : parts.join(" + ");
}

QString measureText(QGraphicsScene *s, QPointF a, QPointF b) { return spanText(measureSpan(s, a, b)); }

void snapToken(TokenItem *t) {
    MapItem *m = t->scene() ? mapAt(t->scene(), t->centre(), true) : nullptr;
    if (!m) return;
    const Calibration &c = m->cal;
    t->setDiameter(c.cellSize() * t->cells * 0.9 * m->scale() / t->scale());
    const QPointF l = m->mapFromScene(t->centre());
    const QPointF target = !c.isHex() && t->cells % 2 == 0 ? c.nearestCorner(l) : c.cellCentre(c.cellAt(l));
    t->setPos(t->pos() + m->mapToScene(target) - t->centre());   // layers have no transform: scene offsets are position offsets
}

QPolygonF visibleFrom(QGraphicsScene *s, QPointF c, double r) {
    const QRectF reach(c.x() - r, c.y() - r, 2 * r, 2 * r);
    QVector<QLineF> walls;
    for (QGraphicsItem *it : s->items(reach))
        if (auto *w = qgraphicsitem_cast<StrokeItem *>(it); w && w->blocksSight() && w->isVisible())
            for (int i = 1; i < w->pts.size(); ++i) {
                const QLineF l(w->mapToScene(w->pts[i - 1]), w->mapToScene(w->pts[i]));
                if (reach.intersects(QRectF(l.p1(), l.p2()).normalized().adjusted(-1, -1, 1, 1))) walls << l;
            }
    if (walls.isEmpty()) return {};
    // Rays every 0.5° for the round edge, and just either side of each wall end so corners are exact.
    std::vector<double> angles;
    for (int i = 0; i < 720; ++i) angles.push_back(i * M_PI / 360);
    for (const QLineF &l : walls)
        for (QPointF e : {l.p1(), l.p2()}) {
            const double a = std::atan2(e.y() - c.y(), e.x() - c.x());
            for (double d : {-1e-4, 0.0, 1e-4}) angles.push_back(std::fmod(a + d + 2 * M_PI, 2 * M_PI));
        }
    std::sort(angles.begin(), angles.end());
    QPolygonF seen;
    for (double a : angles) {
        const QPointF dir(std::cos(a), std::sin(a));
        double t = r;
        for (const QLineF &l : walls) {   // ray c + t·dir against segment p1 + u·(p2 − p1)
            const QPointF e = l.p2() - l.p1(), w = l.p1() - c;
            const double den = dir.x() * e.y() - dir.y() * e.x();
            if (std::abs(den) < 1e-12) continue;
            const double tt = (w.x() * e.y() - w.y() * e.x()) / den, u = (w.x() * dir.y() - w.y() * dir.x()) / den;
            if (tt > 0 && tt < t && u >= 0 && u <= 1) t = tt;
        }
        seen << c + t * dir;
    }
    return seen;
}

QList<MapItem *> revealSight(TokenItem *t, bool commit) {
    MapItem *under = t->scene() && t->sight > 0 ? mapAt(t->scene(), t->centre(), true) : nullptr;
    if (!under || !(under->cal.unitsPerCell > 0)) return {};
    const double r = t->sight / under->cal.unitsPerCell * under->cal.cellSize() * under->scale() / t->scale();
    t->setSightRadius(r);
    QPainterPath area = t->mapToScene(t->sightArea(r));
    if (const QPolygonF seen = visibleFrom(t->scene(), t->centre(), r * t->scale()); !seen.isEmpty()) {
        QPainterPath p;
        p.addPolygon(seen);
        area = area.intersected(p);
    }
    QList<MapItem *> touched;
    for (QGraphicsItem *it : t->scene()->items(area))
        if (auto *m = qgraphicsitem_cast<MapItem *>(it); m && !m->fog.isNull() && m->isVisible()) {
            m->clearFog(m->mapFromScene(area));
            if (commit) m->fogStrokeDone();
            touched << m;
        }
    t->update();
    if (commit && !touched.isEmpty()) sceneEdited();
    return touched;
}

Canvas::Canvas(QGraphicsScene *scene, QWidget *parent) : QGraphicsView(scene, parent) {
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    // Cosmetic pens and screen-sized labels have no fixed scene bounds; full repaints avoid leftover pixels.
    setViewportUpdateMode(FullViewportUpdate);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setResizeAnchor(AnchorViewCenter);
    setDragMode(RubberBandDrag);
    setBackgroundBrush(QColor(36, 37, 41));
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    eraserRing = scene->addEllipse(QRectF(), QPen(QColor(255, 255, 255, 180), 0));
    eraserRing->setZValue(1e6);
    eraserRing->hide();
    calibrator = new Calibrator(this);
    loupe = new Loupe(this);
    camera.setEasingCurve(QEasingCurve::InOutCubic);
    connect(&camera, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { centerOn(v.toPointF()); });
}

bool Canvas::alive(QGraphicsItem *it) const { return it && scene()->items().contains(it); }

void Canvas::setTool(Tool t) {
    cancelDrafts();
    calibrator->stop();
    currentTool = t;
    setDragMode(t == Select ? RubberBandDrag : NoDrag);
    eraserRing->setVisible(false);
    fogging = false;
    if (t == Link && !linksVisible) showLinks(true);
    updateCursor();
    emit toolChanged(t);
    // The whole hint the first 3 times a tool is picked, then only its name and key: the toast stays out of the way.
    QSettings settings;
    const QString seen = QString("hints/%1").arg(int(t));
    const int times = settings.value(seen, 0).toInt();
    const char keys[] = "VHPETNMKROGJBFWDAAA";   // order of Canvas::Tool
    if (times < 3) settings.setValue(seen, times + 1);
    emit status(times < 3 ? toolHint(t) : QString("%1 · %2").arg(toolHint(t).section(':', 0, 0)).arg(QChar(keys[t])));
    if (t == Calibrate) {
        const QList<QGraphicsItem *> sel = scene()->selectedItems();
        calibrator->start(sel.size() == 1 ? qgraphicsitem_cast<MapItem *>(sel[0]) : nullptr);
    }
}

void Canvas::showLinks(bool on) {
    if (alive(hoveredLink)) hoveredLink->setHover(false);
    hoveredLink = nullptr;
    setLinksVisible(scene(), on);
    emit linksShown(on);
}

void Canvas::updateCursor() {
    if (panning) viewport()->setCursor(Qt::ClosedHandCursor);
    else if (currentTool == Pan || spaceDown) viewport()->setCursor(Qt::OpenHandCursor);
    else if (currentTool == Select) hoveredLink ? viewport()->setCursor(Qt::PointingHandCursor) : viewport()->unsetCursor();
    else viewport()->setCursor(Qt::CrossCursor);
}

void Canvas::cancelDrafts() {
    delete draft;
    draft = nullptr;
    delete wallDraft;
    wallDraft = nullptr;
    dragArea = -1;
    stroke = nullptr;
    erasing = false;
    dragRuler = -1;
    pressedLink = nullptr;
    if (alive(hoveredLink)) hoveredLink->setHover(false);
    hoveredLink = nullptr;
}

bool Canvas::layerUsable() {
    if (activeLayer && activeLayer->links) {
        emit status(T("The Links layer only holds links. Pick another layer in the Layers panel (L)."));
        return false;
    }
    if (activeLayer && activeLayer->isVisible() && !activeLayer->locked) return true;
    emit status(T("The active layer is hidden or locked. Pick another one in the Layers panel (L)."));
    return false;
}

QPointF Canvas::snapped(QPointF p, bool centre) const { return snapEnabled ? snapPoint(scene(), p, centre) : p; }

void Canvas::zoomBy(double f) {
    const double z = zoom() * f;
    // Fit all can leave the zoom outside 0.5 %..6400 %: only steps that go further out are refused.
    if ((f < 1 && z < 0.005) || (f > 1 && z > 64)) return;
    scale(f, f);
    emit zoomChanged(zoom());
}

void Canvas::wheelEvent(QWheelEvent *e) {
    camera.stop();
    // The scene point under the cursor stays there. Not AnchorUnderMouse: it uses the last mouse move the view
    // handled, which is the one before a pan, so the first zoom after a pan shifted the view by the pan distance.
    const QPoint vp = e->position().toPoint();
    const QPointF anchor = mapToScene(vp);
    setTransformationAnchor(NoAnchor);
    zoomBy(std::pow(1.0015, e->angleDelta().y()));
    const QPoint d = mapFromScene(anchor) - vp;
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + d.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() + d.y());
}

LinkItem *Canvas::linkAt(QPoint vp) const {
    for (QGraphicsItem *it : items(vp))
        if (it->type() == LinkItem::Type) return static_cast<LinkItem *>(it);
    return nullptr;
}

// The end still waiting for its pair. Looked up, not remembered: it is also found after the project is reopened.
LinkItem *Canvas::unpairedLink(const LinkItem *except) const {
    for (QGraphicsItem *it : linksLayer->childItems())
        if (it->type() == LinkItem::Type && it != except && !static_cast<LinkItem *>(it)->partner) return static_cast<LinkItem *>(it);
    return nullptr;
}

void Canvas::mousePressEvent(QMouseEvent *e) {
    const QPoint vp = e->position().toPoint();
    const QPointF sp = mapToScene(vp);
    camera.stop();
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (currentTool == Pan || spaceDown))) {
        panning = true;
        lastPan = vp;
        updateCursor();
        return;
    }
    if (e->button() == Qt::RightButton) {
        // The menu opens on the context-menu event. The view would clear the selection on this press, and the
        // menu's Selection commands (delete, move to the active layer) would have nothing left to act on.
        QGraphicsItem *it = itemAt(vp);
        if (it && it->type() == HandleItem::Type) QGraphicsView::mousePressEvent(e);   // removes the vertex
        else if (it != scene()->focusItem()) scene()->setFocusItem(nullptr);           // ends a text edit, as any click outside it
        return;
    }
    if (e->button() == Qt::LeftButton && currentTool == Select && !linksVisible) {
        if (LinkItem *l = linkAt(vp)) {   // hidden links answer clicks: the release jumps
            pressedLink = l;
            pressedLinkAt = vp;
            return;
        }
    }
    if (e->button() != Qt::LeftButton || currentTool == Select) {
        QGraphicsView::mousePressEvent(e);
        if (currentTool == Select)
            for (QGraphicsItem *i : scene()->selectedItems()) pressPositions[i] = i->pos();
        return;
    }
    switch (currentTool) {
    case Pen:
        if (!layerUsable()) return;
        stroke = new StrokeItem(color, penWidth / zoom());
        stroke->setParentItem(activeLayer);
        stroke->addPoint(sp);
        return;
    case Eraser:
        erasing = true;
        erasedAny = false;
        eraseAt(sp);
        return;
    case Text:
    case Note: {
        if (qgraphicsitem_cast<TextItem *>(itemAt(vp))) { QGraphicsView::mousePressEvent(e); return; }
        if (!layerUsable()) return;
        auto *t = new TextItem(currentTool == Note, currentTool == Note ? QColor(246, 229, 141) : color);
        t->setParentItem(activeLayer);
        t->setScale(1 / zoom());   // starts at a readable screen size
        t->setPos(sp);
        setTool(Select);
        scene()->clearSelection();
        t->setSelected(true);
        t->startEditing();
        sceneEdited();
        return;
    }
    case Measure: {
        QPointF p = snapped(sp, true);
        for (QGraphicsItem *it : items(vp))   // a ruler started on a token measures from its centre
            if (auto *k = qgraphicsitem_cast<TokenItem *>(it)) { p = k->centre(); break; }
        dragRuler = -1;
        for (int i = 0; i < rulers.size(); ++i)   // grabbing a ruler's end adds a turn to it
            if (QLineF(mapFromScene(rulers[i].pts.last()), vp).length() < 14) dragRuler = i;
        if (dragRuler < 0) {
            Ruler r;
            QPen pen(QColor(255, 210, 60), 2.5);
            pen.setCosmetic(true);
            pen.setCapStyle(Qt::RoundCap);
            r.line = scene()->addPath(QPainterPath(), pen);
            r.line->setZValue(1e6);
            r.pts << p;
            rulers << r;
            dragRuler = int(rulers.size()) - 1;
        }
        rulers[dragRuler].pts << p;
        updateRuler(rulers[dragRuler]);
        return;
    }
    case Calibrate:
        if (!calibrator->wantsPoint()) { calibrator->mousePress(sp); return; }
        marking = true;   // the point is taken where the button is released: the loupe helps put it there
        static_cast<Loupe *>(loupe)->showAt(sp, vp);
        return;
    case Wall:
    case Door: {
        if (!layerUsable()) return;
        const QPointF p = e->modifiers() & Qt::AltModifier ? sp : snapPoint(scene(), sp);
        if (wallDraft) {   // the next corner of a wall
            QVector<QPointF> pts = wallDraft->pts;
            pts.last() = p;
            pts << p;
            wallDraft->setPoints(pts);
            return;
        }
        wallDraft = new StrokeItem(color, penWidth / zoom());
        wallDraft->wall = true;
        wallDraft->door = currentTool == Door;
        wallDraft->setParentItem(activeLayer);
        wallDraft->setPoints({p, p});
        return;
    }
    case AreaSphere:
    case AreaCone:
    case AreaLine: {
        MapItem *m = mapAt(scene(), sp, true);
        if (!m) { emit status(T("Map not calibrated (K)")); return; }
        areaMap = m;
        areaOrigin = m->mapFromScene(snapPoint(scene(), sp, currentTool == AreaLine || m->cal.isHex()));
        Area a;
        a.cells = scene()->addPath(QPainterPath());
        a.cells->setZValue(1e6 - 1);
        a.label = new LabelItem;
        scene()->addItem(a.label);
        areas << a;
        dragArea = int(areas.size()) - 1;
        updateArea(sp);
        return;
    }
    case Token: {
        if (!layerUsable()) return;
        int n = 0;
        for (QGraphicsItem *it : activeLayer->childItems()) n += it->type() == TokenItem::Type;
        auto *k = new TokenItem(QString::number(n + 1), color, 40 / zoom(), 1);   // 40 px on screen where no map is calibrated
        k->setParentItem(activeLayer);
        k->setPos(sp);
        snapToken(k);
        revealSight(k);
        scene()->clearSelection();
        k->setSelected(true);
        sceneEdited();
        return;
    }
    case Fog:
        if (e->modifiers() & Qt::ShiftModifier) { revealRoom(sp, e->modifiers() & Qt::AltModifier); return; }
        fogging = true;
        fogCover = e->modifiers() & Qt::AltModifier;
        fogLast = sp;
        fogTouched.clear();
        fogTo(sp);
        return;
    case ZoneRect:
    case ZoneEllipse:
    case Link:
        if (currentTool == Link) {
            if (!linksLayer || linksLayer->locked) { emit status(T("The Links layer is locked.")); return; }
        } else if (!layerUsable()) return;
        dragStart = snapped(sp);
        if (currentTool == Link) {
            const LinkItem *pending = unpairedLink();
            int n = 0;
            for (QGraphicsItem *it : linksLayer->childItems()) n += it->type() == LinkItem::Type;
            draft = new LinkItem(QRectF(dragStart, dragStart), pending ? pending->color : linkColours[(n / 2) % 5]);
            draft->setParentItem(linksLayer);
        } else {
            draft = new ZoneItem(currentTool == ZoneRect ? ZoneItem::Rect : ZoneItem::Ellipse, {dragStart, dragStart}, color);
            draft->setParentItem(activeLayer);
        }
        return;
    case ZonePoly: {
        if (!layerUsable()) return;
        const QPointF p = snapped(sp);
        if (!draft) {
            draft = new ZoneItem(ZoneItem::Poly, {p, p}, color);
            draft->setParentItem(activeLayer);
            return;
        }
        QPolygonF pts = draft->pts;
        const double toFirst = QLineF(mapFromScene(pts.first()), vp).length();
        if (pts.size() >= 4 && toFirst < 10) { finishPoly(); return; }
        pts.last() = p;
        pts << p;
        draft->setPoints(pts);
        return;
    }
    default:
        QGraphicsView::mousePressEvent(e);
    }
}

void Canvas::mouseMoveEvent(QMouseEvent *e) {
    const QPoint vp = e->position().toPoint();
    const QPointF sp = mapToScene(vp);
    if (panning) {
        const QPoint d = vp - lastPan;
        lastPan = vp;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        return;
    }
    if (currentTool == Select && !linksVisible && !(e->buttons() & Qt::LeftButton)) {
        LinkItem *l = linkAt(vp);
        if (l != hoveredLink) {
            if (alive(hoveredLink)) hoveredLink->setHover(false);
            hoveredLink = l;
            if (l) l->setHover(true);
            updateCursor();
        }
    }
    if (pressedLink) return;
    if (marking) { static_cast<Loupe *>(loupe)->showAt(sp, vp); return; }
    if (wallDraft) {
        QVector<QPointF> pts = wallDraft->pts;
        pts.last() = e->modifiers() & Qt::AltModifier ? sp : snapPoint(scene(), sp);
        wallDraft->setPoints(pts);
        return;
    }
    if (dragArea >= 0) { updateArea(sp); return; }
    if (currentTool == Fog) {
        const double r = fogWidth() / 2;
        eraserRing->setRect(QRectF(sp - QPointF(r, r), QSizeF(2 * r, 2 * r)));
        eraserRing->show();
        if (fogging) fogTo(sp);
        return;
    }
    if (currentTool == Eraser) {
        const double r = 12 / zoom();
        eraserRing->setRect(QRectF(sp - QPointF(r, r), QSizeF(2 * r, 2 * r)));
        eraserRing->show();
    }
    if (stroke) {
        const QPointF l = stroke->mapFromScene(sp);
        if (QLineF(stroke->pts.last(), l).length() * zoom() >= 1.5) stroke->addPoint(l);
        return;
    }
    if (erasing) { eraseAt(sp); return; }
    if (dragRuler >= 0) {
        rulers[dragRuler].pts.last() = snapped(sp, true);
        updateRuler(rulers[dragRuler]);
        return;
    }
    if (draft && draft->shapeKind != ZoneItem::Poly && (e->buttons() & Qt::LeftButton)) {
        QPointF p = snapped(sp);
        if (e->modifiers() & Qt::ShiftModifier) {
            const QPointF d = p - dragStart;
            const double side = qMax(std::abs(d.x()), std::abs(d.y()));
            p = dragStart + QPointF(d.x() < 0 ? -side : side, d.y() < 0 ? -side : side);
        }
        draft->setPoints({dragStart, p});
        return;
    }
    if (draft && draft->shapeKind == ZoneItem::Poly) {
        QPolygonF pts = draft->pts;
        pts.last() = snapped(sp);
        draft->setPoints(pts);
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
    if (currentTool == Select && (e->buttons() & Qt::LeftButton))   // tokens being dragged see along the way
        for (auto it = pressPositions.cbegin(); it != pressPositions.cend(); ++it)
            if (auto *k = qgraphicsitem_cast<TokenItem *>(it.key()); k && k->sight > 0 && alive(k) && k->pos() != it.value())
                for (MapItem *m : revealSight(k, false))
                    if (!sightTouched.contains(m)) sightTouched << m;
}

void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    const QPoint vp = e->position().toPoint();
    if (panning) { panning = false; updateCursor(); return; }
    if (pressedLink) {
        LinkItem *l = pressedLink;
        pressedLink = nullptr;
        if ((vp - pressedLinkAt).manhattanLength() < 6 && alive(l)) travel(l, mapToScene(vp));
        return;
    }
    if (stroke) { stroke = nullptr; sceneEdited(); return; }
    if (marking) {
        marking = false;
        loupe->hide();
        calibrator->mousePress(mapToScene(vp));
        return;
    }
    if (wallDraft && wallDraft->door) {
        StrokeItem *d = wallDraft;
        wallDraft = nullptr;
        if (QLineF(d->pts.first(), d->pts.last()).length() * zoom() < 4) { delete d; return; }   // a click, not a drag
        sceneEdited();
        return;
    }
    if (wallDraft) return;   // a wall ends with a double click or Enter
    if (dragArea >= 0) { dragArea = -1; return; }   // the area stays until Esc
    if (erasing) { erasing = false; if (erasedAny) sceneEdited(); return; }
    if (fogging) {
        fogging = false;
        for (MapItem *m : fogTouched)
            if (alive(m)) m->fogStrokeDone();
        if (!fogTouched.isEmpty()) sceneEdited();
        return;
    }
    if (dragRuler >= 0) {
        Ruler &r = rulers[dragRuler];
        if (QLineF(mapFromScene(r.pts[r.pts.size() - 2]), mapFromScene(r.pts.last())).length() < 3) r.pts.removeLast();
        if (r.pts.size() < 2) deleteRuler(dragRuler);
        else updateRuler(r);
        dragRuler = -1;
        return;
    }
    if (draft && draft->shapeKind != ZoneItem::Poly) {
        ZoneItem *z = draft;
        draft = nullptr;
        if (z->box().width() * zoom() < 4 || z->box().height() * zoom() < 4) { delete z; return; }   // a click, not a drag
        if (z->type() == LinkItem::Type) return finishLink(static_cast<LinkItem *>(z));
        setTool(Select);
        scene()->clearSelection();
        z->setSelected(true);
        sceneEdited();
        return;
    }
    QGraphicsView::mouseReleaseEvent(e);
    if (currentTool == Select) {
        bool moved = false;
        for (auto it = pressPositions.cbegin(); it != pressPositions.cend(); ++it)
            if (alive(it.key()) && it.key()->pos() != it.value()) {
                moved = true;
                if (auto *k = qgraphicsitem_cast<TokenItem *>(it.key())) { snapToken(k); revealSight(k); }
            }
        for (MapItem *m : sightTouched)
            if (alive(m)) m->fogStrokeDone();
        sightTouched.clear();
        if (moved) sceneEdited();
        pressPositions.clear();
    }
}

void Canvas::finishLink(LinkItem *l) {
    l->id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    if (LinkItem *pending = unpairedLink(l)) {
        l->partner = pending;
        pending->partner = l;
        l->name = pending->name;
        setTool(Select);
        scene()->clearSelection();
        l->setSelected(true);
        emit status(T("Linked. With links hidden (Shift+J), a click on either area moves the view to the other."));
    } else {
        int n = 0;
        for (QGraphicsItem *it : linksLayer->childItems()) n += it->type() == LinkItem::Type;
        l->name = T("Link %1").arg((n + 1) / 2);
        emit status(T("Now drag its pair. It can be on another map: pan and zoom first if needed."));
    }
    sceneEdited();
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    if (draft && draft->shapeKind == ZoneItem::Poly) { finishPoly(); return; }
    if (wallDraft && !wallDraft->door) { finishWall(); return; }
    if (currentTool == Select)
        for (QGraphicsItem *it : items(e->position().toPoint()))
            if (auto *d = qgraphicsitem_cast<StrokeItem *>(it); d && d->door) { toggleDoor(d); return; }
    if (linksVisible && currentTool == Select)
        if (LinkItem *l = linkAt(e->position().toPoint())) { travel(l, mapToScene(e->position().toPoint())); return; }
    QGraphicsView::mouseDoubleClickEvent(e);
}

void Canvas::travel(LinkItem *from, QPointF click) {
    LinkItem *to = from->partner;
    if (!to) { emit status(T("This link has no pair yet: drag one with the Link tool (J).")); return; }
    // The clicked spot lands on the matching spot of the pair, at the same place on screen and the same zoom.
    const QRectF a = from->mapRectToScene(from->box()), b = to->mapRectToScene(to->box());
    // a link without area (old projects have them: a click used to make one) sends its centre
    const QPointF rel(a.width() > 0 ? (click.x() - a.left()) / a.width() : 0.5, a.height() > 0 ? (click.y() - a.top()) / a.height() : 0.5);
    const QPointF target(b.left() + rel.x() * b.width(), b.top() + rel.y() * b.height());
    glideTo(mapToScene(viewport()->rect().center()) + (target - click));
    emit status(to->name.isEmpty() ? T("Moved to the linked area.") : "→ " + to->name);
}

void Canvas::glideTo(QPointF sceneCentre) {
    camera.stop();
    camera.setStartValue(mapToScene(viewport()->rect().center()));
    camera.setEndValue(sceneCentre);
    camera.setDuration(650);
    camera.start();
}

void Canvas::finishPoly() {
    ZoneItem *z = draft;
    draft = nullptr;
    QPolygonF pts = z->pts;
    pts.removeLast();   // the vertex following the cursor
    if (pts.size() < 3) { delete z; return; }
    z->setPoints(pts);
    setTool(Select);
    scene()->clearSelection();
    z->setSelected(true);
    sceneEdited();
}

void Canvas::eraseAt(QPointF sp) {
    const double r = 12 / zoom();
    QPainterPath area;
    area.addEllipse(sp, r, r);
    for (QGraphicsItem *it : scene()->items(area, Qt::IntersectsItemBoundingRect)) {   // shape() strokes the whole path per call
        auto *s = qgraphicsitem_cast<StrokeItem *>(it);
        if (!s || !s->isVisible()) continue;
        if (auto *layer = static_cast<LayerItem *>(s->parentItem()); layer && layer->locked) continue;
        const auto runs = eraseRuns(s->pts, s->mapFromScene(sp), r + s->pen().widthF() / 2);
        if (runs.size() == 1 && runs[0] == s->pts) continue;
        erasedAny = true;
        if (runs.isEmpty()) { delete s; continue; }
        s->setPoints(runs[0]);
        for (int i = 1; i < runs.size(); ++i) {
            auto *n = new StrokeItem(s->pen().color(), s->pen().widthF());
            n->wall = s->wall;   // a wall cut in two is two walls
            n->door = s->door;
            n->open = s->open;
            n->setParentItem(s->parentItem());
            n->setPos(s->pos());
            n->setPoints(runs[i]);
        }
    }
}

void Canvas::fogTo(QPointF sp) {
    const double w = fogWidth();
    const QRectF reach = QRectF(fogLast, sp).normalized().adjusted(-w, -w, w, w);
    for (QGraphicsItem *it : scene()->items(reach)) {
        auto *m = qgraphicsitem_cast<MapItem *>(it);
        if (!m || !m->isVisible()) continue;
        if (m->fog.isNull()) {
            if (!fogCover) continue;   // revealing a map without fog does nothing; covering gives it fog
            m->enableFog(false);
            if (m->fog.isNull()) continue;
        }
        m->paintFog(m->mapFromScene(fogLast), m->mapFromScene(sp), w / m->scale(), fogCover);
        if (!fogTouched.contains(m)) fogTouched << m;
    }
    fogLast = sp;
}

void Canvas::updateRuler(Ruler &r) {
    QPainterPath path(r.pts[0]);
    for (int i = 1; i < r.pts.size(); ++i) path.lineTo(r.pts[i]);
    r.line->setPath(path);
    qDeleteAll(r.marks);
    r.marks.clear();
    QVector<Span> spans;
    for (int i = 1; i < r.pts.size(); ++i) spans << measureSpan(scene(), r.pts[i - 1], r.pts[i]);
    for (int i = 0; i < r.pts.size(); ++i) {
        auto *dot = scene()->addEllipse(-3.5, -3.5, 7, 7, QPen(QColor(30, 30, 30), 1), QColor(255, 210, 60));
        dot->setFlag(QGraphicsItem::ItemIgnoresTransformations);
        dot->setPos(r.pts[i]);
        dot->setZValue(1e6 + 1);
        r.marks << dot;
        if (i == 0 || (i == r.pts.size() - 1)) continue;
        auto *turn = new LabelItem;   // each turn shows its own segment
        turn->setText(spanText(spans[i - 1]));
        turn->setPos(r.pts[i]);
        turn->setOpacity(0.85);
        scene()->addItem(turn);
        r.marks << turn;
    }
    r.text = spans.size() > 1 ? T("Total: ") + totalText(spans) : spanText(spans[0]);
    auto *end = new LabelItem;
    end->setText(r.text);
    end->setPos(r.pts.last());
    scene()->addItem(end);
    r.marks << end;
}

void Canvas::deleteRuler(int i) {
    qDeleteAll(rulers[i].marks);
    delete rulers[i].line;
    rulers.removeAt(i);
}

void Canvas::clearRulers() {
    while (!rulers.isEmpty()) deleteRuler(0);
    dragRuler = -1;
    for (const Area &a : areas) {
        delete a.cells;
        delete a.label;
    }
    areas.clear();
    dragArea = -1;
}

QString Canvas::rulerText(int i) const { return i >= 0 && i < rulers.size() ? rulers[i].text : QString(); }

void Canvas::deleteSelected() {
    QList<QGraphicsItem *> doomed;
    for (QGraphicsItem *i : scene()->selectedItems()) {
        if (!i->parentItem() || i->parentItem()->type() != LayerItem::Type) continue;
        doomed << i;
        if (i->type() == LinkItem::Type)   // a link goes with its pair
            if (LinkItem *p = static_cast<LinkItem *>(i)->partner; p && !doomed.contains(p)) doomed << p;
    }
    const QSet<QGraphicsItem *> unique(doomed.begin(), doomed.end());
    qDeleteAll(unique);
    if (!unique.isEmpty()) sceneEdited();
}

void Canvas::keyPressEvent(QKeyEvent *e) {
    if (auto *t = qgraphicsitem_cast<TextItem *>(scene()->focusItem()); t && t->editing()) {
        if (e->key() == Qt::Key_Escape) t->clearFocus();
        else QGraphicsView::keyPressEvent(e);
        return;
    }
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) { QGraphicsView::keyPressEvent(e); return; }
    if (currentTool == Calibrate && calibrator->key(e)) return;
    if (e->key() == Qt::Key_J && (e->modifiers() & Qt::ShiftModifier)) {
        showLinks(!linksVisible);
        emit status(T(linksVisible ? "Links shown: select, resize or delete them. Double click one to jump." : "Links hidden: click one to jump to its pair."));
        return;
    }
    if (e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BracketRight || e->key() == Qt::Key_BraceLeft
        || e->key() == Qt::Key_BraceRight) {   // turn the selected cones 15°, 45° with Shift
        const double step = (e->modifiers() & Qt::ShiftModifier ? 45 : 15)
                            * (e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BraceLeft ? -1 : 1);
        for (QGraphicsItem *it : scene()->selectedItems())
            if (auto *k = qgraphicsitem_cast<TokenItem *>(it); k && k->cone) {
                k->facing = std::fmod(k->facing + step + 360, 360);
                revealSight(k);
                sceneEdited();
            }
        return;
    }
    // Tool letters live here instead of QAction shortcuts so typing in a text item never switches tools.
    static const QHash<int, Tool> keys{{Qt::Key_V, Select}, {Qt::Key_H, Pan}, {Qt::Key_P, Pen}, {Qt::Key_E, Eraser},
                                       {Qt::Key_T, Text}, {Qt::Key_N, Note}, {Qt::Key_M, Measure}, {Qt::Key_K, Calibrate},
                                       {Qt::Key_R, ZoneRect}, {Qt::Key_O, ZoneEllipse}, {Qt::Key_G, ZonePoly}, {Qt::Key_J, Link},
                                       {Qt::Key_B, Token}, {Qt::Key_F, Fog}, {Qt::Key_W, Wall}, {Qt::Key_D, Door}};
    if (keys.contains(e->key()) && !(e->modifiers() & Qt::ShiftModifier)) { setTool(keys[e->key()]); return; }
    if (e->key() == Qt::Key_A && !(e->modifiers() & Qt::ShiftModifier)) {   // sphere, cone, line, sphere...
        setTool(currentTool == AreaSphere ? AreaCone : currentTool == AreaCone ? AreaLine : AreaSphere);
        return;
    }
    switch (e->key()) {
    case Qt::Key_L:
        emit toggleLayersRequested();
        return;
    case Qt::Key_S:
        snapEnabled = !snapEnabled;
        emit snapChanged(snapEnabled);
        emit status(T(snapEnabled ? "Snap to grid on: zone points stick to corners, ruler points to cell centres." : "Snap to grid off."));
        return;
    case Qt::Key_Space:
        if (!e->isAutoRepeat()) { spaceDown = true; updateCursor(); }
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        deleteSelected();
        return;
    case Qt::Key_Escape:
        cancelDrafts();
        clearRulers();
        scene()->clearSelection();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (draft && draft->shapeKind == ZoneItem::Poly) finishPoly();
        if (wallDraft && !wallDraft->door) finishWall();
        return;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down: {   // move the selection by screen pixels; with nothing selected the arrows scroll the view
        const QList<QGraphicsItem *> sel = scene()->selectedItems();
        if (sel.isEmpty()) break;
        const double d = ((e->modifiers() & Qt::ShiftModifier) ? 10 : 1) / zoom();
        const int k = e->key();
        for (QGraphicsItem *i : sel) i->moveBy(k == Qt::Key_Left ? -d : k == Qt::Key_Right ? d : 0, k == Qt::Key_Up ? -d : k == Qt::Key_Down ? d : 0);
        sceneEdited();
        return;
    }
    }
    QGraphicsView::keyPressEvent(e);
}

void Canvas::keyReleaseEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) { spaceDown = false; updateCursor(); return; }
    QGraphicsView::keyReleaseEvent(e);
}

void Canvas::focusOutEvent(QFocusEvent *e) {
    spaceDown = false;   // Space released in another window never gets here: pan mode would stay on
    updateCursor();
    QGraphicsView::focusOutEvent(e);
}

void Canvas::contextMenuEvent(QContextMenuEvent *e) {
    QGraphicsItem *it = itemAt(e->pos());
    if (auto *t = qgraphicsitem_cast<TextItem *>(it); t && t->editing()) return QGraphicsView::contextMenuEvent(e);   // copy/paste menu
    if (it && it->type() == HandleItem::Type) return;   // right click on a vertex removes it
    // The topmost item of a layer there. Rulers and hidden links lie over maps and zones without being seen.
    QGraphicsItem *top = nullptr;
    for (QGraphicsItem *i : items(e->pos())) {
        while (i->parentItem() && i->parentItem()->type() != LayerItem::Type) i = i->parentItem();
        if (!i->parentItem() || (i->type() == LinkItem::Type && !linksVisible)) continue;
        top = i;
        break;
    }
    emit contextMenuRequested(e->pos(), top && top->isSelected() ? top : nullptr);
}

void Canvas::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void Canvas::dragMoveEvent(QDragMoveEvent *e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void Canvas::dropEvent(QDropEvent *e) {
    QStringList files;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile()) files << u.toLocalFile();
    if (files.isEmpty()) return;
    e->acceptProposedAction();
    emit filesDropped(files, mapToScene(e->position().toPoint()));
}

QImage roomMask(MapItem *m, QPointF at) {
    if (!m->cal.valid() || !m->src->ready) return {};
    double s = 1;
    const QImage pic = roomPicture(m, &s);
    if (pic.isNull()) return {};
    const int w = pic.width(), h = pic.height();
    const double cw = m->cal.cellSize() * s;   // a cell, in pixels of `pic`
    auto px = [&](int x, int y) { return reinterpret_cast<const QRgb *>(pic.constScanLine(y))[x]; };
    // Median of each channel over about a third of a cell: thin grid lines and texture go, walls stay.
    const int r = std::max(1, int(std::lround(cw * 0.15)));
    std::vector<QRgb> med(size_t(w) * h);
    std::vector<int> ch[3];
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            for (auto &c : ch) c.clear();
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const QRgb v = px(qBound(0, x + dx, w - 1), qBound(0, y + dy, h - 1));
                    ch[0].push_back(qRed(v));
                    ch[1].push_back(qGreen(v));
                    ch[2].push_back(qBlue(v));
                }
            int out[3];
            for (int k = 0; k < 3; ++k) {
                std::nth_element(ch[k].begin(), ch[k].begin() + ch[k].size() / 2, ch[k].end());
                out[k] = ch[k][ch[k].size() / 2];
            }
            med[size_t(y) * w + x] = qRgb(out[0], out[1], out[2]);
        }
    auto distance = [](QRgb a, QRgb b) { return std::hypot(qRed(a) - qRed(b), std::hypot(qGreen(a) - qGreen(b), qBlue(a) - qBlue(b))); };
    // The floor's colour: the median around the point. How far a pixel may stray from it: from the spread there.
    const QPoint c(qBound(0, int(at.x() * s), w - 1), qBound(0, int(at.y() * s), h - 1));
    const int rs = std::max(2, int(cw / 2));
    std::vector<QRgb> around;
    for (int y = std::max(0, c.y() - rs); y <= std::min(h - 1, c.y() + rs); ++y)
        for (int x = std::max(0, c.x() - rs); x <= std::min(w - 1, c.x() + rs); ++x) around.push_back(med[size_t(y) * w + x]);
    int seed[3];
    for (int k = 0; k < 3; ++k) {
        std::vector<int> v;
        for (QRgb q : around) v.push_back(k == 0 ? qRed(q) : k == 1 ? qGreen(q) : qBlue(q));
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        seed[k] = v[v.size() / 2];
    }
    const QRgb floor = qRgb(seed[0], seed[1], seed[2]);
    std::vector<double> spread;
    for (QRgb q : around) spread.push_back(distance(q, floor));
    std::nth_element(spread.begin(), spread.begin() + spread.size() / 2, spread.end());
    const double tolerance = qBound(32.0, 2.5 * spread[spread.size() / 2] + 24, 90.0);
    // Floor: close to that colour, and not under a wall or a closed door drawn over the map.
    QImage drawn(w, h, QImage::Format_Grayscale8);
    drawn.fill(0);
    {
        QPainter p(&drawn);
        p.setPen(QPen(Qt::white, std::max(2.0, cw * 0.15), Qt::SolidLine, Qt::RoundCap));
        for (QGraphicsItem *it : m->scene()->items(m->sceneBoundingRect()))
            if (auto *st = qgraphicsitem_cast<StrokeItem *>(it); st && st->blocksSight() && st->isVisible()) {
                QPolygonF line;
                for (QPointF q : st->pts) line << m->mapFromScene(st->mapToScene(q)) * s;
                p.drawPolyline(line);
            }
    }
    std::vector<uchar> wall(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            wall[size_t(y) * w + x] = distance(med[size_t(y) * w + x], floor) > tolerance || drawn.constScanLine(y)[x];
    // Cores: floor farther than about half a cell from anything else. A doorway a cell wide has none, a room has.
    const std::vector<int> d = chamfer(wall, w, h);
    double rd = 0;
    int start = -1;
    for (double share : {0.55, 0.25}) {   // a corridor a cell wide has only thinner cores
        rd = share * cw;
        double best = 1e300;
        const int reach = std::max(2, int(cw));
        for (int y = std::max(0, c.y() - reach); y <= std::min(h - 1, c.y() + reach); ++y)
            for (int x = std::max(0, c.x() - reach); x <= std::min(w - 1, c.x() + reach); ++x)
                if (d[size_t(y) * w + x] > 3 * rd && std::hypot(x - c.x(), y - c.y()) < best) {
                    best = std::hypot(x - c.x(), y - c.y());
                    start = y * w + x;
                }
        if (start >= 0) break;
    }
    if (start < 0) return {};
    // The core around the point, grown back through the floor (not across walls) by the same distance.
    std::vector<int> grown(size_t(w) * h, 1 << 28);
    std::vector<int> queue{start};
    std::vector<uchar> inCore(size_t(w) * h, 0);
    inCore[start] = 1;
    for (size_t i = 0; i < queue.size(); ++i) {
        const int q = queue[i], x = q % w, y = q / w;
        grown[q] = 0;
        for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            const int nx = x + dx, ny = y + dy, n = ny * w + nx;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h || inCore[n] || d[n] <= 3 * rd) continue;
            inCore[n] = 1;
            queue.push_back(n);
        }
    }
    const int limit = int(3 * (rd + 1.5));
    std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, std::greater<>> open;
    for (int q : queue) open.push({0, q});
    while (!open.empty()) {
        const auto [g, q] = open.top();
        open.pop();
        if (g > grown[q]) continue;
        const int x = q % w, y = q / w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy, n = ny * w + nx;
                if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= w || ny >= h || wall[n]) continue;
                const int ng = g + (dx && dy ? 4 : 3);
                if (ng <= limit && ng < grown[n]) { grown[n] = ng; open.push({ng, n}); }
            }
    }
    // Holes (furniture, text, tokens painted on the floor) are filled; then a quarter cell more, so its walls show.
    std::vector<uchar> outside(size_t(w) * h, 0);
    std::vector<int> edge;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if ((x == 0 || y == 0 || x == w - 1 || y == h - 1) && grown[size_t(y) * w + x] > limit) {
                outside[size_t(y) * w + x] = 1;
                edge.push_back(y * w + x);
            }
    for (size_t i = 0; i < edge.size(); ++i) {
        const int q = edge[i], x = q % w, y = q / w;
        for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            const int nx = x + dx, ny = y + dy, n = ny * w + nx;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h || outside[n] || grown[n] <= limit) continue;
            outside[n] = 1;
            edge.push_back(n);
        }
    }
    std::vector<uchar> room(size_t(w) * h);
    for (size_t i = 0; i < room.size(); ++i) room[i] = !outside[i];
    const std::vector<int> toRoom = chamfer(room, w, h);
    QImage mask(w, h, QImage::Format_Alpha8);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) mask.scanLine(y)[x] = toRoom[size_t(y) * w + x] <= 3 * 0.25 * cw ? 255 : 0;
    return mask;
}

bool Canvas::loupeShown() const { return loupe->isVisible(); }

void Canvas::updateArea(QPointF sp) {
    auto *m = qobject_cast<MapItem *>(areaMap.data());
    if (!m || dragArea < 0) return;
    Area &a = areas[dragArea];
    const AreaShape shape = currentTool == AreaCone ? AreaShape::Cone : currentTool == AreaLine ? AreaShape::Line : AreaShape::Sphere;
    int length = 0;
    const QVector<QPoint> cells = areaCells(m->cal, shape, areaOrigin, m->mapFromScene(sp), &length);
    QPainterPath path;
    for (QPoint q : cells) path.addPolygon(m->mapToScene(m->cal.cellPolygon(q)));
    a.cells->setPath(path);
    QColor fill = color;
    fill.setAlpha(80);
    QPen edge(color, 1);
    edge.setCosmetic(true);
    a.cells->setPen(edge);
    a.cells->setBrush(fill);
    Span span;
    span.ok = true;
    span.hex = m->cal.isHex();
    span.steps = length;
    span.distance = length * m->cal.unitsPerCell;
    span.unit = m->cal.unit;
    static const char *const names[] = {"Sphere", "Cone", "Line"};
    a.text = QString("%1 %2: %3 %4").arg(T(names[int(shape)]), spanText(span)).arg(cells.size()).arg(T(m->cal.noun(int(cells.size()))));
    a.label->setText(a.text);
    a.label->setPos(sp);
}

void Canvas::finishWall() {
    StrokeItem *w = wallDraft;
    wallDraft = nullptr;
    QVector<QPointF> pts = w->pts;
    pts.removeLast();   // the corner that followed the pointer
    while (pts.size() >= 2 && pts.last() == pts[pts.size() - 2]) pts.removeLast();
    if (pts.size() < 2) { delete w; return; }
    w->setPoints(pts);
    sceneEdited();
}

void Canvas::toggleDoor(StrokeItem *d) {
    d->open = !d->open;
    d->jsonHead.clear();   // the undo text has it
    d->update();
    for (QGraphicsItem *it : scene()->items())   // through an open door the tokens see more
        if (auto *k = qgraphicsitem_cast<TokenItem *>(it); k && k->sight > 0) revealSight(k);
    sceneEdited();
    emit status(T(d->open ? "Door open: tokens see through it." : "Door closed."));
}

void Canvas::revealRoom(QPointF sp, bool cover) {
    MapItem *m = mapAt(scene(), sp, true);
    if (!m) { emit status(T("Calibrate the map first (K): rooms are measured in its squares.")); return; }
    if (m->fog.isNull()) {
        if (!cover) { emit status(T("This map has no fog. Cover it in its properties, Fog of war tab.")); return; }
        m->enableFog(false);
    }
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const QImage room = roomMask(m, m->mapFromScene(sp));
    QGuiApplication::restoreOverrideCursor();
    if (room.isNull()) { emit status(T("No room found here.")); return; }
    m->fogMask(room, cover);
    m->fogStrokeDone();
    sceneEdited();
    emit status(T(cover ? "Room covered." : "Room revealed."));
}
