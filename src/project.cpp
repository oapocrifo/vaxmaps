#include "project.h"
#include "items.h"
#include "mapitem.h"

#include <QGraphicsScene>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>

namespace {

QJsonArray pointsToJson(const QVector<QPointF> &pts) {
    QJsonArray a;
    for (const QPointF &p : pts) a << p.x() << p.y();
    return a;
}

QVector<QPointF> pointsFromJson(const QJsonArray &a) {
    QVector<QPointF> pts;
    for (int i = 0; i + 1 < a.size(); i += 2) pts << QPointF(a[i].toDouble(), a[i + 1].toDouble());
    return pts;
}

const char *gridNames[] = {"square", "hexPointy", "hexFlat"};
const int formatVersion = 3;

QJsonObject calToJson(const Calibration &c) {
    QJsonObject o{{"type", gridNames[c.grid]}, {"ox", c.origin.x()}, {"oy", c.origin.y()}, {"ax", c.a.x()}, {"ay", c.a.y()},
                  {"bx", c.b.x()}, {"by", c.b.y()}, {"perCell", c.unitsPerCell}, {"unit", c.unit}, {"show", c.showGrid}};
    if (c.diagonal != Calibration::One) o["diag"] = c.diagonal == Calibration::Alternating ? "alternating" : "straight";
    if (!c.field.isEmpty())
        o["field"] = QJsonObject{{"x0", c.fieldOrigin.x()}, {"y0", c.fieldOrigin.y()}, {"dx", c.fieldStep.x()}, {"dy", c.fieldStep.y()},
                                 {"cols", c.fieldCols}, {"rows", c.fieldRows}, {"d", pointsToJson(c.field)}};
    return o;
}

Calibration calFromJson(const QJsonObject &o, int version) {
    Calibration c;
    if (o.contains("cell")) {   // v1-v2: squares only
        c = Calibration::square(o["cell"].toDouble(), QPointF(o["ox"].toDouble(), o["oy"].toDouble()));
        c.showGrid = o["grid"].toBool() && version >= 2;   // v1 turned the grid on by itself after every calibration
    } else {
        const QString t = o["type"].toString();
        c.grid = t == "hexPointy" ? Calibration::HexPointy : t == "hexFlat" ? Calibration::HexFlat : Calibration::Square;
        c.origin = QPointF(o["ox"].toDouble(), o["oy"].toDouble());
        c.a = QPointF(o["ax"].toDouble(), o["ay"].toDouble());
        c.b = QPointF(o["bx"].toDouble(), o["by"].toDouble());
        c.showGrid = o["show"].toBool();
        const QJsonObject f = o["field"].toObject();
        const QVector<QPointF> d = pointsFromJson(f["d"].toArray());
        const int cols = f["cols"].toInt(), rows = f["rows"].toInt();
        if (cols > 0 && rows > 0 && d.size() == cols * rows && f["dx"].toDouble() > 0 && f["dy"].toDouble() > 0) {
            c.fieldCols = cols;
            c.fieldRows = rows;
            c.fieldOrigin = QPointF(f["x0"].toDouble(), f["y0"].toDouble());
            c.fieldStep = QPointF(f["dx"].toDouble(), f["dy"].toDouble());
            c.field = d;
        }
    }
    c.unitsPerCell = o["perCell"].toDouble(1.5);
    c.unit = o["unit"].toString("m");
    const QString diag = o["diag"].toString();
    c.diagonal = diag == "alternating" ? Calibration::Alternating : diag == "straight" ? Calibration::Straight : Calibration::One;
    return c;
}

QJsonObject itemToJson(QGraphicsItem *it, const QDir &base, bool strokePoints = true) {
    QJsonObject o{{"x", it->pos().x()}, {"y", it->pos().y()}};
    if (auto *m = qgraphicsitem_cast<MapItem *>(it)) {
        o["type"] = "map";
        o["path"] = base.relativeFilePath(m->src->path);
        if (m->cal.valid()) o["cal"] = calToJson(m->cal);
        if (!m->fog.isNull()) o["fog"] = QString::fromLatin1(m->fogPng.toBase64());
    } else if (it->type() == LinkItem::Type) {
        auto *l = static_cast<LinkItem *>(it);
        o["type"] = "link";
        o["id"] = l->id;
        o["partner"] = l->partner ? l->partner->id : QString();
        o["pts"] = pointsToJson(l->pts);
        o["name"] = l->name;
        o["color"] = l->color.name(QColor::HexArgb);
    } else if (auto *s = qgraphicsitem_cast<StrokeItem *>(it)) {
        o["type"] = "stroke";
        o["color"] = s->pen().color().name(QColor::HexArgb);
        o["width"] = s->pen().widthF();
        if (strokePoints) o["pts"] = pointsToJson(s->pts);
    } else if (auto *k = qgraphicsitem_cast<TokenItem *>(it)) {
        o["type"] = "token";
        o["name"] = k->name;
        o["color"] = k->color.name(QColor::HexArgb);
        o["d"] = k->diameter;
        o["cells"] = k->cells;
    } else if (auto *t = qgraphicsitem_cast<TextItem *>(it)) {
        if (t->toPlainText().trimmed().isEmpty()) return {};   // just created, nothing typed yet: it deletes itself when the edit ends
        o["type"] = t->note ? "note" : "text";
        o["text"] = t->toPlainText();
        o["color"] = t->color.name(QColor::HexArgb);
        o["scale"] = t->scale();
    } else if (auto *z = qgraphicsitem_cast<ZoneItem *>(it)) {
        o["type"] = "zone";
        o["shape"] = z->shapeKind == ZoneItem::Rect ? "rect" : z->shapeKind == ZoneItem::Ellipse ? "ellipse" : "poly";
        o["pts"] = pointsToJson(z->pts);
        o["name"] = z->name;
        o["desc"] = z->desc;
        o["color"] = z->color.name(QColor::HexArgb);
    } else {
        return {};
    }
    return o;
}

QGraphicsItem *itemFromJson(const QJsonObject &o, const QDir &base, int version) {
    const QString type = o["type"].toString();
    QGraphicsItem *it = nullptr;
    if (type == "map") {
        auto *m = new MapItem(base.absoluteFilePath(o["path"].toString()));
        if (const QByteArray png = QByteArray::fromBase64(o["fog"].toString().toLatin1()); !png.isEmpty()) {
            m->fog = QImage::fromData(png, "png").convertToFormat(QImage::Format_Alpha8);
            if (!m->fog.isNull()) m->fogPng = png;   // kept as read: undo steps do not encode it again
        }
        const QJsonObject c = o["cal"].toObject();
        if (!c.isEmpty()) m->cal = calFromJson(c, version);
        it = m;
    } else if (type == "link") {
        const QVector<QPointF> p = pointsFromJson(o["pts"].toArray());
        if (p.size() != 2) return nullptr;
        auto *l = new LinkItem(QRectF(p[0], p[1]), QColor(o["color"].toString()));
        l->id = o["id"].toString();
        l->name = o["name"].toString();
        l->setData(0, o["partner"].toString());   // resolved once every link is loaded
        it = l;
    } else if (type == "stroke") {
        auto *s = new StrokeItem(QColor(o["color"].toString()), o["width"].toDouble(4));
        s->setPoints(pointsFromJson(o["pts"].toArray()));
        it = s;
    } else if (type == "token") {
        const double d = o["d"].toDouble();
        if (!(d > 0)) return nullptr;
        it = new TokenItem(o["name"].toString(), QColor(o["color"].toString()), d, qBound(1, o["cells"].toInt(1), 4));
    } else if (type == "text" || type == "note") {
        auto *t = new TextItem(type == "note", QColor(o["color"].toString()));
        t->setPlainText(o["text"].toString());
        t->setScale(o["scale"].toDouble(1));
        it = t;
    } else if (type == "zone") {
        const QString shape = o["shape"].toString();
        auto *z = new ZoneItem(shape == "rect" ? ZoneItem::Rect : shape == "ellipse" ? ZoneItem::Ellipse : ZoneItem::Poly,
                               QPolygonF(pointsFromJson(o["pts"].toArray())), QColor(o["color"].toString()));
        if (z->shapeKind == ZoneItem::Poly ? z->pts.size() < 3 : z->pts.size() != 2) { delete z; return nullptr; }
        z->name = o["name"].toString();
        z->setDesc(o["desc"].toString());
        it = z;
    }
    if (it) it->setPos(o["x"].toDouble(), o["y"].toDouble());
    return it;
}

} // namespace

