#include "mainwindow.h"
#include "canvas.h"
#include "items.h"
#include "mapitem.h"
#include "project.h"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

void setSwatch(QPushButton *b, const QColor &c) {
    QPixmap px(16, 16);
    px.fill(c);
    b->setIcon(px);
}

LayerItem *layerOf(QTreeWidgetItem *ti) {
    return ti ? reinterpret_cast<LayerItem *>(ti->data(2, Qt::UserRole).value<quintptr>()) : nullptr;
}

} // namespace

MainWindow::MainWindow() {
    scene = new QGraphicsScene(this);
    // "Infinite": ±5 million px stays inside the scrollbars' int range even at 64x zoom.
    scene->setSceneRect(-5e6, -5e6, 1e7, 1e7);
    canvas = new Canvas(scene, this);
    setCentralWidget(canvas);
    onSceneEdited = [this] { setDirty(true); };

    // ---- tools ----
    auto *tb = addToolBar("Ferramentas");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto *group = new QActionGroup(this);
    struct ToolDef { Canvas::Tool tool; const char *name, *key; };
    static const ToolDef tools[] = {   // order matches Canvas::Tool
        {Canvas::Select, "Selecionar", "V"}, {Canvas::Pan, "Mover vista", "H"}, {Canvas::Pen, "Caneta", "P"},
        {Canvas::Eraser, "Borracha", "E"}, {Canvas::Text, "Texto", "T"}, {Canvas::Note, "Nota", "N"},
        {Canvas::Measure, "Régua", "M"}, {Canvas::Calibrate, "Calibrar", "K"}, {Canvas::ZoneRect, "Zona retângulo", "R"},
        {Canvas::ZoneEllipse, "Zona círculo", "O"}, {Canvas::ZonePoly, "Zona vértices", "G"}};
    QList<QAction *> toolActions;
    for (const ToolDef &t : tools) {
        QAction *a = tb->addAction(t.name);
        a->setToolTip(QString("%1 (%2)").arg(t.name, t.key));
        a->setCheckable(true);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { canvas->setTool(t.tool); canvas->setFocus(); });
        toolActions << a;
    }
    connect(canvas, &Canvas::toolChanged, this, [toolActions](int t) { toolActions[t]->setChecked(true); });
    tb->addSeparator();
    colorButton = new QPushButton;
    colorButton->setToolTip("Cor de novos traços, textos e zonas");
    setSwatch(colorButton, canvas->color);
    connect(colorButton, &QPushButton::clicked, this, &MainWindow::pickColor);
    tb->addWidget(colorButton);
    auto *width = new QSpinBox;
    width->setRange(1, 60);
    width->setValue(int(canvas->penWidth));
    width->setSuffix(" px");
    width->setToolTip("Espessura da caneta");
    connect(width, &QSpinBox::valueChanged, this, [this](int v) { canvas->penWidth = v; });
    tb->addWidget(width);
    QAction *snap = tb->addAction("Snap na grade");
    snap->setToolTip("Snap na grade (S): zonas e régua grudam nas interseções do mapa calibrado");
    snap->setCheckable(true);
    connect(snap, &QAction::toggled, this, [](bool on) { snapEnabled = on; });
    connect(canvas, &Canvas::snapChanged, snap, &QAction::setChecked);

    // ---- menus ----
    auto add = [this](QMenu *m, const QString &text, const QKeySequence &ks, auto fn) {
        QAction *a = m->addAction(text);
        a->setShortcut(ks);
        connect(a, &QAction::triggered, this, fn);
        return a;
    };
    QMenu *fm = menuBar()->addMenu("&Arquivo");
    add(fm, "Novo", QKeySequence::New, [this] { if (maybeSave()) newProject(); });
    add(fm, "Abrir…", QKeySequence::Open, [this] { open(); });
    add(fm, "Salvar", QKeySequence::Save, [this] { save(); });
    add(fm, "Salvar como…", QKeySequence("Ctrl+Shift+S"), [this] { saveAs(); });
    fm->addSeparator();
    add(fm, "Importar mapas…", QKeySequence("Ctrl+I"), [this] { importDialog(); });
    fm->addSeparator();
    add(fm, "Sair", QKeySequence("Ctrl+Q"), [this] { close(); });
    QMenu *em = menuBar()->addMenu("&Editar");
    add(em, "Excluir seleção\tDel", QKeySequence(), [this] { canvas->deleteSelected(); });
    add(em, "Mover seleção para a camada ativa", QKeySequence("Ctrl+M"), [this] { moveSelectionToActiveLayer(); });
    QMenu *vm = menuBar()->addMenu("E&xibir");
    add(vm, "Ajustar à tela", QKeySequence("Ctrl+0"), [this] { fitAll(); });
    add(vm, "Zoom 100%", QKeySequence("Ctrl+1"), [this] {
        const QPointF c = canvas->mapToScene(canvas->viewport()->rect().center());
        canvas->resetTransform();
        canvas->centerOn(c);
        emit canvas->zoomChanged(1);
    });
    add(vm, "Aumentar zoom", QKeySequence::ZoomIn, [this] { canvas->setTransformationAnchor(QGraphicsView::AnchorViewCenter); canvas->zoomBy(1.25); });
    add(vm, "Diminuir zoom", QKeySequence::ZoomOut, [this] { canvas->setTransformationAnchor(QGraphicsView::AnchorViewCenter); canvas->zoomBy(0.8); });

    // ---- layers ----
    auto *layerDock = new QDockWidget("Camadas", this);
    layerDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    auto *lw = new QWidget;
    auto *lv = new QVBoxLayout(lw);
    layerList = new QTreeWidget;
    layerList->setColumnCount(3);
    layerList->setHeaderLabels({"Ver", "Travar", "Camada (ativa em destaque)"});
    layerList->setRootIsDecorated(false);
    layerList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layerList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    layerList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layerList->header()->setStretchLastSection(true);
    lv->addWidget(layerList);
    auto *row = new QHBoxLayout;
    auto button = [&](const QString &text, const QString &tip, auto fn) {
        auto *b = new QPushButton(text);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        row->addWidget(b);
    };
    button("+", "Nova camada", [this] {
        canvas->activeLayer = addLayer(QString("Camada %1").arg(layers.size() + 1));
        refreshLayers();
        setDirty(true);
    });
    button("−", "Excluir camada ativa e seu conteúdo", [this] {
        LayerItem *l = canvas->activeLayer;
        if (!l || layers.size() == 1) return;
        if (!l->childItems().isEmpty()
            && QMessageBox::question(this, "Vax Maps", QString("Excluir a camada \"%1\" e %2 itens?").arg(l->name).arg(l->childItems().size()))
                   != QMessageBox::Yes)
            return;
        canvas->cancelDrafts();
        layers.removeOne(l);
        delete l;
        canvas->activeLayer = layers.last();
        restackLayers();
        refreshLayers();
        setDirty(true);
    });
    button("↑", "Subir camada", [this] {
        const int i = layers.indexOf(canvas->activeLayer);
        if (i < 0 || i == layers.size() - 1) return;
        layers.swapItemsAt(i, i + 1);
        restackLayers();
        refreshLayers();
        setDirty(true);
    });
    button("↓", "Descer camada", [this] {
        const int i = layers.indexOf(canvas->activeLayer);
        if (i <= 0) return;
        layers.swapItemsAt(i, i - 1);
        restackLayers();
        refreshLayers();
        setDirty(true);
    });
    lv->addLayout(row);
    layerDock->setWidget(lw);
    addDockWidget(Qt::RightDockWidgetArea, layerDock);
    connect(layerList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *ti, int col) {
        if (col == 2) layerList->editItem(ti, 2);
    });
    connect(layerList, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *ti, int col) {
        if (refreshing) return;
        LayerItem *l = layerOf(ti);
        if (col == 0) l->setVisible(ti->checkState(0) == Qt::Checked);
        else if (col == 1) l->setLocked(ti->checkState(1) == Qt::Checked);
        else if (col == 2) l->name = ti->text(2);
        setDirty(true);
    });
    connect(layerList, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *cur) {
        if (cur) canvas->activeLayer = layerOf(cur);
    });

    // ---- properties ----
    auto *propDock = new QDockWidget("Propriedades", this);
    propDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    auto *pw = new QWidget;
    props = new QFormLayout(pw);
    propKind = new QLabel;
    props->addRow(propKind);
    nameEdit = new QLineEdit;
    props->addRow("Nome", nameEdit);
    descEdit = new QPlainTextEdit;
    descEdit->setMaximumHeight(140);
    props->addRow("Descrição", descEdit);
    propColor = new QPushButton("Alterar…");
    props->addRow("Cor", propColor);
    cellSpin = new QDoubleSpinBox;
    cellSpin->setRange(0, 100000);
    cellSpin->setDecimals(3);
    cellSpin->setSuffix(" px");
    props->addRow("1 quadrado", cellSpin);
    perCellSpin = new QDoubleSpinBox;
    perCellSpin->setRange(0.001, 1e6);
    perCellSpin->setDecimals(3);
    props->addRow("Vale", perCellSpin);
    unitEdit = new QLineEdit;
    props->addRow("Unidade", unitEdit);
    gridCheck = new QCheckBox("Mostrar grade calibrada");
    props->addRow(gridCheck);
    calHint = new QLabel("Para medir a grade use a ferramenta Calibrar (K).");
    calHint->setWordWrap(true);
    props->addRow(calHint);
    propDock->setWidget(pw);
    addDockWidget(Qt::RightDockWidgetArea, propDock);

    auto selected = [this]() -> QGraphicsItem * {
        return !refreshing && propItem && scene->selectedItems().contains(propItem) ? propItem : nullptr;
    };
    connect(nameEdit, &QLineEdit::textEdited, this, [=](const QString &s) {
        if (auto *z = qgraphicsitem_cast<ZoneItem *>(selected())) { z->name = s; z->update(); setDirty(true); }
    });
    connect(descEdit, &QPlainTextEdit::textChanged, this, [=] {
        if (auto *z = qgraphicsitem_cast<ZoneItem *>(selected())) { z->setDesc(descEdit->toPlainText()); setDirty(true); }
    });
    connect(propColor, &QPushButton::clicked, this, [=] {
        QGraphicsItem *it = selected();
        if (!it) return;
        auto *z = qgraphicsitem_cast<ZoneItem *>(it);
        auto *t = qgraphicsitem_cast<TextItem *>(it);
        auto *s = qgraphicsitem_cast<StrokeItem *>(it);
        const QColor c = QColorDialog::getColor(z ? z->color : t ? t->color : s ? s->pen().color() : Qt::white, this, "Cor");
        if (!c.isValid()) return;
        if (z) { z->color = c; z->update(); }
        if (t) t->setColor(c);
        if (s) { QPen p = s->pen(); p.setColor(c); s->setPen(p); }
        setDirty(true);
    });
    auto editMap = [=](auto fn) {
        if (auto *m = qgraphicsitem_cast<MapItem *>(selected())) { fn(m->cal); m->update(); setDirty(true); }
    };
    connect(cellSpin, &QDoubleSpinBox::valueChanged, this, [=](double v) { editMap([v](Calibration &c) { c.cellPx = v; }); });
    connect(perCellSpin, &QDoubleSpinBox::valueChanged, this, [=](double v) { editMap([v](Calibration &c) { c.unitsPerCell = v; }); });
    connect(unitEdit, &QLineEdit::textEdited, this, [=](const QString &s) { editMap([s](Calibration &c) { c.unit = s; }); });
    connect(gridCheck, &QCheckBox::toggled, this, [=](bool on) { editMap([on](Calibration &c) { c.showGrid = on; }); });
    connect(scene, &QGraphicsScene::selectionChanged, this, &MainWindow::onSelection);

    // ---- status ----
    statusText = new QLabel;
    zoomText = new QLabel("Zoom 100%");
    statusBar()->addWidget(statusText, 1);
    statusBar()->addPermanentWidget(zoomText);
    connect(canvas, &Canvas::status, statusText, &QLabel::setText);
    connect(canvas, &Canvas::zoomChanged, this, [this](double z) { zoomText->setText(QString("Zoom %1%").arg(qRound(z * 100))); });
    connect(canvas, &Canvas::filesDropped, this, [this](const QStringList &files, QPointF pos) {
        for (const QString &f : files)
            if (f.endsWith(".vaxmap", Qt::CaseInsensitive)) { if (maybeSave()) openProject(f); return; }
        for (const QString &f : files)
            if (MapItem *m = importMap(f, pos)) pos.rx() += m->boundingRect().width() + 100;
    });

    newProject();
    onSelection();
    canvas->setTool(Canvas::Select);
    resize(1500, 950);
}

