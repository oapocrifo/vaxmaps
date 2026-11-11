#pragma once
#include <QDir>
#include <QJsonObject>
#include <QList>

class LayerItem;
class QGraphicsScene;

// .vaxmap files are JSON. Map images are linked by path (relative to the project file when possible), not embedded.
QJsonObject layersToJson(const QList<LayerItem *> &layers, const QDir &base);
// Adds the layers stored in `o` to the scene, bottom first.
QList<LayerItem *> layersFromJson(const QJsonObject &o, QGraphicsScene *scene, const QDir &base);
