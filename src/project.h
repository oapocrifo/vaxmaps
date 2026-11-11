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