MainWindow::~MainWindow() { onSceneEdited = nullptr; }

void MainWindow::setDirty(bool d) {
    dirty = d;
    setWindowTitle(QString("Vax Maps - %1%2").arg(file.isEmpty() ? "Sem título" : QFileInfo(file).fileName(), d ? " *" : ""));
}

LayerItem *MainWindow::addLayer(const QString &name) {
    auto *l = new LayerItem(name);
    scene->addItem(l);
    layers << l;
    restackLayers();
    return l;
}

void MainWindow::restackLayers() {
    for (int i = 0; i < layers.size(); ++i) layers[i]->setZValue(i);
}

void MainWindow::refreshLayers() {
    refreshing = true;
    LayerItem *active = canvas->activeLayer;
    layerList->clear();
    QTreeWidgetItem *current = nullptr;
    for (int i = layers.size() - 1; i >= 0; --i) {   // top layer first
        LayerItem *l = layers[i];
        auto *ti = new QTreeWidgetItem(layerList);
        ti->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
        ti->setCheckState(0, l->isVisible() ? Qt::Checked : Qt::Unchecked);
        ti->setCheckState(1, l->locked ? Qt::Checked : Qt::Unchecked);
        ti->setText(2, l->name);
        ti->setData(2, Qt::UserRole, QVariant::fromValue(quintptr(l)));
        if (l == active || !current) current = ti;
    }
    layerList->setCurrentItem(current);
    refreshing = false;
    canvas->activeLayer = layerOf(current);
}

