#include "mapitem.h"

#include <QApplication>
#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStyleOptionGraphicsItem>
#include <QThreadPool>
#include <QWidget>
#include <atomic>
#include <cmath>
#include <deque>

namespace {

std::atomic<bool> quitting{false};
QHash<QString, QSharedPointer<MapData>> registry;

// One build at a time bounds peak memory: decoding a 10000 x 10000 map takes 400 MB.
QThreadPool *builderPool() {
    static QThreadPool *p = [] { auto *t = new QThreadPool(qApp); t->setMaxThreadCount(1); return t; }();
    return p;
}

QThreadPool *loaderPool() {
    static QThreadPool *p = [] {
        auto *t = new QThreadPool(qApp);
        t->setMaxThreadCount(qBound(2, QThread::idealThreadCount() - 1, 6));
        return t;
    }();
    return p;
}

QImage::Format displayFormat(const QImage &img) {
    return img.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
}

// ---- tile loader: LRU of decoded tiles shared by all maps, newest request first ----

struct Request { QString key; QSharedPointer<MapData> map; int level, tx, ty; qint64 at; };

struct Loader {
    QCache<QString, QImage> cache{1024 * 1024};   // cost in KB: 1 GB of decoded tiles
    std::deque<Request> pending;
    QSet<QString> inFlight, failed;
    QElapsedTimer clock;
    bool pumpQueued = false;
    Loader() { clock.start(); }
};

Loader &loader() { static Loader *l = new Loader; return *l; }

QString tileFile(const MapData *m, int level, int tx, int ty) {
    return QString("%1/%2_%3_%4.%5").arg(m->dir).arg(level).arg(tx).arg(ty).arg(m->ext);
}

void pump() {
    Loader &ld = loader();
    ld.pumpQueued = false;
    const int maxInFlight = loaderPool()->maxThreadCount() * 2;
    while (ld.inFlight.size() < maxInFlight && !ld.pending.empty()) {
        Request r = ld.pending.front();
        ld.pending.pop_front();
        if (ld.clock.elapsed() - r.at > 1500) continue;   // not painted lately: the view moved on
        ld.inFlight.insert(r.key);
        const QString file = tileFile(r.map.data(), r.level, r.tx, r.ty);
        loaderPool()->start([r, file] {
            QImage img(file);
            if (!img.isNull()) img = img.convertToFormat(displayFormat(img));
            QMetaObject::invokeMethod(qApp, [r, img] {
                Loader &ld = loader();
                ld.inFlight.remove(r.key);
                if (img.isNull()) ld.failed.insert(r.key);
                else ld.cache.insert(r.key, new QImage(img), qMax<qsizetype>(1, img.sizeInBytes() / 1024));
                emit r.map->changed();
                pump();
            }, Qt::QueuedConnection);
        });
    }
}

const QImage *tileImage(const QSharedPointer<MapData> &m, int level, int tx, int ty, bool request) {
    Loader &ld = loader();
    const QString key = QString("%1/%2/%3/%4").arg(m->dir).arg(level).arg(tx).arg(ty);
    if (QImage *img = ld.cache.object(key)) return img;
    if (!request || ld.inFlight.contains(key) || ld.failed.contains(key)) return nullptr;
    auto it = std::find_if(ld.pending.begin(), ld.pending.end(), [&](const Request &r) { return r.key == key; });
    if (it != ld.pending.end()) ld.pending.erase(it);
    ld.pending.push_front({key, m, level, tx, ty, ld.clock.elapsed()});
    if (ld.pending.size() > 512) ld.pending.pop_back();
    if (!ld.pumpQueued) {   // queue the whole frame's requests before dispatching
        ld.pumpQueued = true;
        QMetaObject::invokeMethod(qApp, pump, Qt::QueuedConnection);
    }
    return nullptr;
}

// ---- pyramid build (worker thread) ----

struct Built { QSize size; int levels = 0; QString ext; QImage overview; QString error; };

bool opaque(const QImage &img) {
    if (!img.hasAlphaChannel()) return true;
    const QImage a = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            if (qAlpha(line[x]) != 255) return false;
    }
    return true;
}

