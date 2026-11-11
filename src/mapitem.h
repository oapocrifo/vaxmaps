#pragma once
#include <QGraphicsObject>
#include <QImage>
#include <QSharedPointer>

// Content pixels per tile. Tile files carry 1 px from each neighbour so bilinear filtering shows no seams.
constexpr int TILE = 510;

struct Calibration {
    double cellPx = 0;          // grid cell size in image pixels, 0 = not calibrated
    QPointF origin;             // one grid intersection, in image pixels
    double unitsPerCell = 1.5;
    QString unit = "m";
    bool showGrid = false;
    bool valid() const { return cellPx > 0; }
    // a and b: two grid intersections on the same row or column, `cells` squares apart.
    static Calibration fromPoints(QPointF a, QPointF b, double cells, double unitsPerCell, const QString &unit);
};

// One per source image, shared by every MapItem that shows it.
// Holds a tile pyramid on disk (level 0 = full size, each level half the previous one)
// and keeps only the smallest level (`overview`) in memory.
class MapData : public QObject {
    Q_OBJECT
public:
    static QSharedPointer<MapData> get(const QString &path);
    static void shutdown();
    static QString cacheRoot();

    QString path, dir, ext, error;
    QSize size;          // full image size
    int levels = 0;      // levels - 1 is the overview
    QImage overview;
    bool ready = false;
    QSize levelSize(int level) const;
signals:
    void changed();
};

class MapItem : public QGraphicsObject {
    Q_OBJECT
public:
    enum { Type = UserType + 1 };
    explicit MapItem(const QString &path);
    int type() const override { return Type; }
    QRectF boundingRect() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *opt, QWidget *w) override;
    static int levelFor(double screenScale, int levels);

    QSharedPointer<MapData> src;
    Calibration cal;

private:
    QRectF tileRect(int level, int tx, int ty) const;
    bool drawTile(QPainter *p, int level, int tx, int ty, bool request);
    void drawFallback(QPainter *p, int level, int tx, int ty);
    QSize shownSize;
};

// Decoded tiles waiting or loading (for tests and benchmarks).
int tileLoadsBusy();
qint64 tileCacheBytes();
