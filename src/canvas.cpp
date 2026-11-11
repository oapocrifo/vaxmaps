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
#include <QScrollBar>
#include <QUuid>
#include <QWheelEvent>
#include <cmath>

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
    }
    return {};
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
    return QString("%1 %2 · %3 %4").arg(s.steps).arg(noun, fmt(s.distance), s.unit);
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
    if (t == Link && !linksVisible) showLinks(true);
    updateCursor();
    emit toolChanged(t);
    emit status(toolHint(t));
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
        const QPointF p = snapped(sp, true);
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
        calibrator->mousePress(sp);
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
    if (erasing) { erasing = false; if (erasedAny) sceneEdited(); return; }
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
        for (auto it = pressPositions.cbegin(); it != pressPositions.cend(); ++it)
            if (alive(it.key()) && it.key()->pos() != it.value()) { sceneEdited(); break; }
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
            n->setParentItem(s->parentItem());
            n->setPos(s->pos());
            n->setPoints(runs[i]);
        }
    }
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
    // Tool letters live here instead of QAction shortcuts so typing in a text item never switches tools.
    static const QHash<int, Tool> keys{{Qt::Key_V, Select}, {Qt::Key_H, Pan}, {Qt::Key_P, Pen}, {Qt::Key_E, Eraser},
                                       {Qt::Key_T, Text}, {Qt::Key_N, Note}, {Qt::Key_M, Measure}, {Qt::Key_K, Calibrate},
                                       {Qt::Key_R, ZoneRect}, {Qt::Key_O, ZoneEllipse}, {Qt::Key_G, ZonePoly}, {Qt::Key_J, Link}};
    if (keys.contains(e->key()) && !(e->modifiers() & Qt::ShiftModifier)) { setTool(keys[e->key()]); return; }
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
