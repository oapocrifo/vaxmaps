#include "items.h"
#include "canvas.h"
#include "lang.h"
#include "ui.h"

#include <QGraphicsScene>
#include <QFocusEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPen>
#include <QStyleOptionGraphicsItem>
#include <QTextCursor>
#include <QTextDocument>
#include <cmath>

std::function<void()> onSceneEdited;
bool linksVisible = false;

// ---- LayerItem ----

void LayerItem::applyLock(QGraphicsItem *c) const {
    if (c->type() == LinkItem::Type) return static_cast<LinkItem *>(c)->syncFlags();
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

void StrokeItem::paint(QPainter *p, const QStyleOptionGraphicsItem *o, QWidget *w) {
    if (!door) return QGraphicsPathItem::paint(p, o, w);
    // A door: a bar with a light core while closed, a thin dashed line while open.
    QPen bar = pen();
    if (open) {
        bar.setStyle(Qt::DashLine);
        bar.setWidthF(bar.widthF() / 2);
    }
    p->setPen(bar);
    p->drawPath(path());
    if (!open) {
        p->setPen(QPen(QColor(245, 235, 210), bar.widthF() / 3, Qt::SolidLine, Qt::FlatCap));
        p->drawPath(path());
    }
    if (isSelected()) {
        QPen sel(QColor(255, 255, 255, 200), 0, Qt::DashLine);
        p->setPen(sel);
        p->setBrush(Qt::NoBrush);
        p->drawRect(boundingRect());
    }
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
    // A context menu or switching windows is not the end of the edit (an empty new note would be deleted).
    if (e->reason() == Qt::PopupFocusReason || e->reason() == Qt::ActiveWindowFocusReason) return;
    setTextInteractionFlags(Qt::NoTextInteraction);
    QTextCursor tc = textCursor();   // text selected during the edit would stay highlighted
    tc.clearSelection();
    setTextCursor(tc);
    if (toPlainText().trimmed().isEmpty()) deleteLater();
}

// ---- TokenItem ----

TokenItem::TokenItem(const QString &name, const QColor &color, double diameter, int cells)
    : name(name), color(color), diameter(diameter), cells(cells) {
    setFlags(ItemIsSelectable | ItemIsMovable);
}

QRectF TokenItem::boundingRect() const {
    const double r = diameter / 2;
    const QRectF own(-r * 1.6, -r - 2, r * 3.2, diameter * 1.45 + 4);   // the name sits under the disc and may be wider
    const double s = sight > 0 ? sightRadius + 1 : 0;
    return own.united(QRectF(-s, -s, 2 * s, 2 * s));
}

QPainterPath TokenItem::sightArea(double radius) const {
    QPainterPath path;
    if (!cone) {
        path.addEllipse(QPointF(0, 0), radius, radius);
        return path;
    }
    // Qt arcs turn counterclockwise on screen; facing turns clockwise.
    path.moveTo(0, 0);
    path.arcTo(QRectF(-radius, -radius, 2 * radius, 2 * radius), -(facing + coneAngle / 2), coneAngle);
    path.closeSubpath();
    return path;
}

QPainterPath TokenItem::shape() const {
    QPainterPath path;
    path.addEllipse(QPointF(0, 0), diameter / 2, diameter / 2);
    return path;
}

void TokenItem::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) {
    const double r = diameter / 2;
    p->setRenderHint(QPainter::Antialiasing);
    if (sight > 0 && sightRadius > 0 && (isSelected() || cone)) {   // a cone always shows where it points
        p->setPen(QPen(QColor(255, 220, 90, isSelected() ? 230 : 110), 0, Qt::DashLine));
        p->setBrush(QColor(255, 220, 90, isSelected() ? 30 : 0));
        p->drawPath(sightArea(sightRadius));
    }
    p->setPen(QPen(isSelected() ? QColor(80, 160, 255) : QColor(255, 255, 255), diameter * (isSelected() ? 0.09 : 0.06)));
    p->setBrush(color);
    p->drawEllipse(QPointF(0, 0), r * 0.94, r * 0.94);
    QFont f = p->font();
    f.setBold(true);
    f.setPixelSize(qMax(1, int(diameter * 0.38)));
    p->setFont(f);
    p->setPen(color.lightnessF() > 0.6 ? Qt::black : Qt::white);
    p->drawText(QRectF(-r, -r, diameter, diameter), Qt::AlignCenter, name.left(2));   // the start of the name, or its number
    if (name.size() <= 2) return;
    f.setPixelSize(qMax(1, int(diameter * 0.22)));
    p->setFont(f);
    const QRectF label(-r * 1.6, r + 1, r * 3.2, diameter * 0.4);
    const QString shown = QFontMetricsF(f).elidedText(name, Qt::ElideRight, label.width());
    QPainterPath text;
    text.addText(label.center().x() - QFontMetricsF(f).horizontalAdvance(shown) / 2, label.top() + QFontMetricsF(f).ascent(), f, shown);
    p->setPen(QPen(QColor(0, 0, 0, 200), diameter * 0.05, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(Qt::NoBrush);
    p->drawPath(text);   // dark outline: readable on any map
    p->fillPath(text, Qt::white);
}

// ---- ZoneItem ----

ZoneItem::ZoneItem(Shape shape, const QPolygonF &pts, const QColor &color)
    : shapeKind(shape), pts(pts), color(color) {
    setFlags(ItemIsSelectable | ItemIsMovable);
}

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

void ZoneItem::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) {
    QColor fill = color;
    fill.setAlpha(60);
    QPen pen(color, 2);
    pen.setCosmetic(true);
    if (isSelected()) {
        pen.setStyle(Qt::DashLine);
        if (shapeKind != Rect) {   // the box the resize handles act on
            p->setPen(QPen(QColor(255, 255, 255, 90), 0, Qt::DotLine));
            p->setBrush(Qt::NoBrush);
            p->drawRect(box());
        }
    }
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

void ZoneItem::syncHandles() {
    if (boxHandles.isEmpty()) return;
    const QRectF b = box();
    const QPointF c = b.center();
    const QPointF at[8] = {b.topLeft(), {c.x(), b.top()}, b.topRight(), {b.right(), c.y()},
                           b.bottomRight(), {c.x(), b.bottom()}, b.bottomLeft(), {b.left(), c.y()}};
    for (int i = 0; i < 8; ++i) boxHandles[i]->setPos(at[i]);
    const int n = shapeKind == Poly ? int(pts.size()) : 0;
    for (int i = 0; i < n; ++i) {
        if (i == vertexHandles.size()) vertexHandles << new HandleItem(this, true, i);
        vertexHandles[i]->setPos(pts[i]);
        vertexHandles[i]->setVisible(true);
    }
    for (int i = n; i < vertexHandles.size(); ++i) vertexHandles[i]->setVisible(false);   // a handle may be mid-click: hide, don't delete
}

void ZoneItem::beginDrag() {
    startPts = pts;
    startBox = box();
}

void ZoneItem::dragBox(int id, QPointF p, bool keepAspect) {
    const QRectF r = startBox;
    if (r.width() <= 0 || r.height() <= 0) return;
    const bool left = id == 0 || id == 6 || id == 7, right = id >= 2 && id <= 4;
    const bool top = id <= 2, bottom = id >= 4 && id <= 6;
    double x1 = r.left(), x2 = r.right(), y1 = r.top(), y2 = r.bottom();
    if (left) x1 = p.x();
    if (right) x2 = p.x();
    if (top) y1 = p.y();
    if (bottom) y2 = p.y();
    if (keepAspect && (left || right) && (top || bottom)) {   // Shift on a corner: same proportions
        double sx = (x2 - x1) / r.width(), sy = (y2 - y1) / r.height();
        const double s = std::max(std::abs(sx), std::abs(sy));
        sx = sx < 0 ? -s : s;
        sy = sy < 0 ? -s : s;
        if (left) x1 = x2 - sx * r.width(); else x2 = x1 + sx * r.width();
        if (top) y1 = y2 - sy * r.height(); else y2 = y1 + sy * r.height();
    }
    if (std::abs(x2 - x1) < 2 || std::abs(y2 - y1) < 2) return;
    QPolygonF out;
    for (const QPointF &q : startPts)
        out << QPointF(x1 + (q.x() - r.left()) * (x2 - x1) / r.width(), y1 + (q.y() - r.top()) * (y2 - y1) / r.height());
    setPoints(out);
}

void ZoneItem::dragVertex(int i, QPointF p) {
    QPolygonF out = pts;
    out[i] = p;
    setPoints(out);
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
            for (int i = 0; i < 8; ++i) boxHandles << new HandleItem(this, false, i);
            syncHandles();
        } else {
            qDeleteAll(boxHandles);
            qDeleteAll(vertexHandles);
            boxHandles.clear();
            vertexHandles.clear();
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
    // only on an edge (10 px on screen): a double click in the middle of the zone used to pull a vertex there
    const auto *view = e->widget() ? qobject_cast<const QGraphicsView *>(e->widget()->parentWidget()) : nullptr;
    if (view && bestDist * view->transform().m11() > 10) return QGraphicsItem::mouseDoubleClickEvent(e);
    QPolygonF p = pts;
    p.insert(best + 1, e->pos());
    setPoints(p);
    sceneEdited();
}

// ---- HandleItem ----

HandleItem::HandleItem(ZoneItem *zone, bool vertex, int index) : QGraphicsObject(zone), vertex(vertex), index(index), zone(zone) {
    setFlag(ItemIgnoresTransformations);   // same size on screen at any zoom
    setAcceptHoverEvents(true);
    setZValue(vertex ? 2 : 1);
    static const Qt::CursorShape cursors[8] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                               Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor};
    setCursor(vertex ? Qt::SizeAllCursor : cursors[index]);
    setToolTip(T(vertex ? "Drag to move the vertex. Right click removes it."
                        : "Drag to resize. Shift on a corner keeps the proportions."));
    setOpacity(0);
    auto *a = new QPropertyAnimation(this, "opacity", this);   // handles fade in when the zone is selected
    a->setDuration(160);
    a->setEndValue(1.0);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

QPointF HandleItem::outward() const {
    if (vertex) return {};
    static const QPointF dir[8] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
    return dir[index] * 11;
}

QRectF HandleItem::boundingRect() const { return QRectF(-8, -8, 16, 16).translated(outward()); }

QPainterPath HandleItem::shape() const {   // exact hit area: a polygon vertex on the box corner stays reachable
    QPainterPath p;
    if (vertex) p.addEllipse(QPointF(), 7, 7);
    else p.addRect(QRectF(-6, -6, 12, 12).translated(outward()));
    return p;
}

void HandleItem::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) {
    p->setRenderHint(QPainter::Antialiasing);
    p->translate(outward());
    if (vertex) {
        p->setPen(QPen(Qt::white, 1.5));
        p->setBrush(ui::accent);
        p->drawEllipse(QPointF(), 5, 5);
    } else {
        p->setPen(QPen(QColor(20, 20, 24), 1));
        p->setBrush(Qt::white);
        p->drawRoundedRect(QRectF(-4.5, -4.5, 9, 9), 2, 2);
    }
}

void HandleItem::grow(double to) {
    auto *a = new QPropertyAnimation(this, "scale", this);
    a->setDuration(130);
    a->setEasingCurve(QEasingCurve::OutCubic);
    a->setEndValue(to);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void HandleItem::hoverEnterEvent(QGraphicsSceneHoverEvent *) { grow(1.4); }
void HandleItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *) { grow(1.0); }

void HandleItem::mousePressEvent(QGraphicsSceneMouseEvent *e) {
    if (e->button() == Qt::RightButton) {
        if (vertex) zone->removeVertex(index);
        return;   // accepted: the canvas shows no menu for it
    }
    if (e->button() != Qt::LeftButton) return e->ignore();
    grab = zone->mapFromScene(e->scenePos()) - pos();
    moved = false;
    zone->beginDrag();
}

void HandleItem::mouseMoveEvent(QGraphicsSceneMouseEvent *e) {
    // A right click that removes a vertex also grabs the mouse: moving 1 px dragged the next vertex to the cursor.
    if (!(e->buttons() & Qt::LeftButton)) return;
    QPointF scenePt = zone->mapToScene(zone->mapFromScene(e->scenePos()) - grab);
    if (snapEnabled) scenePt = snapPoint(zone->scene(), scenePt);
    const QPointF p = zone->mapFromScene(scenePt);
    if (vertex) zone->dragVertex(index, p);
    else zone->dragBox(index, p, e->modifiers() & Qt::ShiftModifier);
    moved = true;
}

void HandleItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *) {
    if (moved) sceneEdited();
}

