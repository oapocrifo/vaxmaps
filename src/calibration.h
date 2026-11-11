#pragma once
// Grid calibration. The manual flow follows Atlas VTT 0.13.1 (measure one cell in each quarter of the map and let
// the map's own lines make it exact). Detection finds squares or hexes from the lines of the whole map and follows
// local variations of the drawn grid with a field of corrections (see Calibration::field).
#include "grid.h"
#include "ui.h"
#include <QImage>
#include <QPointer>
#include <optional>

class Canvas;
class MapItem;
class QDoubleSpinBox;
class QGraphicsItem;
class QKeyEvent;
class QLabel;
class QLineEdit;

// Line evidence of a map, from the image scaled to at most 4096 px ("analysis" pixels).
struct Analysis {
    QImage gray, energy;     // energy: edge strength per pixel (hex centres sit where it is low)
    double sx = 1, sy = 1;   // analysis px per image px
    struct Projection {
        QPointF n;           // normal of the lines measured; t = n·(x, y)
        double t0 = 0;       // t of band 0
        QVector<float> sum, count;   // line strength summed per 1 px band of t, and pixels per band
    };
    // Lines at 0, 30, 60, 90, 120 and 150 degrees, over the whole map (region 0) and over each quarter (1-4):
    // the quarters keep a grid whose size drifts across the map from smearing out.
    Projection proj[5][6];
    bool valid() const { return !proj[0][0].sum.isEmpty(); }
};
Analysis analyse(const QImage &image, QSize fullSize);
Analysis loadAnalysis(const QString &path);

struct GridFit {
    Calibration cal;     // image px
    double strength = 0; // how far the lines stand out from the rest of the map (0 = not at all)
    int regions = 0;     // map regions whose local correction was measured from the lines
};
// The grid of `type` within 13 % of `cellHint` (image px) that the map's lines support.
std::optional<GridFit> fitGrid(const Analysis &a, Calibration::Grid type, double cellHint);
// Squares or hexes of any size, found from the lines alone.
std::optional<GridFit> detectGrid(const Analysis &a);

struct Measurement { QPointF a, b; };   // image px: two adjacent corners (squares) or two neighbouring centres (hexes)
// Atlas' calculateFromMeasurements: mean cell size, circular mean of the clicked points for the position.
// Hexes: the angle between the two centres tells pointy-top (0°) from flat-top (30°).
Calibration gridFromMeasurements(const QVector<Measurement> &m, bool hex);

// Panel shown over the canvas while the Calibrate tool is active.
class Calibrator : public Panel {
    Q_OBJECT
public:
    explicit Calibrator(Canvas *canvas);
    void start(MapItem *selected);
    void stop();                   // drops a result that was not applied
    void mousePress(QPointF scenePos);
    bool key(QKeyEvent *e);
    void apply();
    void autoDetect();
    void setHex(bool hex);

    MapItem *target() const { return map; }
    const std::optional<Calibration> &current() const { return result; }
    bool confirmed() const { return matched; }
    int measured() const { return int(measurements.size()); }
    bool busy() const { return waitingAnalysis || detecting; }

private:
    void choose(MapItem *m);
    void reset();
    void measureAgain();
    void evaluate();
    void showStep();
    void setResult(const Calibration &c, bool fromLines, const QString &how);
    void requestAnalysis();
    void analysisReady(const QString &path);
    void runDetection();
    void clearMarks();
    QGraphicsItem *mark(QGraphicsItem *item);
    QRectF quarter(int q) const;
    void setText(const QString &s);

    Canvas *canvas;
    QPointer<MapItem> map;
    QVector<Measurement> measurements;
    std::optional<QPointF> pointA;
    std::optional<Calibration> result;
    QString how;
    bool hex = false, matched = false, waitingAnalysis = false, detecting = false;
    QList<QGraphicsItem *> marks;   // children of the map: deleted with it

    QLabel *title, *instruction, *progress, *unitsLabel;
    MenuButton *squaresButton, *hexButton, *detectButton, *applyButton, *againButton;
    QDoubleSpinBox *perCell;
    QLineEdit *unit;
};