void MainWindow::onSelection() {
    refreshing = true;
    const QList<QGraphicsItem *> sel = scene->selectedItems();
    propItem = sel.size() == 1 ? sel[0] : nullptr;
    auto *z = qgraphicsitem_cast<ZoneItem *>(propItem);
    auto *m = qgraphicsitem_cast<MapItem *>(propItem);
    auto *t = qgraphicsitem_cast<TextItem *>(propItem);
    auto *s = qgraphicsitem_cast<StrokeItem *>(propItem);
    propKind->setText(sel.isEmpty() ? "Nada selecionado"
                      : sel.size() > 1 ? QString("%1 itens selecionados").arg(sel.size())
                      : z ? "Zona"
                      : m ? "Mapa: " + QFileInfo(m->src->path).fileName()
                      : t ? (t->note ? "Nota" : "Texto")
                      : "Traço");
    props->setRowVisible(nameEdit, z);
    props->setRowVisible(descEdit, z);
    props->setRowVisible(propColor, z || t || s);
    for (QWidget *w : {static_cast<QWidget *>(cellSpin), static_cast<QWidget *>(perCellSpin), static_cast<QWidget *>(unitEdit),
                       static_cast<QWidget *>(gridCheck), static_cast<QWidget *>(calHint)})
        props->setRowVisible(w, m);
    if (z) {
        nameEdit->setText(z->name);
        descEdit->setPlainText(z->desc);
    }
    if (m) {
        cellSpin->setValue(m->cal.cellPx);
        perCellSpin->setValue(m->cal.unitsPerCell);
        unitEdit->setText(m->cal.unit);
        gridCheck->setChecked(m->cal.showGrid);
    }
    refreshing = false;
}

