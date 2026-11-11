#pragma once
// Grid calibration modelled on Atlas VTT 0.13.1 (GridAlignmentOverlay, IntersectionsTab, gridAlignmentMath):
// measure one cell by two adjacent corners, let the map's lines make it exact, measure more cells in the
// other quarters of the map only when the lines do not confirm it; or detect the grid automatically.
#include <QFrame>
#include <QPointer>
#include <QVector>
#include <optional>

class Canvas;
class MapItem;
class QDoubleSpinBox;
class QGraphicsItem;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPushButton;

// Line strength of a map summed per column (x) and per row (y), from the image scaled to at most 4096 px.
struct LineProfiles {
    QVector<float> x, y;
    double sx = 1, sy = 1;   // analysis pixels per image pixel
};
LineProfiles lineProfiles(const QImage &image, QSize fullSize);
LineProfiles loadLineProfiles(const QString &path);

struct GridFit {
    double cell = 0;     // image px
    QPointF origin;      // a grid intersection, image px
    double strength = 0; // how far the lines stand out from the rest of the map (0 = not at all)
};
// The grid within 13 % of `cellHint` (image px) that the map's lines support.
std::optional<GridFit> fitGrid(const LineProfiles &p, double cellHint);
// The map's grid found from its lines alone.
std::optional<GridFit> detectGrid(const LineProfiles &p);

struct Measurement { QPointF a, b; };   // two adjacent corners of one cell, image px
// Atlas' calculateFromMeasurements: mean of the cell sizes, circular mean of the corners for the offset.
GridFit gridFromMeasurements(const QVector<Measurement> &m);

// Panel shown over the canvas while the Calibrate tool is active.
class Calibrator : public QFrame {
    Q_OBJECT
public:
    explicit Calibrator(Canvas *canvas);
    void start(MapItem *selected);
    void stop();                  // drops a result that was not applied
    void mousePress(QPointF scenePos);
    bool key(QKeyEvent *e);
    void apply();
    void autoDetect();

    MapItem *target() const { return map; }
    const std::optional<GridFit> &current() const { return result; }
    bool busy() const { return waitingFit || waitingDetect; }

private:
    void choose(MapItem *m);
    void reset();
    void measureAgain();
    void showStep();
    void measured();
    void tryFit();
    void setResult(const GridFit &g, const QString &how);
    void requestProfiles();
    void profilesReady(const QString &path);
    void clearMarks();
    QGraphicsItem *mark(QGraphicsItem *item);
    QRectF quarter(int q) const;
    void setText(const QString &s);

    Canvas *canvas;
    QPointer<MapItem> map;
    QVector<Measurement> measurements;
    std::optional<QPointF> pointA;
    std::optional<GridFit> result;
    bool waitingFit = false, waitingDetect = false;
    QList<QGraphicsItem *> marks;   // children of the map: deleted with it

    QLabel *title, *instruction, *progress;
    QPushButton *detectButton, *applyButton, *againButton;
    QDoubleSpinBox *perCell;
    QLineEdit *unit;
};
