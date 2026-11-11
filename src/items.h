#pragma once
#include <QGraphicsPathItem>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsTextItem>
#include <QPen>
#include <QVariantAnimation>
#include <functional>

// Called after any edit the user makes on the canvas (sets the project's unsaved flag).
extern std::function<void()> onSceneEdited;
inline void sceneEdited() { if (onSceneEdited) onSceneEdited(); }

// A layer is an empty parent item: hiding it hides its children, its z value orders it.
class LayerItem : public QGraphicsItem {
public:
    enum { Type = UserType + 5 };
    explicit LayerItem(const QString &name) : name(name) { setFlag(ItemHasNoContents); }
    int type() const override { return Type; }
    QRectF boundingRect() const override { return {}; }
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget *) override {}
    void setLocked(bool l);
    void applyLock(QGraphicsItem *child) const;
    QString name;
    bool locked = false;
    bool links = false;   // the layer that holds the links between maps
};

class StrokeItem : public QGraphicsPathItem {
public:
    enum { Type = UserType + 2 };
    StrokeItem(const QColor &color, double width);
    int type() const override { return Type; }
    void setPoints(const QVector<QPointF> &p);
    void addPoint(QPointF p);
    QVector<QPointF> pts;   // item coordinates
    // Undo steps: the stroke as JSON text, and what it was made from. Each part is used only while that still matches.
    QVector<QPointF> jsonPts;
    QByteArray jsonPart;   // the points
    QPointF jsonPos;
    QPen jsonPen;
    QByteArray jsonHead;   // the rest: position, colour, width
};

// Splits a polyline where it passes through the circle (c, r). Returns the pieces left outside it.
QVector<QVector<QPointF>> eraseRuns(const QVector<QPointF> &pts, QPointF c, double r);

// Free text or a note (text on a coloured box).
class TextItem : public QGraphicsTextItem {
public:
    enum { Type = UserType + 3 };
    TextItem(bool note, const QColor &color);
    int type() const override { return Type; }
    void setColor(const QColor &c);
    void startEditing();
    bool editing() const { return textInteractionFlags() & Qt::TextEditable; }
    bool note;
    QColor color;   // note: box colour; text: text colour
protected:
    void paint(QPainter *p, const QStyleOptionGraphicsItem *o, QWidget *w) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
};

class HandleItem;

class ZoneItem : public QGraphicsItem {
public:
    enum Shape { Rect, Ellipse, Poly };
    enum { Type = UserType + 4 };
    ZoneItem(Shape shape, const QPolygonF &pts, const QColor &color);
    int type() const override { return Type; }
    QRectF boundingRect() const override;
    QPainterPath shape() const override { return outline(); }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *o, QWidget *w) override;
    QPainterPath outline() const;
    void setPoints(const QPolygonF &p);
    void setDesc(const QString &d) { desc = d; setToolTip(d); }
    QRectF box() const { return outline().boundingRect(); }
    // Box handles 0..7 (TL, T, TR, R, BR, B, BL, L) resize the whole zone from the opposite side;
    // vertex handles move one polygon vertex. `p` is in item coordinates.
    void beginDrag();
    void dragBox(int id, QPointF p, bool keepAspect);
    void dragVertex(int i, QPointF p);
    void removeVertex(int i);

    Shape shapeKind;
    QPolygonF pts;   // Rect/Ellipse: two opposite corners; Poly: vertices. Item coordinates.
    QString name, desc;
    QColor color;
protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) override;
private:
    void syncHandles();
    QList<HandleItem *> boxHandles, vertexHandles;   // children: deleted with the zone
    QPolygonF startPts;
    QRectF startBox;
};

class HandleItem : public QGraphicsObject {
    Q_OBJECT
public:
    enum { Type = UserType + 6 };
    HandleItem(ZoneItem *zone, bool vertex, int index);
    int type() const override { return Type; }
    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override;
    const bool vertex;
    int index;
protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *e) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *e) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *e) override;
private:
    void grow(double to);
    QPointF outward() const;   // box handles sit a few px outside the box so polygon vertices stay clickable
    ZoneItem *zone;
    QPointF grab;              // press point minus handle point, so the shape does not jump
    bool moved = false;
};

// A creature or object on the map: a disc `cells` squares wide with its name. Dragged, it lands on the grid
// (see snapToken in canvas.h).
class TokenItem : public QGraphicsItem {
public:
    enum { Type = UserType + 8 };
    TokenItem(const QString &name, const QColor &color, double diameter, int cells);
    int type() const override { return Type; }
    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *o, QWidget *w) override;
    void setDiameter(double d) { prepareGeometryChange(); diameter = d; }
    QPointF centre() const { return mapToScene(QPointF(0, 0)); }
    QString name;
    QColor color;
    double diameter;   // item units
    int cells;
};

// Links: pairs of areas; clicking one moves the camera to the other at the same zoom. They are invisible (but
// clickable) unless `linksVisible` is on, which is also when they can be selected, resized and deleted.
extern bool linksVisible;

class LinkItem : public ZoneItem {
public:
    enum { Type = UserType + 7 };
    LinkItem(const QRectF &r, const QColor &color);
    ~LinkItem() override;
    int type() const override { return Type; }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *o, QWidget *w) override;
    void setHover(bool on);   // invisible links glow under the cursor
    void syncFlags();         // selectable and movable only while links are shown and the layer is not locked
    QString id;
    LinkItem *partner = nullptr;
private:
    double glow = 0;
    QVariantAnimation glowAnim;
};
void setLinksVisible(QGraphicsScene *s, bool on);

// Screen-sized label with a dark background (ruler readout, hints).
class LabelItem : public QGraphicsSimpleTextItem {
public:
    LabelItem();
    QRectF boundingRect() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *o, QWidget *w) override;
};
