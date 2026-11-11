#include "canvas.h"
#include "items.h"
#include "mapitem.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QLocale>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <cmath>

bool snapEnabled = false;

namespace {

QString fmt(double v) {
    QString s = QLocale(QLocale::Portuguese, QLocale::Brazil).toString(v, 'f', 2);
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith(',')) s.chop(1);
    return s;
}

const char *toolHint(Canvas::Tool t) {
    switch (t) {
    case Canvas::Select: return "Selecionar: clique ou arraste para selecionar, arraste para mover. Del exclui. Trave a camada dos mapas para não movê-los por engano.";
    case Canvas::Pan: return "Mover vista: arraste. Também funciona com o botão do meio ou Espaço + arrastar.";
    case Canvas::Pen: return "Caneta: arraste para desenhar.";
    case Canvas::Eraser: return "Borracha: arraste sobre os traços.";
    case Canvas::Text: return "Texto: clique para escrever. Duplo clique edita um texto existente.";
    case Canvas::Note: return "Nota: clique para criar uma nota.";
    case Canvas::Measure: return "Régua: arraste para medir. Usa a calibração do mapa onde a medição começa.";
    case Canvas::Calibrate: return "Calibrar: clique numa interseção da grade do mapa.";
    case Canvas::ZoneRect: return "Zona retangular: arraste. Shift mantém quadrado.";
    case Canvas::ZoneEllipse: return "Zona circular: arraste. Shift mantém círculo.";
    case Canvas::ZonePoly: return "Zona por vértices: clique para cada vértice. Duplo clique, Enter ou clique no primeiro vértice fecha.";
    }
    return "";
}

} // namespace

MapItem *mapAt(QGraphicsScene *s, QPointF scenePos, bool calibratedOnly) {
    for (QGraphicsItem *it : s->items(scenePos))
        if (auto *m = qgraphicsitem_cast<MapItem *>(it); m && m->isVisible() && (!calibratedOnly || m->cal.valid()))
            return m;
    return nullptr;
}

QPointF snapPoint(QGraphicsScene *s, QPointF scenePos) {
    MapItem *m = mapAt(s, scenePos, true);
    if (!m) return scenePos;
    const QPointF l = m->mapFromScene(scenePos), o = m->cal.origin;
    const double c = m->cal.cellPx;
    return m->mapToScene(o + QPointF(std::round((l.x() - o.x()) / c) * c, std::round((l.y() - o.y()) / c) * c));
}

QString measureText(QGraphicsScene *s, QPointF a, QPointF b) {
    MapItem *m = mapAt(s, a, true);
    if (!m) return QString("%1 px (mapa sem calibração)").arg(fmt(QLineF(a, b).length()));
    const double cells = QLineF(m->mapFromScene(a), m->mapFromScene(b)).length() / m->cal.cellPx;
    return QString("%1 %2 · %3 %4").arg(fmt(cells * m->cal.unitsPerCell), m->cal.unit, fmt(cells),
                                        fmt(cells) == "1" ? "quadrado" : "quadrados");
}

Canvas::Canvas(QGraphicsScene *scene, QWidget *parent) : QGraphicsView(scene, parent) {
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    // Cosmetic pens and screen-sized labels have no fixed scene bounds; full repaints avoid leftover pixels.
    setViewportUpdateMode(FullViewportUpdate);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(AnchorUnderMouse);
    setResizeAnchor(AnchorViewCenter);
    setDragMode(RubberBandDrag);
    setBackgroundBrush(QColor(36, 37, 41));
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);

    measureLine = scene->addLine(QLineF(), QPen(QColor(255, 210, 60), 2, Qt::SolidLine, Qt::RoundCap));
    measureLine->setPen([] { QPen p(QColor(255, 210, 60), 2); p.setCosmetic(true); return p; }());
    measureLine->setZValue(1e6);
    measureLine->hide();
    measureLabel = new LabelItem;
    scene->addItem(measureLabel);
    measureLabel->hide();
    eraserRing = scene->addEllipse(QRectF(), QPen(QColor(255, 255, 255, 180), 0));
    eraserRing->setZValue(1e6);
    eraserRing->hide();
    calibMarker = scene->addEllipse(-5, -5, 10, 10, QPen(QColor(255, 210, 60), 2), Qt::NoBrush);
    calibMarker->setFlag(QGraphicsItem::ItemIgnoresTransformations);
    calibMarker->setZValue(1e6);
    calibMarker->hide();
}

