#pragma once
#include <QGraphicsView>
#include <QHash>

class LayerItem;
class MapItem;
class StrokeItem;
class ZoneItem;
class LabelItem;
class Calibrator;

extern bool snapEnabled;
MapItem *mapAt(QGraphicsScene *s, QPointF scenePos, bool calibratedOnly);
// Nearest grid intersection of the calibrated map under the point (the point itself if there is none).
QPointF snapPoint(QGraphicsScene *s, QPointF scenePos);
// "7,5 m · 5 quadrados" using the calibration of the map under `a`, or pixels when there is none.
QString measureText(QGraphicsScene *s, QPointF a, QPointF b);

class Canvas : public QGraphicsView {
    Q_OBJECT
public:
    enum Tool { Select, Pan, Pen, Eraser, Text, Note, Measure, Calibrate, ZoneRect, ZoneEllipse, ZonePoly };
    explicit Canvas(QGraphicsScene *scene, QWidget *parent = nullptr);
    void setTool(Tool t);
    Tool tool() const { return currentTool; }
    double zoom() const { return transform().m11(); }
    void zoomBy(double factor);
    bool layerUsable();
    void deleteSelected();
    void cancelDrafts();

    LayerItem *activeLayer = nullptr;
    Calibrator *calibrator;
    QColor color = QColor(230, 60, 60);
    double penWidth = 4;   // screen pixels at the zoom the stroke starts

signals:
    void toolChanged(int tool);
    void status(const QString &msg);
    void zoomChanged(double zoom);
    void snapChanged(bool on);
    // Right click: `selected` is the selected item under the cursor, if any (it gets its properties popup).
    void contextMenuRequested(QPoint viewPos, QGraphicsItem *selected);
    void toggleLayersRequested();
    void filesDropped(const QStringList &files, QPointF scenePos);

protected:
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    QPointF snapped(QPointF p) const;
    void eraseAt(QPointF sp);
    void updateMeasure(QPointF to);
    void finishPoly();
    void updateCursor();

    Tool currentTool = Select;
    bool panning = false, spaceDown = false, erasing = false, erasedAny = false, measuring = false;
    QPoint lastPan;
    StrokeItem *stroke = nullptr;
    ZoneItem *draft = nullptr;
    QPointF dragStart, measureFrom;
    QGraphicsLineItem *measureLine = nullptr;
    LabelItem *measureLabel = nullptr;
    QGraphicsEllipseItem *eraserRing = nullptr;
    QHash<QGraphicsItem *, QPointF> pressPositions;
};