int projectFormatVersion() { return formatVersion; }

QJsonArray itemsToJson(const QList<QGraphicsItem *> &items, const QDir &base) {
    QJsonArray a;
    for (QGraphicsItem *it : items)
        if (QJsonObject o = itemToJson(it, base); !o.isEmpty()) a << o;
    return a;
}

QList<QGraphicsItem *> itemsFromJson(const QJsonArray &a, const QDir &base) {
    QList<QGraphicsItem *> items;
    for (const QJsonValue &v : a)
        if (QGraphicsItem *it = itemFromJson(v.toObject(), base, formatVersion)) items << it;
    return items;
}

QJsonObject layersToJson(const QList<LayerItem *> &layers, const QDir &base) {
    QJsonArray arr;
    for (LayerItem *l : layers)   // children come back in stacking order
        arr << QJsonObject{{"name", l->name}, {"visible", l->isVisible()}, {"locked", l->locked}, {"links", l->links},
                           {"items", itemsToJson(l->childItems(), base)}};
    return QJsonObject{{"format", "vaxmaps"}, {"version", formatVersion}, {"layers", arr}};
}

QList<QByteArray> layersToParts(const QList<LayerItem *> &layers) {
    // 'L' + a layer without its items, then its items; 'P' + the points of the stroke just before.
    QList<QByteArray> parts;
    for (LayerItem *l : layers) {
        parts << 'L' + QJsonDocument(QJsonObject{{"name", l->name}, {"visible", l->isVisible()}, {"locked", l->locked}, {"links", l->links}})
                           .toJson(QJsonDocument::Compact);
        for (QGraphicsItem *c : l->childItems()) {
            auto *s = qgraphicsitem_cast<StrokeItem *>(c);
            if (!s) {
                if (const QJsonObject o = itemToJson(c, QDir::root()); !o.isEmpty()) parts << QJsonDocument(o).toJson(QJsonDocument::Compact);
                continue;
            }
            if (s->jsonHead.isEmpty() || s->jsonPos != s->pos() || s->jsonPen != s->pen()) {
                s->jsonPos = s->pos();
                s->jsonPen = s->pen();
                s->jsonHead = QJsonDocument(itemToJson(s, QDir::root(), false)).toJson(QJsonDocument::Compact);
            }
            parts << s->jsonHead;
            // Compared, not invalidated by setPoints: pts can also change without it. While nothing changed both share
            // one buffer and Qt compares the pointers only.
            if (s->jsonPart.isEmpty() || s->jsonPts != s->pts) {
                s->jsonPts = s->pts;
                s->jsonPart = 'P' + QJsonDocument(pointsToJson(s->pts)).toJson(QJsonDocument::Compact);
            }
            parts << s->jsonPart;
        }
    }
    return parts;
}