Built buildPyramid(const QString &path, const QString &dir) {
    Built b;
    if (quitting) { b.error = "cancelled"; return b; }   // queued behind another build when the program was closed
    QImageReader reader(path);
    QImage img = reader.read();
    if (img.isNull()) { b.error = reader.errorString(); return b; }
    img.convertTo(opaque(img) ? QImage::Format_RGB32 : QImage::Format_ARGB32_Premultiplied);   // in place: no second full-size copy
    b.size = img.size();
    b.ext = "png";   // lossless: JPEG tiles showed block and colour artifacts at high zoom
    if (!QDir().mkpath(dir)) { b.error = "cannot create " + dir; return b; }
    for (int level = 0;; ++level) {
        if (img.width() <= TILE && img.height() <= TILE) {
            b.overview = img;
            b.levels = level + 1;
            if (!img.save(dir + "/overview.png")) { b.error = "cannot write the cache"; return b; }
            break;
        }
        for (int ty = 0; ty * TILE < img.height(); ++ty) {
            if (quitting) { b.error = "cancelled"; return b; }   // per row: closing does not wait for a whole level of a big map
            for (int tx = 0; tx * TILE < img.width(); ++tx) {
                const QRect r = QRect(tx * TILE - 1, ty * TILE - 1, TILE + 2, TILE + 2) & img.rect();
                const QString f = QString("%1/%2_%3_%4.%5").arg(dir).arg(level).arg(tx).arg(ty).arg(b.ext);
                if (!img.copy(r).save(f, nullptr, 80)) { b.error = "cannot write the cache"; return b; }   // PNG quality 80 = zlib level 1: fast
            }
        }
        img = img.scaled((img.width() + 1) / 2, (img.height() + 1) / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    // meta.json is written last: its presence marks a complete pyramid.
    QSaveFile meta(dir + "/meta.json");
    if (!meta.open(QIODevice::WriteOnly)) { b.error = "cannot write the cache"; return b; }
    meta.write(QJsonDocument(QJsonObject{{"w", b.size.width()}, {"h", b.size.height()},
                                         {"levels", b.levels}, {"ext", b.ext}}).toJson());
    if (!meta.commit()) b.error = "cannot write the cache";
    return b;
}

} // namespace

int tileLoadsBusy() { return int(loader().pending.size()) + int(loader().inFlight.size()) + (loader().pumpQueued ? 1 : 0); }
qint64 tileCacheBytes() { return qint64(loader().cache.totalCost()) * 1024; }

QString MapData::cacheRoot() {
    static const QString root = [] {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        // v0.1 kept JPEG tiles in "tiles": drop them in the background.
        if (QDir(base + "/tiles").exists())
            QThreadPool::globalInstance()->start([base] { QDir(base + "/tiles").removeRecursively(); });
        return base + "/tiles2";
    }();
    return root;
}

QSize MapData::levelSize(int level) const {
    QSize s = size;
    for (int i = 0; i < level; ++i) s = QSize((s.width() + 1) / 2, (s.height() + 1) / 2);
    return s;
}

QSharedPointer<MapData> MapData::get(const QString &path) {
    const QFileInfo fi(path);
    const QString abs = fi.absoluteFilePath();
    // A map that failed (file missing when the project was opened) is tried again the next time it is asked for.
    if (auto m = registry.value(abs); m && m->error.isEmpty()) return m;
    auto m = QSharedPointer<MapData>::create();
    m->path = abs;
    registry.insert(abs, m);
    if (!fi.isFile()) { m->error = "file not found"; return m; }
    m->size = QImageReader(abs).size();
    const QByteArray id = (abs + '|' + QString::number(fi.size()) + '|'
                           + QString::number(fi.lastModified().toMSecsSinceEpoch())).toUtf8();
    m->dir = cacheRoot() + "/" + QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex().left(20);

    QFile metaFile(m->dir + "/meta.json");
    if (metaFile.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(metaFile.readAll()).object();
        const QImage ov(m->dir + "/overview.png");
        if (!ov.isNull() && o["levels"].toInt() > 0) {
            m->size = QSize(o["w"].toInt(), o["h"].toInt());
            m->levels = o["levels"].toInt();
            m->ext = o["ext"].toString();
            m->overview = ov.convertToFormat(displayFormat(ov));
            m->ready = true;
            return m;
        }
    }
    const QString dir = m->dir;
    builderPool()->start([m, abs, dir] {
        const Built b = buildPyramid(abs, dir);
        QMetaObject::invokeMethod(qApp, [m, b] {
            if (!b.error.isEmpty()) {
                m->error = b.error;
            } else {
                m->size = b.size;
                m->levels = b.levels;
                m->ext = b.ext;
                m->overview = b.overview.convertToFormat(displayFormat(b.overview));
                m->ready = true;
            }
            emit m->changed();
        }, Qt::QueuedConnection);
    });
    return m;
}

void MapData::shutdown() {
    quitting = true;
    builderPool()->waitForDone();
    loaderPool()->waitForDone();
    QThreadPool::globalInstance()->waitForDone();   // grid analysis jobs post back to qApp
    registry.clear();
}

// ---- MapItem ----

MapItem::MapItem(const QString &path) : src(MapData::get(path)), shownSize(src->size) {
    setFlags(ItemIsSelectable | ItemIsMovable | ItemUsesExtendedStyleOption);
    connect(src.data(), &MapData::changed, this, [this] {
        if (src->size != shownSize) { prepareGeometryChange(); shownSize = src->size; }
        update();
    });
}

QRectF MapItem::boundingRect() const {
    return QRectF(QPointF(0, 0), shownSize.isEmpty() ? QSizeF(512, 512) : QSizeF(shownSize));
}

int MapItem::levelFor(double screenScale, int levels) {
    if (screenScale >= 1 || levels <= 1) return 0;
    return qBound(0, int(std::floor(std::log2(1.0 / screenScale))), levels - 1);
}

QRectF MapItem::tileRect(int level, int tx, int ty) const {
    const QSize ls = src->levelSize(level);
    const double fx = src->size.width() / double(ls.width()), fy = src->size.height() / double(ls.height());
    const int cx = tx * TILE, cy = ty * TILE;
    return QRectF(cx * fx, cy * fy, qMin(TILE, ls.width() - cx) * fx, qMin(TILE, ls.height() - cy) * fy);
}

bool MapItem::drawTile(QPainter *p, int level, int tx, int ty, bool request) {
    const QImage *img = tileImage(src, level, tx, ty, request);
    if (!img) return false;
    const QSize ls = src->levelSize(level);
    const int cx = tx * TILE, cy = ty * TILE;
    const QRectF content(cx - qMax(0, cx - 1), cy - qMax(0, cy - 1), qMin(TILE, ls.width() - cx), qMin(TILE, ls.height() - cy));
    p->drawImage(tileRect(level, tx, ty), *img, content);
    return true;
}

// While a tile loads, show the closest coarser level already in memory (PureRef shows a lower mip the same way).
void MapItem::drawFallback(QPainter *p, int level, int tx, int ty) {
    const QRectF r = tileRect(level, tx, ty);
    p->save();
    p->setClipRect(r, Qt::IntersectClip);
    for (int up = level + 1; up < src->levels; ++up) {
        if (up == src->levels - 1) { p->drawImage(boundingRect(), src->overview); break; }
        const QSize ls = src->levelSize(up);
        const double fx = src->size.width() / double(ls.width()), fy = src->size.height() / double(ls.height());
        const int ax0 = int(r.left() / fx) / TILE, ax1 = qMin(int(std::ceil(r.right() / fx)) - 1, ls.width() - 1) / TILE;
        const int ay0 = int(r.top() / fy) / TILE, ay1 = qMin(int(std::ceil(r.bottom() / fy)) - 1, ls.height() - 1) / TILE;
        bool all = true;
        for (int ay = ay0; ay <= ay1 && all; ++ay)
            for (int ax = ax0; ax <= ax1 && all; ++ax) all = tileImage(src, up, ax, ay, false) != nullptr;
        if (!all) continue;
        for (int ay = ay0; ay <= ay1; ++ay)
            for (int ax = ax0; ax <= ax1; ++ax) drawTile(p, up, ax, ay, false);
        break;
    }
    p->restore();
}

void MapItem::paint(QPainter *p, const QStyleOptionGraphicsItem *opt, QWidget *w) {
    const QRectF full = boundingRect();
    p->setRenderHint(QPainter::Antialiasing, false);   // antialiased image edges leave seams between tiles
    const double s = QStyleOptionGraphicsItem::levelOfDetailFromTransform(p->worldTransform())
                     * (w ? w->devicePixelRatioF() : 1.0);
    if (!src->ready) {
        p->fillRect(full, QColor(55, 55, 60));
        QFont f = p->font();
        f.setPixelSize(qMax(1, int(full.height() / 24)));
        p->setFont(f);
        p->setPen(src->error.isEmpty() ? QColor(220, 220, 220) : QColor(255, 120, 120));
        p->drawText(full, Qt::AlignCenter | Qt::TextWordWrap,
                    src->error.isEmpty() ? QString("Preparing map…\n%1").arg(QFileInfo(src->path).fileName())
                                         : QString("Error: %1\n%2").arg(src->error, src->path));
    } else {
        const int level = levelFor(s, src->levels);
        if (level == src->levels - 1) {
            p->drawImage(full, src->overview);
        } else {
            const QRectF ex = opt->exposedRect & full;
            const QSize ls = src->levelSize(level);
            const double fx = src->size.width() / double(ls.width()), fy = src->size.height() / double(ls.height());
            const int x0 = int(ex.left() / fx) / TILE, x1 = qMin(int(std::ceil(ex.right() / fx)) - 1, ls.width() - 1) / TILE;
            const int y0 = int(ex.top() / fy) / TILE, y1 = qMin(int(std::ceil(ex.bottom() / fy)) - 1, ls.height() - 1) / TILE;
            for (int ty = y0; ty <= y1; ++ty)
                for (int tx = x0; tx <= x1; ++tx)
                    if (!drawTile(p, level, tx, ty, true)) drawFallback(p, level, tx, ty);
        }
        const Calibration *g = preview ? &*preview : (cal.showGrid && cal.valid() ? &cal : nullptr);
        if (g) drawGrid(p, *g, opt->exposedRect & full, s, preview ? QColor(255, 220, 0, 230) : QColor(0, 230, 255, 150));
    }
    if (isSelected()) {
        p->setPen(QPen(QColor(80, 160, 255), 0, Qt::DashLine));
        p->setBrush(Qt::NoBrush);
        p->drawRect(full);
    }
}