void MainWindow::pickColor() {
    const QColor c = QColorDialog::getColor(canvas->color, this, "Cor");
    if (!c.isValid()) return;
    canvas->color = c;
    setSwatch(colorButton, c);
}

MapItem *MainWindow::importMap(const QString &path, QPointF scenePos) {
    if (!canvas->layerUsable()) return nullptr;
    if (!QImageReader(path).canRead()) {
        statusText->setText("Formato de imagem não suportado: " + QFileInfo(path).fileName());
        return nullptr;
    }
    auto *m = new MapItem(path);
    m->setParentItem(canvas->activeLayer);
    m->setPos(scenePos - m->boundingRect().center());
    setDirty(true);
    return m;
}

void MainWindow::importDialog() {
    const QStringList files = QFileDialog::getOpenFileNames(this, "Importar mapas", QString(),
        "Imagens (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff *.gif);;Todos os arquivos (*)");
    QPointF pos = canvas->mapToScene(canvas->viewport()->rect().center());
    for (const QString &f : files)
        if (MapItem *m = importMap(f, pos)) pos.rx() += m->boundingRect().width() + 100;
}

void MainWindow::newProject() {
    canvas->setTool(Canvas::Select);   // ends drafts and a calibration in progress
    qDeleteAll(layers);
    layers.clear();
    for (const char *name : {"Mapas", "Zonas", "Desenhos", "Textos"}) addLayer(name);
    canvas->activeLayer = layers.first();
    canvas->resetTransform();
    canvas->centerOn(0, 0);
    emit canvas->zoomChanged(1);
    file.clear();
    refreshLayers();
    setDirty(false);
}