QJsonObject layersFromParts(const QList<QByteArray> &parts) {
    QByteArray doc = QByteArray(R"({"format":"vaxmaps","version":)") + QByteArray::number(formatVersion) + R"(,"layers":[)";
    bool firstLayer = true, firstItem = true;
    for (const QByteArray &part : parts) {
        if (part.startsWith('L')) {
            if (!firstLayer) doc += "]},";
            doc += part.mid(1).chopped(1) + R"(,"items":[)";   // the layer object, open again for its items
            firstLayer = false;
            firstItem = true;
        } else if (part.startsWith('P')) {
            doc.chop(1);   // the stroke's closing brace
            doc += R"(,"pts":)" + part.mid(1) + '}';
        } else {
            if (!firstItem) doc += ',';
            doc += part;
            firstItem = false;
        }
    }
    if (!firstLayer) doc += "]}";
    doc += "]}";
    return QJsonDocument::fromJson(doc).object();
}

void reuseParts(const QList<LayerItem *> &layers, const QList<QByteArray> &parts) {
    QList<StrokeItem *> strokes;
    for (LayerItem *l : layers)
        for (QGraphicsItem *c : l->childItems())
            if (auto *s = qgraphicsitem_cast<StrokeItem *>(c)) strokes << s;
    QList<int> at;   // index of each stroke's points piece; its head is the piece before
    for (int i = 1; i < parts.size(); ++i)
        if (parts[i].startsWith('P')) at << i;
    if (at.size() != strokes.size()) return;   // not the same strokes: they build their pieces again, which is only slower
    for (int k = 0; k < strokes.size(); ++k) {
        StrokeItem *s = strokes[k];
        s->jsonPts = s->pts;
        s->jsonPart = parts[at[k]];
        s->jsonPos = s->pos();
        s->jsonPen = s->pen();
        s->jsonHead = parts[at[k] - 1];
    }
}

QList<LayerItem *> layersFromJson(const QJsonObject &o, QGraphicsScene *scene, const QDir &base) {
    QList<LayerItem *> layers;
    const int version = o["version"].toInt(1);
    QHash<QString, LinkItem *> links;
    for (const QJsonValue &lv : o["layers"].toArray()) {
        const QJsonObject lo = lv.toObject();
        auto *l = new LayerItem(lo["name"].toString());
        l->links = lo["links"].toBool();
        scene->addItem(l);
        l->setZValue(layers.size());
        for (const QJsonValue &iv : lo["items"].toArray())
            if (QGraphicsItem *it = itemFromJson(iv.toObject(), base, version)) {
                if (it->type() == LinkItem::Type && !l->links) { delete it; continue; }   // links live only in the links layer
                it->setParentItem(l);
                if (it->type() == LinkItem::Type) links.insert(static_cast<LinkItem *>(it)->id, static_cast<LinkItem *>(it));
            }
        if (!l->links) l->setVisible(lo["visible"].toBool(true));
        l->setLocked(lo["locked"].toBool());
        layers << l;
    }
    for (LinkItem *l : links) {
        LinkItem *p = links.value(l->data(0).toString());
        if (p && p != l && !p->partner) { l->partner = p; p->partner = l; }
        l->setData(0, QVariant());
    }
    return layers;
}