// ---- LinkItem ----

LinkItem::LinkItem(const QRectF &r, const QColor &color) : ZoneItem(Rect, {r.topLeft(), r.bottomRight()}, color) {
    glowAnim.setDuration(180);
    glowAnim.setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(&glowAnim, &QVariantAnimation::valueChanged, [this](const QVariant &v) { glow = v.toDouble(); update(); });
    syncFlags();
}

LinkItem::~LinkItem() {
    if (partner) partner->partner = nullptr;
}

void LinkItem::syncFlags() {
    const auto *layer = static_cast<const LayerItem *>(parentItem());
    const bool editable = linksVisible && !(layer && layer->locked);
    setFlag(ItemIsSelectable, editable);
    setFlag(ItemIsMovable, editable);
    if (!editable) setSelected(false);
    update();
}

void LinkItem::setHover(bool on) { ui::glide(glowAnim, glow, on ? 1 : 0, on ? 160 : 260); }

void LinkItem::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) {
    const QRectF r = box();
    p->setRenderHint(QPainter::Antialiasing);
    if (!linksVisible) {
        if (glow < 0.01) return;
        QColor fill = color, edge = color;
        fill.setAlphaF(0.22 * glow);
        edge.setAlphaF(0.9 * glow);
        QPen pen(edge, 2);
        pen.setCosmetic(true);
        p->setPen(pen);
        p->setBrush(fill);
        p->drawRect(r);
        return;
    }
    QColor fill = color;
    fill.setAlpha(70);
    QPen pen(partner ? color : ui::danger, 2, isSelected() ? Qt::DashLine : Qt::DashDotLine);
    pen.setCosmetic(true);
    p->setPen(pen);
    p->setBrush(fill);
    p->drawRect(r);
    const double lod = QStyleOptionGraphicsItem::levelOfDetailFromTransform(p->worldTransform());
    if (lod <= 0) return;
    const QString label = (name.isEmpty() ? T("Link") : name) + (partner ? QString("  ⇄") : "  " + T("(no pair)"));
    QFont f = p->font();
    f.setPixelSize(13);
    f.setBold(true);
    if (r.width() * lod < QFontMetrics(f).horizontalAdvance(label) + 8) return;
    p->save();
    p->translate(r.center());
    p->scale(1 / lod, 1 / lod);
    p->setFont(f);
    p->setPen(QColor(0, 0, 0, 200));
    p->drawText(QRectF(-200, -11, 400, 24), Qt::AlignCenter, label);
    p->setPen(Qt::white);
    p->drawText(QRectF(-200, -12, 400, 24), Qt::AlignCenter, label);
    p->restore();
}

void setLinksVisible(QGraphicsScene *s, bool on) {
    linksVisible = on;
    for (QGraphicsItem *it : s->items())
        if (it->type() == LinkItem::Type) static_cast<LinkItem *>(it)->syncFlags();
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