void Canvas::setTool(Tool t) {
    cancelDrafts();
    currentTool = t;
    setDragMode(t == Select ? RubberBandDrag : NoDrag);
    eraserRing->setVisible(false);
    if (t != Measure) { measureLine->hide(); measureLabel->hide(); }
    updateCursor();
    emit toolChanged(t);
    emit status(toolHint(t));
}

void Canvas::updateCursor() {
    if (panning) viewport()->setCursor(Qt::ClosedHandCursor);
    else if (currentTool == Pan || spaceDown) viewport()->setCursor(Qt::OpenHandCursor);
    else if (currentTool == Select) viewport()->unsetCursor();
    else viewport()->setCursor(Qt::CrossCursor);
}

void Canvas::cancelDrafts() {
    delete draft;
    draft = nullptr;
    stroke = nullptr;
    erasing = measuring = false;
    calibPending = false;
    calibMarker->hide();
}

bool Canvas::layerUsable() {
    if (activeLayer && activeLayer->isVisible() && !activeLayer->locked) return true;
    emit status("A camada ativa está oculta ou travada. Escolha outra no painel Camadas.");
    return false;
}

QPointF Canvas::snapped(QPointF p) const { return snapEnabled ? snapPoint(scene(), p) : p; }

void Canvas::zoomBy(double f) {
    const double z = zoom() * f;
    if (z < 0.005 || z > 64) return;
    scale(f, f);
    emit zoomChanged(zoom());
}

void Canvas::wheelEvent(QWheelEvent *e) {
    setTransformationAnchor(AnchorUnderMouse);
    zoomBy(std::pow(1.0015, e->angleDelta().y()));
}

void Canvas::mousePressEvent(QMouseEvent *e) {
    const QPoint vp = e->position().toPoint();
    const QPointF sp = mapToScene(vp);
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (currentTool == Pan || spaceDown))) {
        panning = true;
        lastPan = vp;
        updateCursor();
        return;
    }
    if (e->button() != Qt::LeftButton || currentTool == Select) {
        QGraphicsView::mousePressEvent(e);
        if (currentTool == Select)
            for (QGraphicsItem *i : scene()->selectedItems()) pressPositions[i] = i->pos();
        return;
    }
    switch (currentTool) {
    case Pen:
        if (!layerUsable()) return;
        stroke = new StrokeItem(color, penWidth / zoom());
        stroke->setParentItem(activeLayer);
        stroke->addPoint(sp);
        return;
    case Eraser:
        erasing = true;
        erasedAny = false;
        eraseAt(sp);
        return;
    case Text:
    case Note: {
        if (qgraphicsitem_cast<TextItem *>(itemAt(vp))) { QGraphicsView::mousePressEvent(e); return; }
        if (!layerUsable()) return;
        auto *t = new TextItem(currentTool == Note, currentTool == Note ? QColor(246, 229, 141) : color);
        t->setParentItem(activeLayer);
        t->setScale(1 / zoom());   // starts at a readable screen size
        t->setPos(sp);
        setTool(Select);
        scene()->clearSelection();
        t->setSelected(true);
        t->startEditing();
        sceneEdited();
        return;
    }
    case Measure:
        measureFrom = snapped(sp);
        measuring = true;
        measureLine->show();
        measureLabel->show();
        updateMeasure(measureFrom);
        return;
    case Calibrate:
        calibrateClick(sp);
        return;
    case ZoneRect:
    case ZoneEllipse:
        if (!layerUsable()) return;
        dragStart = snapped(sp);
        draft = new ZoneItem(currentTool == ZoneRect ? ZoneItem::Rect : ZoneItem::Ellipse, {dragStart, dragStart}, color);
        draft->setParentItem(activeLayer);
        return;
    case ZonePoly: {
        if (!layerUsable()) return;
        const QPointF p = snapped(sp);
        if (!draft) {
            draft = new ZoneItem(ZoneItem::Poly, {p, p}, color);
            draft->setParentItem(activeLayer);
            return;
        }
        QPolygonF pts = draft->pts;
        const double toFirst = QLineF(mapFromScene(pts.first()), vp).length();
        if (pts.size() >= 4 && toFirst < 10) { finishPoly(); return; }
        pts.last() = p;
        pts << p;
        draft->setPoints(pts);
        return;
    }
    default:
        QGraphicsView::mousePressEvent(e);
    }
}

