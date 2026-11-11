#pragma once
#include <QGraphicsObject>
#include <QImage>
#include <QSharedPointer>
#include <functional>
#include <optional>
#include "grid.h"

// Content pixels per tile. Tile files carry 1 px from each neighbour so bilinear filtering shows no seams.
constexpr int TILE = 510;


// One per source image, shared by every MapItem that shows it.
// Holds a tile pyramid on disk (level 0 = full size, each level half the previous one)
// and keeps only the smallest level (`overview`) in memory.
class MapData : public QObject {
    Q_OBJECT
public:
    static QSharedPointer<MapData> get(const QString &path);
    static void shutdown();
    static QString cacheRoot();
    // Deletes the pyramids of the maps not opened in this session, in the background. `done` gets the bytes freed.
    static void clearCache(std::function<void(qint64)> done);

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
    void setSource(const QString &path);   // another image file, same position and calibration
    int type() const override { return Type; }
    QRectF boundingRect() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *opt, QWidget *w) override;
    static int levelFor(double screenScale, int levels);

    QSharedPointer<MapData> src;
    Calibration cal;
    // Fog of war: 255 where the map is covered. At most 2048 px wide or high, stretched over the map.
    QImage fog;
    QByteArray fogPng;   // fog as saved in the project; made again when a brush stroke ends (setFog)
    void enableFog(bool covered);                         // a new fog, all covered or all revealed
    void setFog(const QImage &mask);                       // loaded from a project or an undo step
    void paintFog(QPointF a, QPointF b, double width, bool cover);   // item coordinates
    void fogStrokeDone();
    std::optional<Calibration> preview;   // grid being calibrated, drawn in yellow until applied

private:
    QRectF tileRect(int level, int tx, int ty) const;
    bool drawTile(QPainter *p, int level, int tx, int ty, bool request);
    void drawFallback(QPainter *p, int level, int tx, int ty);
    QSize shownSize;
};

extern bool fogOpaque;   // the GM's view: fog hides the map, or only darkens it
extern bool syncTiles;   // tiles load before painting returns (export at full resolution)

// Decoded tiles waiting or loading (for tests and benchmarks).
int tileLoadsBusy();
qint64 tileCacheBytes();
