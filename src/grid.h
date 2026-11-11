#pragma once
// A map's grid: squares or hexes, regular or following local variations of the drawn grid.
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QVector>

class QPainter;
class QRectF;
class QColor;

struct Calibration {
    enum Grid { Square, HexPointy, HexFlat };
    // Squares only: what a diagonal step costs. One: 1 (D&D 5e). Alternating: 1, 2, 1, 2... (5/10/5 ft).
    // Straight: the straight-line distance between the cell centres.
    enum Diagonal { One, Alternating, Straight };
    Grid grid = Square;
    Diagonal diagonal = One;
    QPointF origin;   // square: a corner where lines cross; hex: the centre of a hex. Image px.
    QPointF a, b;     // one lattice step along each axis, image px. Square: (c, 0) and (0, c); hex: to two neighbours 60° apart.
    double unitsPerCell = 1.5;
    QString unit = "m";
    bool showGrid = false;
    // Local corrections: how far the map's real grid sits from the regular one, sampled on a coarse lattice of
    // control points (bilinear in between). Empty means a perfectly regular grid.
    QPointF fieldOrigin, fieldStep;
    int fieldCols = 0, fieldRows = 0;
    QVector<QPointF> field;

    static Calibration square(double cell, QPointF corner);
    static Calibration hex(Grid g, double width, QPointF centre);   // width: distance between neighbouring centres

    bool valid() const;
    bool isHex() const { return grid != Square; }
    double cellSize() const;
    void scaleCell(double size);           // keeps the origin, drops local corrections
    QPointF offset(QPointF p) const;       // local correction at p
    QPointF toGrid(QPointF p) const;       // lattice coordinates (u, v)
    QPointF fromGrid(QPointF uv) const;
    QPoint cellAt(QPointF p) const;        // the square or hex containing p
    QPointF cellCentre(QPoint c) const;
    QPointF nearestCorner(QPointF p) const;   // square corner or hex vertex
    double steps(QPoint from, QPoint to) const;   // in cells: squares by the diagonal rule, hexes by hex distance
    QString noun(int count) const;
};

// Grid lines inside `exposed` (image px); skipped when cells are too small on screen.
void drawGrid(QPainter *p, const Calibration &c, const QRectF &exposed, double screenScale, const QColor &color);