void Canvas::mouseMoveEvent(QMouseEvent *e) {
    const QPoint vp = e->position().toPoint();
    const QPointF sp = mapToScene(vp);
    if (panning) {
        const QPoint d = vp - lastPan;
        lastPan = vp;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        return;
    }
    if (currentTool == Eraser) {
        const double r = 12 / zoom();
        eraserRing->setRect(QRectF(sp - QPointF(r, r), QSizeF(2 * r, 2 * r)));
        eraserRing->show();
    }
    if (stroke) {
        const QPointF l = stroke->mapFromScene(sp);
        if (QLineF(stroke->pts.last(), l).length() * zoom() >= 1.5) stroke->addPoint(l);
        return;
    }
    if (erasing) { eraseAt(sp); return; }
    if (measuring) { updateMeasure(snapped(sp)); return; }
    if (draft && draft->shapeKind != ZoneItem::Poly && (e->buttons() & Qt::LeftButton)) {
        QPointF p = snapped(sp);
        if (e->modifiers() & Qt::ShiftModifier) {
            const QPointF d = p - dragStart;
            const double side = qMax(std::abs(d.x()), std::abs(d.y()));
            p = dragStart + QPointF(d.x() < 0 ? -side : side, d.y() < 0 ? -side : side);
        }
        draft->setPoints({dragStart, p});
        return;
    }
    if (draft && draft->shapeKind == ZoneItem::Poly) {
        QPolygonF pts = draft->pts;
        pts.last() = snapped(sp);
        draft->setPoints(pts);
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
}

void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (panning) { panning = false; updateCursor(); return; }
    if (stroke) { stroke = nullptr; sceneEdited(); return; }
    if (erasing) { erasing = false; if (erasedAny) sceneEdited(); return; }
    if (measuring) { measuring = false; return; }
    if (draft && draft->shapeKind != ZoneItem::Poly) {
        ZoneItem *z = draft;
        draft = nullptr;
        if (z->boundingRect().width() * zoom() < 4 || z->boundingRect().height() * zoom() < 4) { delete z; return; }
        setTool(Select);
        scene()->clearSelection();
        z->setSelected(true);
        sceneEdited();
        return;
    }
    QGraphicsView::mouseReleaseEvent(e);
    if (currentTool == Select) {
        for (auto it = pressPositions.cbegin(); it != pressPositions.cend(); ++it)
            if (scene()->items().contains(it.key()) && it.key()->pos() != it.value()) { sceneEdited(); break; }
        pressPositions.clear();
    }
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    if (draft && draft->shapeKind == ZoneItem::Poly) { finishPoly(); return; }
    QGraphicsView::mouseDoubleClickEvent(e);
}

void Canvas::finishPoly() {
    ZoneItem *z = draft;
    draft = nullptr;
    QPolygonF pts = z->pts;
    pts.removeLast();   // the vertex following the cursor
    if (pts.size() < 3) { delete z; return; }
    z->setPoints(pts);
    setTool(Select);
    scene()->clearSelection();
    z->setSelected(true);
    sceneEdited();
}

void Canvas::eraseAt(QPointF sp) {
    const double r = 12 / zoom();
    QPainterPath area;
    area.addEllipse(sp, r, r);
    for (QGraphicsItem *it : scene()->items(area)) {
        auto *s = qgraphicsitem_cast<StrokeItem *>(it);
        if (!s || !s->isVisible()) continue;
        if (auto *layer = static_cast<LayerItem *>(s->parentItem()); layer && layer->locked) continue;
        const auto runs = eraseRuns(s->pts, s->mapFromScene(sp), r + s->pen().widthF() / 2);
        if (runs.size() == 1 && runs[0] == s->pts) continue;
        erasedAny = true;
        if (runs.isEmpty()) { delete s; continue; }
        s->setPoints(runs[0]);
        for (int i = 1; i < runs.size(); ++i) {
            auto *n = new StrokeItem(s->pen().color(), s->pen().widthF());
            n->setParentItem(s->parentItem());
            n->setPos(s->pos());
            n->setPoints(runs[i]);
        }
    }
}

void Canvas::updateMeasure(QPointF to) {
    measureLine->setLine(QLineF(measureFrom, to));
    measureLabel->setText(measureText(scene(), measureFrom, to));
    measureLabel->setPos(to);
}

