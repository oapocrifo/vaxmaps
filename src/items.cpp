#include "items.h"
#include "canvas.h"

#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPen>
#include <QStyleOptionGraphicsItem>
#include <QTextCursor>
#include <QTextDocument>
#include <cmath>

std::function<void()> onSceneEdited;

// ---- LayerItem ----

void LayerItem::applyLock(QGraphicsItem *c) const {
    c->setFlag(ItemIsSelectable, !locked);
    c->setFlag(ItemIsMovable, !locked);
    c->setAcceptedMouseButtons(locked ? Qt::NoButton : Qt::AllButtons);   // clicks fall through to layers below
    if (locked) c->setSelected(false);
}

void LayerItem::setLocked(bool l) {
    locked = l;
    for (QGraphicsItem *c : childItems()) applyLock(c);
}

// ---- StrokeItem ----

StrokeItem::StrokeItem(const QColor &color, double width) {
    setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    setFlags(ItemIsSelectable | ItemIsMovable);
}

void StrokeItem::setPoints(const QVector<QPointF> &p) {
    pts = p;
    QPainterPath path;
    if (!pts.isEmpty()) {
        path.moveTo(pts[0]);
        if (pts.size() == 1) path.lineTo(pts[0] + QPointF(0.01, 0));   // a click draws a dot
        for (int i = 1; i < pts.size(); ++i) path.lineTo(pts[i]);
    }
    setPath(path);
}

void StrokeItem::addPoint(QPointF p) {
    if (pts.isEmpty()) { setPoints({p}); return; }
    pts << p;
    QPainterPath path = this->path();
    path.lineTo(p);
    setPath(path);
}

QVector<QVector<QPointF>> eraseRuns(const QVector<QPointF> &pts, QPointF c, double r) {
    QVector<QVector<QPointF>> runs;
    QVector<QPointF> cur;
    if (pts.isEmpty()) return runs;
    if (QLineF(pts[0], c).length() > r) cur << pts[0];
    for (int i = 1; i < pts.size(); ++i) {
        const QPointF a = pts[i - 1], b = pts[i], d = b - a, f = a - c;
        // |a + t d - c| = r  ->  t^2 (d.d) + 2t (f.d) + f.f - r^2 = 0
        const double A = QPointF::dotProduct(d, d), B = 2 * QPointF::dotProduct(f, d);
        const double C = QPointF::dotProduct(f, f) - r * r;
        const double disc = B * B - 4 * A * C;
        double t1 = 2, t2 = 2;
        if (A > 0 && disc > 0) {
            t1 = (-B - std::sqrt(disc)) / (2 * A);
            t2 = (-B + std::sqrt(disc)) / (2 * A);
        } else if (A == 0 && C <= 0) {
            t1 = -1; t2 = 2;   // zero-length segment inside the circle
        }
        if (t2 < 0 || t1 > 1) { cur << b; continue; }   // segment misses the circle
        if (t1 > 0) { cur << a + t1 * d; runs << cur; cur.clear(); }
        if (t2 < 1) cur << a + t2 * d << b;
    }
    if (!cur.isEmpty()) runs << cur;
    return runs;
}

// ---- TextItem ----

TextItem::TextItem(bool note, const QColor &color) : note(note) {
    setFlags(ItemIsSelectable | ItemIsMovable);
    QFont f = font();
    f.setPixelSize(20);
    setFont(f);
    if (note) {
        setTextWidth(220);
        setDefaultTextColor(Qt::black);
        document()->setDocumentMargin(10);
    }
    setColor(color);
    connect(document(), &QTextDocument::contentsChanged, [] { sceneEdited(); });
}

void TextItem::setColor(const QColor &c) {
    color = c;
    if (!note) setDefaultTextColor(c);
    update();
}

void TextItem::startEditing() {
    setTextInteractionFlags(Qt::TextEditorInteraction);
    setFocus(Qt::MouseFocusReason);
    QTextCursor tc = textCursor();
    tc.movePosition(QTextCursor::End);
    setTextCursor(tc);
}

void TextItem::paint(QPainter *p, const QStyleOptionGraphicsItem *o, QWidget *w) {
    if (note) {
        p->setPen(QPen(color.darker(150), 0));
        p->setBrush(color);
        p->drawRoundedRect(boundingRect(), 4, 4);
    }
    QGraphicsTextItem::paint(p, o, w);
}

void TextItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) {
    if (!editing()) startEditing();
    QGraphicsTextItem::mouseDoubleClickEvent(e);
}

void TextItem::focusOutEvent(QFocusEvent *e) {
    QGraphicsTextItem::focusOutEvent(e);
    setTextInteractionFlags(Qt::NoTextInteraction);
    if (toPlainText().trimmed().isEmpty()) deleteLater();
}

// ---- ZoneItem ----

ZoneItem::ZoneItem(Shape shape, const QPolygonF &pts, const QColor &color)
    : shapeKind(shape), pts(pts), color(color) {
    setFlags(ItemIsSelectable | ItemIsMovable);
}

ZoneItem::~ZoneItem() { syncing = true; }   // children (handles) die after us: ignore their position updates

QPainterPath ZoneItem::outline() const {
    QPainterPath path;
    if (shapeKind == Poly) {
        path.addPolygon(pts);
        path.closeSubpath();
    } else if (pts.size() == 2) {
        const QRectF r = QRectF(pts[0], pts[1]).normalized();
        shapeKind == Rect ? path.addRect(r) : path.addEllipse(r);
    }
    return path;
}

QRectF ZoneItem::boundingRect() const { return outline().boundingRect().adjusted(-1, -1, 1, 1); }

