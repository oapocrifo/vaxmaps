#pragma once
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>

class LayerItem;
class QGraphicsItem;
class QGraphicsScene;

// .vaxmap files are JSON. Map images are linked by path (relative to the project file when possible), not embedded.
QJsonObject layersToJson(const QList<LayerItem *> &layers, const QDir &base);
// Adds the layers stored in `o` to the scene, bottom first.
QList<LayerItem *> layersFromJson(const QJsonObject &o, QGraphicsScene *scene, const QDir &base);
// Single items, for the clipboard. The new items have no parent yet.
QJsonArray itemsToJson(const QList<QGraphicsItem *> &items, const QDir &base);
QList<QGraphicsItem *> itemsFromJson(const QJsonArray &a, const QDir &base);
// Undo steps: the project as pieces of JSON text. The points of a stroke are a piece of their own, made again only when
// they change and otherwise shared with the steps before, so a step costs the items that changed, not the whole project.
QList<QByteArray> layersToParts(const QList<LayerItem *> &layers);
QJsonObject layersFromParts(const QList<QByteArray> &parts);
// After layers were made from `parts`: their strokes keep those pieces, so the next step does not build them again.
void reuseParts(const QList<LayerItem *> &layers, const QList<QByteArray> &parts);