void Canvas::calibrateClick(QPointF sp) {
    MapItem *m = mapAt(scene(), sp, false);
    if (!m) { emit status("Clique sobre um mapa."); return; }
    if (!calibPending || m != calibMap) {
        calibMap = m;
        calibA = m->mapFromScene(sp);
        calibPending = true;
        calibMarker->setPos(sp);
        calibMarker->show();
        emit status("Agora clique em outra interseção na mesma linha ou coluna, alguns quadrados adiante. Quanto mais longe, mais precisa.");
        return;
    }
    const QPointF b = m->mapFromScene(sp);
    calibPending = false;
    calibMarker->hide();

    QDialog dlg(this);
    dlg.setWindowTitle("Calibrar mapa");
    auto *form = new QFormLayout(&dlg);
    auto *cells = new QDoubleSpinBox;
    cells->setRange(0.5, 10000);
    cells->setDecimals(1);
    cells->setValue(m->cal.valid() ? std::round(qMax(std::abs(b.x() - calibA.x()), std::abs(b.y() - calibA.y())) / m->cal.cellPx) : 10);
    auto *per = new QDoubleSpinBox;
    per->setRange(0.001, 1e6);
    per->setDecimals(3);
    per->setValue(m->cal.unitsPerCell);
    auto *unit = new QLineEdit(m->cal.unit);
    form->addRow("Quadrados entre os pontos:", cells);
    form->addRow("Distância de 1 quadrado:", per);
    form->addRow("Unidade:", unit);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const Calibration c = Calibration::fromPoints(calibA, b, cells->value(), per->value(), unit->text().trimmed());
    if (c.cellPx < 1) { emit status("Os pontos estão próximos demais para calibrar."); return; }
    m->cal = c;
    m->update();
    sceneEdited();
    emit status(QString("Calibrado: 1 quadrado = %1 px = %2 %3. Setas ajustam a posição da grade (Shift: 0,1 px).")
                    .arg(fmt(c.cellPx), fmt(c.unitsPerCell), c.unit));
}

void Canvas::deleteSelected() {
    bool any = false;
    for (QGraphicsItem *i : scene()->selectedItems())
        if (i->parentItem() && i->parentItem()->type() == LayerItem::Type) { delete i; any = true; }
    if (any) sceneEdited();
}

void Canvas::keyPressEvent(QKeyEvent *e) {
    if (auto *t = qgraphicsitem_cast<TextItem *>(scene()->focusItem()); t && t->editing()) {
        if (e->key() == Qt::Key_Escape) t->clearFocus();
        else QGraphicsView::keyPressEvent(e);
        return;
    }
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) { QGraphicsView::keyPressEvent(e); return; }
    // Tool letters live here instead of QAction shortcuts so typing in a text item never switches tools.
    static const QHash<int, Tool> keys{{Qt::Key_V, Select}, {Qt::Key_H, Pan}, {Qt::Key_P, Pen}, {Qt::Key_E, Eraser},
                                       {Qt::Key_T, Text}, {Qt::Key_N, Note}, {Qt::Key_M, Measure}, {Qt::Key_K, Calibrate},
                                       {Qt::Key_R, ZoneRect}, {Qt::Key_O, ZoneEllipse}, {Qt::Key_G, ZonePoly}};
    if (keys.contains(e->key()) && !(e->modifiers() & Qt::ShiftModifier)) { setTool(keys[e->key()]); return; }
    switch (e->key()) {
    case Qt::Key_S:
        snapEnabled = !snapEnabled;
        emit snapChanged(snapEnabled);
        emit status(snapEnabled ? "Snap na grade ligado: pontos de zonas e da régua grudam nas interseções do mapa calibrado." : "Snap na grade desligado.");
        return;
    case Qt::Key_Space:
        if (!e->isAutoRepeat()) { spaceDown = true; updateCursor(); }
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        deleteSelected();
        return;
    case Qt::Key_Escape:
        cancelDrafts();
        measureLine->hide();
        measureLabel->hide();
        scene()->clearSelection();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (draft && draft->shapeKind == ZoneItem::Poly) finishPoly();
        return;
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
        if (currentTool == Calibrate && calibMap && scene()->items().contains(calibMap) && calibMap->cal.valid()) {
            const double step = (e->modifiers() & Qt::ShiftModifier) ? 0.1 : 1;
            const int k = e->key();
            calibMap->cal.origin += QPointF(k == Qt::Key_Left ? -step : k == Qt::Key_Right ? step : 0,
                                            k == Qt::Key_Up ? -step : k == Qt::Key_Down ? step : 0);
            calibMap->update();
            sceneEdited();
            return;
        }
        break;
    }
    QGraphicsView::keyPressEvent(e);
}

void Canvas::keyReleaseEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) { spaceDown = false; updateCursor(); return; }
    QGraphicsView::keyReleaseEvent(e);
}

void Canvas::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void Canvas::dragMoveEvent(QDragMoveEvent *e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void Canvas::dropEvent(QDropEvent *e) {
    QStringList files;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile()) files << u.toLocalFile();
    if (files.isEmpty()) return;
    e->acceptProposedAction();
    emit filesDropped(files, mapToScene(e->position().toPoint()));
}