void ZoneItem::paint(QPainter *p, const QStyleOptionGraphicsItem *o, QWidget *) {
    QColor fill = color;
    fill.setAlpha(60);
    QPen pen(color, 2);
    pen.setCosmetic(true);
    if (isSelected()) pen.setStyle(Qt::DashLine);
    p->setPen(pen);
    p->setBrush(fill);
    p->drawPath(outline());
    if (name.isEmpty()) return;
    const double lod = QStyleOptionGraphicsItem::levelOfDetailFromTransform(p->worldTransform());
    QFont f = p->font();
    f.setPixelSize(14);
    f.setBold(true);
    if (lod <= 0 || outline().boundingRect().width() * lod < QFontMetrics(f).horizontalAdvance(name)) return;   // label wider than the zone on screen
    p->save();
    p->translate(outline().boundingRect().center());
    p->scale(1 / lod, 1 / lod);   // label keeps its screen size at any zoom
    p->setFont(f);
    const QRectF tr(-200, -12, 400, 24);
    p->setPen(QColor(0, 0, 0, 200));
    p->drawText(tr.translated(1, 1), Qt::AlignCenter, name);
    p->setPen(Qt::white);
    p->drawText(tr, Qt::AlignCenter, name);
    p->restore();
}

void ZoneItem::setPoints(const QPolygonF &p) {
    prepareGeometryChange();
    pts = p;
    update();
    syncHandles();
}

QPolygonF ZoneItem::handlePositions() const {
    if (shapeKind == Poly) return pts;
    const QRectF r = QRectF(pts[0], pts[1]).normalized();
    return {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
}

void ZoneItem::syncHandles() {
    if (handles.isEmpty()) return;
    syncing = true;
    const QPolygonF pos = handlePositions();
    for (int i = 0; i < pos.size(); ++i) {
        if (i == handles.size()) handles << new HandleItem(this, i);
        handles[i]->setPos(pos[i]);
        handles[i]->setVisible(true);
    }
    for (int i = pos.size(); i < handles.size(); ++i) handles[i]->setVisible(false);   // deleted on deselect
    syncing = false;
}

void ZoneItem::moveHandle(int i, QPointF p) {
    if (syncing) return;
    prepareGeometryChange();
    if (shapeKind == Poly) {
        pts[i] = p;
    } else {
        const QPolygonF c = handlePositions();
        pts = {p, c[(i + 2) % 4]};   // the opposite corner stays put
    }
    update();
    syncHandles();
    sceneEdited();
}

void ZoneItem::removeVertex(int i) {
    if (shapeKind != Poly || pts.size() <= 3) return;
    QPolygonF p = pts;
    p.remove(i);
    setPoints(p);
    sceneEdited();
}

QVariant ZoneItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    if (change == ItemSelectedHasChanged) {
        if (value.toBool()) {
            handles << new HandleItem(this, 0);
            syncHandles();
        } else {
            qDeleteAll(handles);
            handles.clear();
        }
    }
    return QGraphicsItem::itemChange(change, value);
}

void ZoneItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) {
    if (shapeKind != Poly || !isSelected()) return QGraphicsItem::mouseDoubleClickEvent(e);
    // insert a vertex on the nearest edge
    int best = -1;
    double bestDist = 1e300;
    for (int i = 0; i < pts.size(); ++i) {
        const QPointF a = pts[i], b = pts[(i + 1) % pts.size()], d = b - a;
        const double len2 = QPointF::dotProduct(d, d);
        const double t = len2 > 0 ? qBound(0.0, QPointF::dotProduct(e->pos() - a, d) / len2, 1.0) : 0;
        const double dist = QLineF(a + t * d, e->pos()).length();
        if (dist < bestDist) { bestDist = dist; best = i; }
    }
    QPolygonF p = pts;
    p.insert(best + 1, e->pos());
    setPoints(p);
    sceneEdited();
}

// ---- HandleItem ----

HandleItem::HandleItem(ZoneItem *zone, int index) : QGraphicsRectItem(-5, -5, 10, 10, zone), index(index), zone(zone) {
    setFlags(ItemIsMovable | ItemIgnoresTransformations | ItemSendsGeometryChanges);
    setBrush(Qt::white);
    setPen(QPen(Qt::black, 0));
    setCursor(Qt::SizeAllCursor);
    setToolTip(zone->shapeKind == ZoneItem::Poly ? "Arraste para mover. Botão direito remove o vértice." : "Arraste para redimensionar.");
}

QVariant HandleItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    if (change == ItemPositionChange && snapEnabled && scene() && scene()->mouseGrabberItem() == this)
        return zone->mapFromScene(snapPoint(scene(), zone->mapToScene(value.toPointF())));
    if (change == ItemPositionHasChanged) zone->moveHandle(index, value.toPointF());
    return QGraphicsRectItem::itemChange(change, value);
}

void HandleItem::mousePressEvent(QGraphicsSceneMouseEvent *e) {
    if (e->button() == Qt::RightButton) { zone->removeVertex(index); return; }
    QGraphicsRectItem::mousePressEvent(e);
}

// ---- LabelItem ----

LabelItem::LabelItem() {
    setFlag(ItemIgnoresTransformations);
    setBrush(Qt::white);
    setZValue(1e6);
    QFont f = font();
    f.setPixelSize(13);
    f.setBold(true);
    setFont(f);
}

QRectF LabelItem::boundingRect() const {
    return QGraphicsSimpleTextItem::boundingRect().adjusted(-5, -3, 5, 3).translated(14, 14);
}

void LabelItem::paint(QPainter *p, const QStyleOptionGraphicsItem *o, QWidget *w) {
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(0, 0, 0, 200));
    p->drawRoundedRect(boundingRect(), 3, 3);
    p->translate(14, 14);
    QGraphicsSimpleTextItem::paint(p, o, w);
}
