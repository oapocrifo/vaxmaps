#pragma once
#include <QGraphicsView>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QVariantAnimation>

class Calibrator;
class LabelItem;
class LayerItem;
class LinkItem;
class MapItem;
class StrokeItem;
class TokenItem;
class ZoneItem;

extern bool snapEnabled;
MapItem *mapAt(QGraphicsScene *s, QPointF scenePos, bool calibratedOnly);
// On the calibrated map under the point: the nearest square corner / hex vertex, or with `centre` the centre of
// the square or hex. The point itself where no map is calibrated.
QPointF snapPoint(QGraphicsScene *s, QPointF scenePos, bool centre = false);

// One ruler segment, counted in the squares or hexes of the map under its start.
struct Span {
    bool ok = false;   // false: no calibrated map there
    bool hex = false;
    double steps = 0;   // cells; not whole with the straight-line diagonal rule
    double distance = 0;
    QString unit;
};
Span measureSpan(QGraphicsScene *s, QPointF a, QPointF b);
QString spanText(const Span &s);                  // "4 squares · 6 m"
QString totalText(const QVector<Span> &spans);    // sums segments, grouped by grid kind and unit
QString measureText(QGraphicsScene *s, QPointF a, QPointF b);
// Puts a token on the grid of the calibrated map under it: on a cell centre, or on a corner when it is an even
// number of squares wide. Sizes it to its cells there.
void snapToken(TokenItem *t);
// Clears the fog of every map inside the token's sight (a circle or a cone, sized by the calibrated map under it,
// stopped by strokes marked as walls). `commit` false: while dragging, the maps keep their saved fog until the drop.
QList<MapItem *> revealSight(TokenItem *t, bool commit = true);
// What is seen from c up to distance r when walls stop the view (scene coordinates). Empty: no wall nearby.
QPolygonF visibleFrom(QGraphicsScene *s, QPointF c, double r);
// The room around `p` (item coordinates of a calibrated map) as a mask over the whole map, 255 inside: the floor
// joined to p in the map's picture, cut at its walls, at closed doors and walls drawn on it, and at openings about a
// cell wide (doorways). Null when no room is found there.
QImage roomMask(MapItem *m, QPointF p);

class Canvas : public QGraphicsView {
    Q_OBJECT
public:
    // Order matches the menu.
    enum Tool { Select, Pan, Pen, Eraser, Text, Note, Measure, Calibrate, ZoneRect, ZoneEllipse, ZonePoly, Link, Token, Fog,
                Wall, Door, AreaSphere, AreaCone, AreaLine };
    explicit Canvas(QGraphicsScene *scene, QWidget *parent = nullptr);
    void setTool(Tool t);
    Tool tool() const { return currentTool; }
    double zoom() const { return transform().m11(); }
    void zoomBy(double factor);
    bool layerUsable();
    void deleteSelected();
    void cancelDrafts();
    void clearRulers();   // and the spell areas
    int rulerCount() const { return int(rulers.size()); }
    QString rulerText(int i) const;   // what the ruler's last label shows
    int areaCount() const { return int(areas.size()); }
    QString areaText(int i) const { return i >= 0 && i < areas.size() ? areas[i].text : QString(); }
    void toggleDoor(StrokeItem *door);   // opens or closes it; the tokens look again
    bool loupeShown() const;
    void travel(LinkItem *from, QPointF scenePoint);
    void glideTo(QPointF sceneCentre);   // animated move of the view
    void showLinks(bool on);
    bool cameraMoving() const { return camera.state() == QAbstractAnimation::Running; }

    LayerItem *activeLayer = nullptr, *linksLayer = nullptr;
    Calibrator *calibrator;
    QColor color = QColor(230, 60, 60);
    double penWidth = 4;   // screen pixels at the zoom the stroke starts

signals:
    void toolChanged(int tool);
    void status(const QString &msg);
    void zoomChanged(double zoom);
    void snapChanged(bool on);
    void linksShown(bool on);
    void filesDropped(const QStringList &files, QPointF scenePos);
    // Right click: `selected` is the selected item under the cursor, if any (it gets its properties popup).
    void contextMenuRequested(QPoint viewPos, QGraphicsItem *selected);
    void toggleLayersRequested();

protected:
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    struct Ruler {
        QVector<QPointF> pts;   // scene coordinates; consecutive points are segments
        QGraphicsPathItem *line = nullptr;
        QList<QGraphicsItem *> marks;
        QString text;
    };
    struct Area {   // a spell area: transient like a ruler, Esc clears it
        QGraphicsPathItem *cells = nullptr;
        LabelItem *label = nullptr;
        QString text;
    };
    QPointF snapped(QPointF p, bool centre = false) const;
    void updateArea(QPointF scenePos);
    void finishWall();
    void revealRoom(QPointF scenePos, bool cover);
    bool alive(QGraphicsItem *it) const;
    LinkItem *linkAt(QPoint viewPos) const;
    LinkItem *unpairedLink(const LinkItem *except = nullptr) const;
    void eraseAt(QPointF sp);
    void fogTo(QPointF sp);
    double fogWidth() const { return penWidth * 10 / zoom(); }   // scene units
    void updateRuler(Ruler &r);
    void deleteRuler(int i);
    void finishPoly();
    void finishLink(LinkItem *l);
    void updateCursor();

    Tool currentTool = Select;
    bool panning = false, spaceDown = false, erasing = false, erasedAny = false, fogging = false, fogCover = false;
    QPointF fogLast;
    QList<MapItem *> fogTouched;
    QPoint lastPan, pressedLinkAt;
    StrokeItem *stroke = nullptr;
    ZoneItem *draft = nullptr;
    QPointF dragStart;
    QList<Ruler> rulers;
    int dragRuler = -1;
    QGraphicsEllipseItem *eraserRing = nullptr;
    LinkItem *pressedLink = nullptr, *hoveredLink = nullptr;
    QVariantAnimation camera;
    QHash<QGraphicsItem *, QPointF> pressPositions;
    QList<MapItem *> sightTouched;   // fog cleared by tokens during a drag, saved on the drop
    StrokeItem *wallDraft = nullptr; // a wall being clicked corner by corner, or a door being dragged
    QList<Area> areas;
    int dragArea = -1;
    QPointer<QObject> areaMap;       // the map the dragged area is counted on
    QPointF areaOrigin;              // its item coordinates
    QWidget *loupe;                  // calibration magnifier, while the button is held
    bool marking = false;
};