bool MainWindow::openProject(const QString &f) {
    QFile in(f);
    if (!in.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Vax Maps", "Não foi possível abrir " + f + ": " + in.errorString());
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll(), &err);
    if (doc.isNull() || doc.object()["format"].toString() != "vaxmaps") {
        QMessageBox::warning(this, "Vax Maps", "Arquivo de projeto inválido: " + f);
        return false;
    }
    canvas->setTool(Canvas::Select);   // ends drafts and a calibration in progress
    qDeleteAll(layers);
    layers = layersFromJson(doc.object(), scene, QFileInfo(f).absoluteDir());
    if (layers.isEmpty()) addLayer("Camada 1");
    canvas->activeLayer = layers.first();
    const QJsonObject v = doc.object()["view"].toObject();
    canvas->resetTransform();
    const double z = qBound(0.005, v["zoom"].toDouble(1), 64.0);
    canvas->scale(z, z);
    canvas->centerOn(v["cx"].toDouble(), v["cy"].toDouble());
    emit canvas->zoomChanged(z);
    file = QFileInfo(f).absoluteFilePath();
    refreshLayers();
    setDirty(false);
    return true;
}

bool MainWindow::saveTo(const QString &f) {
    QJsonObject o = layersToJson(layers, QFileInfo(f).absoluteDir());
    const QPointF c = canvas->mapToScene(canvas->viewport()->rect().center());
    o["view"] = QJsonObject{{"cx", c.x()}, {"cy", c.y()}, {"zoom", canvas->zoom()}};
    QSaveFile out(f);   // writes to a temp file and renames: a failed save never truncates the old project
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(o).toJson(QJsonDocument::Compact)) < 0 || !out.commit()) {
        QMessageBox::warning(this, "Vax Maps", "Falha ao salvar " + f + ": " + out.errorString());
        return false;
    }
    file = QFileInfo(f).absoluteFilePath();
    setDirty(false);
    return true;
}

bool MainWindow::save() { return file.isEmpty() ? saveAs() : saveTo(file); }

bool MainWindow::saveAs() {
    QString f = QFileDialog::getSaveFileName(this, "Salvar projeto", file.isEmpty() ? "mapa.vaxmap" : file,
                                             "Projeto Vax Maps (*.vaxmap)");
    if (f.isEmpty()) return false;
    if (!f.endsWith(".vaxmap", Qt::CaseInsensitive)) f += ".vaxmap";
    return saveTo(f);
}

void MainWindow::open() {
    if (!maybeSave()) return;
    const QString f = QFileDialog::getOpenFileName(this, "Abrir projeto", QString(), "Projeto Vax Maps (*.vaxmap)");
    if (!f.isEmpty()) openProject(f);
}

bool MainWindow::maybeSave() {
    if (!dirty) return true;
    const auto r = QMessageBox::question(this, "Vax Maps", "Salvar as alterações do projeto?",
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Save) return save();
    return r == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (maybeSave()) e->accept();
    else e->ignore();
}

void MainWindow::fitAll() {
    QRectF r;
    for (LayerItem *l : layers)
        if (l->isVisible())
            for (QGraphicsItem *c : l->childItems()) r |= c->sceneBoundingRect();
    if (r.isEmpty()) return;
    canvas->fitInView(r.adjusted(-50, -50, 50, 50), Qt::KeepAspectRatio);
    emit canvas->zoomChanged(canvas->zoom());
}

void MainWindow::moveSelectionToActiveLayer() {
    if (!canvas->layerUsable()) return;
    LayerItem *active = canvas->activeLayer;
    for (QGraphicsItem *it : scene->selectedItems()) {
        if (!it->parentItem() || it->parentItem()->type() != LayerItem::Type || it->parentItem() == active) continue;
        const QPointF sp = it->scenePos();
        it->setParentItem(active);
        it->setPos(active->mapFromScene(sp));
        active->applyLock(it);
    }
    setDirty(true);
}
