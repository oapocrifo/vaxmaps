#include "grid.h"

#include <QColor>
#include <QLineF>
#include <QPainter>
#include <QRectF>
#include <cmath>

namespace {

constexpr double SQRT3 = 1.7320508075688772;

// Hex vertices in lattice coordinates around a centre (any basis of two neighbours 60° apart).
const QPointF hexVertex[6] = {{1.0 / 3, -2.0 / 3}, {2.0 / 3, -1.0 / 3}, {1.0 / 3, 1.0 / 3},
                              {-1.0 / 3, 2.0 / 3}, {-2.0 / 3, 1.0 / 3}, {-1.0 / 3, -1.0 / 3}};

QPoint hexRound(double u, double v) {
    const double x = u, z = v, y = -u - v;
    double rx = std::round(x), ry = std::round(y), rz = std::round(z);
    const double dx = std::abs(rx - x), dy = std::abs(ry - y), dz = std::abs(rz - z);
    if (dx > dy && dx > dz) rx = -ry - rz;
    else if (dy <= dz) rz = -rx - ry;
    return QPoint(int(rx), int(rz));
}

} // namespace

Calibration Calibration::square(double cell, QPointF corner) {
    Calibration c;
    c.grid = Square;
    c.origin = corner;
    c.a = QPointF(cell, 0);
    c.b = QPointF(0, cell);
    return c;
}

Calibration Calibration::hex(Grid g, double w, QPointF centre) {
    Calibration c;
    c.grid = g;
    c.origin = centre;
    if (g == HexPointy) { c.a = QPointF(w, 0); c.b = QPointF(w / 2, w * SQRT3 / 2); }
    else { c.a = QPointF(w * SQRT3 / 2, w / 2); c.b = QPointF(0, w); }
    return c;
}

bool Calibration::valid() const { return std::abs(a.x() * b.y() - a.y() * b.x()) > 1e-6; }

double Calibration::cellSize() const { return std::hypot(a.x(), a.y()); }

void Calibration::scaleCell(double size) {
    if (!valid() || size <= 0) return;
    const double f = size / cellSize();
    a *= f;
    b *= f;
    field.clear();
    fieldCols = fieldRows = 0;
}

QPointF Calibration::offset(QPointF p) const {
    if (field.isEmpty()) return {};
    // Control points sit at region centres: up to half a region beyond them (the map's edge) the trend continues.
    const double fx = qBound(-0.5, (p.x() - fieldOrigin.x()) / fieldStep.x(), fieldCols - 0.5);
    const double fy = qBound(-0.5, (p.y() - fieldOrigin.y()) / fieldStep.y(), fieldRows - 0.5);
    const int i = qBound(0, int(std::floor(fx)), std::max(0, fieldCols - 2)), j = qBound(0, int(std::floor(fy)), std::max(0, fieldRows - 2));
    const double tx = fieldCols > 1 ? fx - i : 0, ty = fieldRows > 1 ? fy - j : 0;
    const int i1 = std::min(i + 1, fieldCols - 1), j1 = std::min(j + 1, fieldRows - 1);
    auto at = [&](int x, int y) { return field[y * fieldCols + x]; };
    return (at(i, j) * (1 - tx) + at(i1, j) * tx) * (1 - ty) + (at(i, j1) * (1 - tx) + at(i1, j1) * tx) * ty;
}

QPointF Calibration::toGrid(QPointF p) const {
    const QPointF q = p - offset(p) - origin;
    const double det = a.x() * b.y() - a.y() * b.x();
    return QPointF((q.x() * b.y() - q.y() * b.x()) / det, (a.x() * q.y() - a.y() * q.x()) / det);
}

QPointF Calibration::fromGrid(QPointF uv) const {
    const QPointF q = origin + uv.x() * a + uv.y() * b;
    QPointF p = q;
    for (int i = 0; i < 3 && !field.isEmpty(); ++i) p = q + offset(p);   // the correction is defined where the point lands
    return p;
}

