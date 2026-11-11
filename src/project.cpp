#include "project.h"
#include "items.h"
#include "mapitem.h"

#include <QGraphicsScene>
#include <QJsonArray>

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

QJsonObject itemToJson(QGraphicsItem *it, const QDir &base) {
    QJsonObject o{{"x", it->pos().x()}, {"y", it->pos().y()}};
    if (auto *m = qgraphicsitem_cast<MapItem *>(it)) {
        o["type"] = "map";
        o["path"] = base.relativeFilePath(m->src->path);
        if (m->cal.valid())
            o["cal"] = QJsonObject{{"cell", m->cal.cellPx}, {"ox", m->cal.origin.x()}, {"oy", m->cal.origin.y()},
                                   {"perCell", m->cal.unitsPerCell}, {"unit", m->cal.unit}, {"grid", m->cal.showGrid}};
    } else if (auto *s = qgraphicsitem_cast<StrokeItem *>(it)) {
        o["type"] = "stroke";
        o["color"] = s->pen().color().name(QColor::HexArgb);
        o["width"] = s->pen().widthF();
        o["pts"] = pointsToJson(s->pts);
    } else if (auto *t = qgraphicsitem_cast<TextItem *>(it)) {
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

QGraphicsItem *itemFromJson(const QJsonObject &o, const QDir &base) {
    const QString type = o["type"].toString();
    QGraphicsItem *it = nullptr;
    if (type == "map") {
        auto *m = new MapItem(base.absoluteFilePath(o["path"].toString()));
        const QJsonObject c = o["cal"].toObject();
        if (!c.isEmpty()) {
            m->cal.cellPx = c["cell"].toDouble();
            m->cal.origin = QPointF(c["ox"].toDouble(), c["oy"].toDouble());
            m->cal.unitsPerCell = c["perCell"].toDouble(1.5);
            m->cal.unit = c["unit"].toString("m");
            m->cal.showGrid = c["grid"].toBool();
        }
        it = m;
    } else if (type == "stroke") {
        auto *s = new StrokeItem(QColor(o["color"].toString()), o["width"].toDouble(4));
        s->setPoints(pointsFromJson(o["pts"].toArray()));
        it = s;
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

QJsonObject layersToJson(const QList<LayerItem *> &layers, const QDir &base) {
    QJsonArray arr;
    for (LayerItem *l : layers) {
        QJsonArray items;
        for (QGraphicsItem *c : l->childItems())   // children come back in stacking order
            if (QJsonObject o = itemToJson(c, base); !o.isEmpty()) items << o;
        arr << QJsonObject{{"name", l->name}, {"visible", l->isVisible()}, {"locked", l->locked}, {"items", items}};
    }
    return QJsonObject{{"format", "vaxmaps"}, {"version", 1}, {"layers", arr}};
}

QList<LayerItem *> layersFromJson(const QJsonObject &o, QGraphicsScene *scene, const QDir &base) {
    QList<LayerItem *> layers;
    for (const QJsonValue &lv : o["layers"].toArray()) {
        const QJsonObject lo = lv.toObject();
        auto *l = new LayerItem(lo["name"].toString());
        scene->addItem(l);
        l->setZValue(layers.size());
        for (const QJsonValue &iv : lo["items"].toArray())
            if (QGraphicsItem *it = itemFromJson(iv.toObject(), base)) it->setParentItem(l);
        l->setVisible(lo["visible"].toBool(true));
        l->setLocked(lo["locked"].toBool());
        layers << l;
    }
    return layers;
}