QPoint Calibration::cellAt(QPointF p) const {
    const QPointF uv = toGrid(p);
    return isHex() ? hexRound(uv.x(), uv.y()) : QPoint(int(std::floor(uv.x())), int(std::floor(uv.y())));
}

QPointF Calibration::cellCentre(QPoint c) const {
    return fromGrid(isHex() ? QPointF(c) : QPointF(c) + QPointF(0.5, 0.5));
}

QPointF Calibration::nearestCorner(QPointF p) const {
    const QPointF uv = toGrid(p);
    if (!isHex()) return fromGrid(QPointF(std::round(uv.x()), std::round(uv.y())));
    const QPoint c = hexRound(uv.x(), uv.y());
    QPointF best;
    double bestDist = 1e300;
    for (const QPointF &v : hexVertex) {
        const QPointF cand = fromGrid(QPointF(c) + v);
        const double d = QLineF(cand, p).length();
        if (d < bestDist) { bestDist = d; best = cand; }
    }
    return best;
}

int Calibration::steps(QPoint from, QPoint to) const {
    const int du = to.x() - from.x(), dv = to.y() - from.y();
    if (!isHex()) return std::max(std::abs(du), std::abs(dv));
    return (std::abs(du) + std::abs(dv) + std::abs(du + dv)) / 2;
}

QString Calibration::noun(int count) const {
    if (isHex()) return count == 1 ? "hex" : "hexes";
    return count == 1 ? "square" : "squares";
}

void drawGrid(QPainter *p, const Calibration &c, const QRectF &ex, double s, const QColor &color) {
    if (!c.valid() || c.cellSize() * s < (c.isHex() ? 12 : 6)) return;
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
    for (QPointF corner : {ex.topLeft(), ex.topRight(), ex.bottomLeft(), ex.bottomRight()}) {
        const QPointF uv = c.toGrid(corner);
        u0 = std::min(u0, uv.x()); u1 = std::max(u1, uv.x());
        v0 = std::min(v0, uv.y()); v1 = std::max(v1, uv.y());
    }
    const int i0 = int(std::floor(u0)) - 1, i1 = int(std::ceil(u1)) + 1, j0 = int(std::floor(v0)) - 1, j1 = int(std::ceil(v1)) + 1;
    if (double(i1 - i0) * (j1 - j0) > 250000) return;
    p->save();
    p->setClipRect(ex, Qt::IntersectClip);   // lines stop at the map's edge
    p->setPen(QPen(color, 0));
    p->setBrush(Qt::NoBrush);
    if (!c.isHex()) {
        const bool straight = c.field.isEmpty();
        for (int i = i0; i <= i1; ++i) {
            QPolygonF line;
            for (int j = j0; j <= j1; j += straight ? j1 - j0 : 1) line << c.fromGrid(QPointF(i, j));
            p->drawPolyline(line);
        }
        for (int j = j0; j <= j1; ++j) {
            QPolygonF line;
            for (int i = i0; i <= i1; i += straight ? i1 - i0 : 1) line << c.fromGrid(QPointF(i, j));
            p->drawPolyline(line);
        }
        p->restore();
        return;
    }
    // Three edges per hex cover every edge exactly once; the local correction is taken at the centre.
    QPointF corner[4];
    for (int k = 0; k < 4; ++k) corner[k] = hexVertex[k].x() * c.a + hexVertex[k].y() * c.b;
    QVector<QLineF> lines;
    lines.reserve((i1 - i0 + 1) * (j1 - j0 + 1) * 3);
    for (int i = i0; i <= i1; ++i)
        for (int j = j0; j <= j1; ++j) {
            const QPointF q = c.origin + i * c.a + j * c.b;
            const QPointF centre = q + c.offset(q);
            if (!ex.adjusted(-c.cellSize(), -c.cellSize(), c.cellSize(), c.cellSize()).contains(centre)) continue;
            for (int k = 0; k < 3; ++k) lines << QLineF(centre + corner[k], centre + corner[k + 1]);
        }
    p->drawLines(lines);
    p->restore();
}
