#include "mainwindow.h"
#include "canvas.h"
#include "items.h"
#include "mapitem.h"
#include "project.h"
#include "ui.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSlider>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

LayerItem *layerOf(QTreeWidgetItem *ti) {
    return ti ? reinterpret_cast<LayerItem *>(ti->data(2, Qt::UserRole).value<quintptr>()) : nullptr;
}

const char *panelStyle = R"(
Panel QLabel { color: #e8e8ec; }
Panel QLineEdit, Panel QPlainTextEdit, Panel QDoubleSpinBox {
    background: rgba(255,255,255,15); border: 1px solid rgba(255,255,255,26); border-radius: 6px;
    padding: 4px 6px; color: #e8e8ec; selection-background-color: #5c8cff; }
Panel QLineEdit:focus, Panel QPlainTextEdit:focus, Panel QDoubleSpinBox:focus { border: 1px solid #5c8cff; }
Panel QSlider::groove:horizontal { height: 4px; background: rgba(255,255,255,30); border-radius: 2px; }
Panel QSlider::sub-page:horizontal { background: #5c8cff; border-radius: 2px; }
Panel QSlider::handle:horizontal { background: white; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }
Panel QTreeWidget { background: transparent; border: none; color: #e8e8ec; outline: 0; }
Panel QTreeWidget::item { height: 28px; }
Panel QTreeWidget::item:hover { background: rgba(255,255,255,15); }
Panel QTreeWidget::item:selected { background: rgba(92,140,255,140); color: white; }
Panel QHeaderView, Panel QHeaderView::section { background: transparent; color: #92949c; border: none; padding: 2px 4px; }
Panel QTreeWidget::indicator { width: 13px; height: 13px; border-radius: 4px; border: 1px solid rgba(255,255,255,70); background: transparent; }
Panel QTreeWidget::indicator:checked { background: #5c8cff; border: 1px solid #5c8cff; }
Panel QScrollBar:vertical { background: transparent; width: 8px; }
Panel QScrollBar::handle:vertical { background: rgba(255,255,255,40); border-radius: 4px; min-height: 24px; }
Panel QScrollBar::add-line, Panel QScrollBar::sub-line { height: 0; }
)";

QLabel *heading(const QString &t) {
    auto *l = new QLabel(t);
    QFont f = l->font();
    f.setPixelSize(14);
    f.setBold(true);
    l->setFont(f);
    return l;
}

} // namespace

MainWindow::MainWindow() {
    scene = new QGraphicsScene(this);
    // "Infinite": ±5 million px stays inside the scrollbars' int range even at 64x zoom.
    scene->setSceneRect(-5e6, -5e6, 1e7, 1e7);
    canvas = new Canvas(scene, this);
    setCentralWidget(canvas);
    onSceneEdited = [this] { setDirty(true); };
    qApp->setStyleSheet(panelStyle);

    toast = new Toast(canvas, Toast::BottomCenter);
    zoomToast = new Toast(canvas, Toast::BottomRight);
    buildMenu();
    buildLayersPanel();
    props = new Panel(canvas);
    props->setFixedWidth(330);
    // while it fades out, the popup's fields no longer edit the item (it may be gone)
    connect(props, &Panel::dismissed, this, [this] { if (propsContent) propsContent->setEnabled(false); canvas->setFocus(); });
    connect(scene, &QGraphicsScene::selectionChanged, this, [this] { if (props->isShown()) props->dismiss(); });

    connect(canvas, &Canvas::status, this, &MainWindow::say);
    connect(canvas, &Canvas::zoomChanged, this, [this](double z) { zoomToast->showMessage(QString("%1%").arg(qRound(z * 100)), 900); });
    connect(canvas, &Canvas::toggleLayersRequested, this, &MainWindow::toggleLayers);
    connect(canvas, &Canvas::contextMenuRequested, this, [this](QPoint vp, QGraphicsItem *it) {
        const QPoint at = canvas->viewport()->mapTo(canvas, vp);
        if (it) openProperties(it, at);
        else openMenu(at);
    });
    connect(canvas, &Canvas::filesDropped, this, [this](const QStringList &files, QPointF pos) {
        for (const QString &f : files)
            if (f.endsWith(".vaxmap", Qt::CaseInsensitive)) { if (maybeSave()) openProject(f); return; }
        for (const QString &f : files)
            if (MapItem *m = importMap(f, pos)) pos.rx() += m->boundingRect().width() + 100;
    });
    qApp->installEventFilter(this);   // closes popups on outside clicks and Esc, keeps overlays placed

    newProject();
    resize(1500, 950);
    say("Right click opens the menu. L shows the layers, F11 toggles full screen.");
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
    onSceneEdited = nullptr;
}

QAction *MainWindow::action(const QString &text, const QKeySequence &keys, std::function<void()> fn) {
    auto *a = new QAction(text, this);
    a->setShortcut(keys);
    a->setShortcutContext(Qt::WindowShortcut);
    connect(a, &QAction::triggered, this, fn);
    addAction(a);   // shortcuts work without a menu bar
    return a;
}

MenuButton *MainWindow::menuButton(QAction *a) {
    auto *b = new MenuButton(a->text(), a->shortcut().toString(QKeySequence::NativeText));
    connect(b, &QAbstractButton::clicked, this, [this, a] { menu->dismiss(); a->trigger(); });
    return b;
}

void MainWindow::buildMenu() {
    menu = new Panel(canvas);
    menu->setFixedWidth(470);
    QVBoxLayout *v = menu->body;

    struct ToolDef { Canvas::Tool tool; const char *name, *key; };
    static const ToolDef tools[] = {   // order matches Canvas::Tool
        {Canvas::Select, "Select", "V"}, {Canvas::Pan, "Pan", "H"}, {Canvas::Pen, "Pen", "P"},
        {Canvas::Eraser, "Eraser", "E"}, {Canvas::Text, "Text", "T"}, {Canvas::Note, "Note", "N"},
        {Canvas::Measure, "Ruler", "M"}, {Canvas::Calibrate, "Calibrate", "K"}, {Canvas::ZoneRect, "Rectangle", "R"},
        {Canvas::ZoneEllipse, "Circle", "O"}, {Canvas::ZonePoly, "Vertices", "G"}, {Canvas::Link, "New link", "J"}};
    auto *group = new QButtonGroup(this);
    QGridLayout *grid = nullptr;
    for (int i = 0; i < int(std::size(tools)); ++i) {
        if (i == 0 || i == 8 || i == 11) {
            v->addWidget(ui::caption(i == 0 ? "Tools" : i == 8 ? "Zones" : "Links between maps"));
            grid = new QGridLayout;
            grid->setSpacing(3);
            v->addLayout(grid);
        }
        const ToolDef t = tools[i];
        auto *b = new MenuButton(t.name, t.key);
        b->setCheckable(true);
        group->addButton(b);
        const int k = i < 8 ? i : i < 11 ? i - 8 : 0;
        grid->addWidget(b, k / 3, k % 3);
        connect(b, &QAbstractButton::clicked, this, [this, t] { menu->dismiss(); canvas->setTool(t.tool); canvas->setFocus(); });
        toolButtons << b;
    }
    linksButton = new MenuButton("Show links", "Shift+J");
    linksButton->setCheckable(true);
    connect(linksButton, &QAbstractButton::clicked, this, [this](bool on) { menu->dismiss(); canvas->showLinks(on); });
    connect(canvas, &Canvas::linksShown, this, [this](bool on) { linksButton->setChecked(on); refreshLayers(); });
    grid->addWidget(linksButton, 0, 1, 1, 2);
    connect(canvas, &Canvas::toolChanged, this, [this](int t) { toolButtons[t]->setChecked(true); });

    v->addWidget(ui::caption("Colour and width"));
    menuSwatches = new SwatchRow;
    connect(menuSwatches, &SwatchRow::picked, this, [this](const QColor &c) { canvas->color = c; });
    v->addWidget(menuSwatches);
    auto *row = new QHBoxLayout;
    widthSlider = new QSlider(Qt::Horizontal);
    widthSlider->setRange(1, 40);
    widthSlider->setFocusPolicy(Qt::NoFocus);
    widthLabel = new QLabel;
    widthLabel->setFixedWidth(44);
    connect(widthSlider, &QSlider::valueChanged, this, [this](int w) { canvas->penWidth = w; widthLabel->setText(QString("%1 px").arg(w)); });
    row->addWidget(widthSlider);
    row->addWidget(widthLabel);
    v->addLayout(row);

    v->addWidget(ui::caption("View"));
    grid = new QGridLayout;
    grid->setSpacing(3);
    snapButton = new MenuButton("Snap to grid", "S");
    snapButton->setCheckable(true);
    connect(snapButton, &QAbstractButton::toggled, this, [](bool on) { snapEnabled = on; });
    connect(canvas, &Canvas::snapChanged, snapButton, &QAbstractButton::setChecked);
    layersButton = new MenuButton("Layers", "L");
    layersButton->setCheckable(true);
    connect(layersButton, &QAbstractButton::clicked, this, [this] { toggleLayers(); });
    fullScreenAction = action("Full screen", QKeySequence(Qt::Key_F11), [this] { toggleFullScreen(); });
    fullButton = new MenuButton("Full screen", "F11");
    fullButton->setCheckable(true);
    connect(fullButton, &QAbstractButton::clicked, this, [this] { toggleFullScreen(); });
    grid->addWidget(snapButton, 0, 0);
    grid->addWidget(layersButton, 0, 1);
    grid->addWidget(fullButton, 0, 2);
    grid->addWidget(menuButton(action("Fit all", QKeySequence("Ctrl+0"), [this] { fitAll(); })), 1, 0);
    grid->addWidget(menuButton(action("Zoom 100%", QKeySequence("Ctrl+1"), [this] {
                        const QPointF c = canvas->mapToScene(canvas->viewport()->rect().center());
                        canvas->resetTransform();
                        canvas->centerOn(c);
                        emit canvas->zoomChanged(1);
                    })), 1, 1);
    action("Zoom in", QKeySequence::ZoomIn, [this] { canvas->setTransformationAnchor(QGraphicsView::AnchorViewCenter); canvas->zoomBy(1.25); });
    action("Zoom out", QKeySequence::ZoomOut, [this] { canvas->setTransformationAnchor(QGraphicsView::AnchorViewCenter); canvas->zoomBy(0.8); });
    v->addLayout(grid);

    v->addWidget(ui::caption("Project"));
    grid = new QGridLayout;
    grid->setSpacing(3);
    grid->addWidget(menuButton(action("New", QKeySequence::New, [this] { if (maybeSave()) newProject(); })), 0, 0);
    grid->addWidget(menuButton(action("Open…", QKeySequence::Open, [this] { open(); })), 0, 1);
    grid->addWidget(menuButton(action("Save", QKeySequence::Save, [this] { save(); })), 1, 0);
    grid->addWidget(menuButton(action("Save as…", QKeySequence("Ctrl+Shift+S"), [this] { saveAs(); })), 1, 1);
    grid->addWidget(menuButton(action("Import maps…", QKeySequence("Ctrl+I"), [this] { importDialog(); })), 2, 0);
    grid->addWidget(menuButton(action("Quit", QKeySequence("Ctrl+Q"), [this] { close(); })), 2, 1);
    v->addLayout(grid);

    v->addWidget(ui::caption("Selection"));
    grid = new QGridLayout;
    grid->setSpacing(3);
    grid->addWidget(menuButton(action("Move to the active layer", QKeySequence("Ctrl+M"), [this] { moveSelectionToActiveLayer(); })), 0, 0, 1, 2);
    auto *del = new MenuButton("Delete", "Del");
    del->ink = ui::danger;
    connect(del, &QAbstractButton::clicked, this, [this] { menu->dismiss(); canvas->deleteSelected(); });
    grid->addWidget(del, 0, 2);
    v->addLayout(grid);
}

void MainWindow::openMenu(QPoint at) {
    props->dismiss();
    toolButtons[canvas->tool()]->setChecked(true);
    snapButton->setChecked(snapEnabled);
    layersButton->setChecked(layersPanel->isShown());
    fullButton->setChecked(isFullScreen());
    menuSwatches->setCurrent(canvas->color);
    widthSlider->setValue(int(canvas->penWidth));
    widthLabel->setText(QString("%1 px").arg(int(canvas->penWidth)));
    menu->popUp(at + QPoint(2, 2));
}

void MainWindow::openProperties(QGraphicsItem *it, QPoint at) {
    menu->dismiss();
    delete propsContent;
    propsContent = new QWidget;
    auto *v = new QVBoxLayout(propsContent);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    props->body->addWidget(propsContent);

    auto deleteButton = [this](const QString &text) {
        auto *b = new MenuButton(text, "Del");
        b->ink = ui::danger;
        connect(b, &QAbstractButton::clicked, this, [this] { props->dismiss(); canvas->deleteSelected(); });
        return b;
    };
    auto colorRow = [this, v](const QColor &current, std::function<void(const QColor &)> apply) {
        v->addWidget(ui::caption("Colour"));
        auto *s = new SwatchRow;
        s->setCurrent(current);
        connect(s, &SwatchRow::picked, this, [this, apply](const QColor &c) { apply(c); setDirty(true); });
        v->addWidget(s);
    };

    const QList<QGraphicsItem *> sel = scene->selectedItems();
    auto *z = qgraphicsitem_cast<ZoneItem *>(it);
    auto *m = qgraphicsitem_cast<MapItem *>(it);
    auto *t = qgraphicsitem_cast<TextItem *>(it);
    auto *s = qgraphicsitem_cast<StrokeItem *>(it);
    if (sel.size() > 1) {
        v->addWidget(heading(QString("%1 items selected").arg(sel.size())));
        auto *move = new MenuButton("Move to the active layer", "Ctrl+M");
        connect(move, &QAbstractButton::clicked, this, [this] { props->dismiss(); moveSelectionToActiveLayer(); });
        v->addWidget(move);
        v->addWidget(deleteButton("Delete selection"));
    } else if (z) {
        v->addWidget(heading(z->shapeKind == ZoneItem::Rect ? "Rectangle zone" : z->shapeKind == ZoneItem::Ellipse ? "Circle zone" : "Vertex zone"));
        v->addWidget(ui::caption("Name"));
        auto *name = new QLineEdit(z->name);
        name->setPlaceholderText("Unnamed");
        connect(name, &QLineEdit::textChanged, this, [this, z](const QString &n) { z->name = n; z->update(); setDirty(true); });
        v->addWidget(name);
        colorRow(z->color, [z](const QColor &c) { z->color = c; z->update(); });
        v->addWidget(ui::caption("Description"));
        auto *desc = new QPlainTextEdit(z->desc);
        desc->setFixedHeight(96);
        desc->setPlaceholderText("Notes about this area");
        connect(desc, &QPlainTextEdit::textChanged, this, [this, z, desc] { z->setDesc(desc->toPlainText()); setDirty(true); });
        v->addWidget(desc);
        v->addSpacing(4);
        v->addWidget(deleteButton("Delete zone"));
    } else if (m) {
        v->addWidget(heading("Map · " + QFileInfo(m->src->path).fileName()));
        const Calibration &cal = m->cal;
        v->addWidget(ui::caption("Grid"));
        auto *kind = new QLabel(!cal.valid() ? QString("Not calibrated yet.")
                                : QString("%1, cell of %2 px%3").arg(cal.grid == Calibration::Square ? "Squares" : cal.grid == Calibration::HexPointy ? "Hexes, pointy top" : "Hexes, flat top")
                                      .arg(cal.cellSize(), 0, 'f', 2)
                                      .arg(cal.field.isEmpty() ? QString() : QString(", follows local variations")));
        kind->setWordWrap(true);
        v->addWidget(kind);
        auto *form = new QGridLayout;
        auto spin = [](double lo, double hi, double val, const QString &suffix) {
            auto *sp = new QDoubleSpinBox;
            sp->setRange(lo, hi);
            sp->setDecimals(3);
            sp->setValue(val);
            sp->setSuffix(suffix);
            sp->setButtonSymbols(QAbstractSpinBox::NoButtons);
            return sp;
        };
        auto *cell = spin(0, 100000, cal.valid() ? cal.cellSize() : 0, " px");
        cell->setEnabled(cal.valid());
        auto *per = spin(0.001, 1e6, cal.unitsPerCell, "");
        auto *unit = new QLineEdit(cal.unit);
        form->addWidget(new QLabel(cal.isHex() ? "1 hex" : "1 square"), 0, 0);
        form->addWidget(cell, 0, 1, 1, 2);
        form->addWidget(new QLabel("Is worth"), 1, 0);
        form->addWidget(per, 1, 1);
        form->addWidget(unit, 1, 2);
        v->addLayout(form);
        auto edit = [this, m](auto fn) { fn(m->cal); m->update(); setDirty(true); };
        connect(cell, &QDoubleSpinBox::editingFinished, this, [edit, cell] { edit([cell](Calibration &c) { c.scaleCell(cell->value()); }); });
        connect(per, &QDoubleSpinBox::valueChanged, this, [edit](double x) { edit([x](Calibration &c) { c.unitsPerCell = x; }); });
        connect(unit, &QLineEdit::textChanged, this, [edit](const QString &u) { edit([u](Calibration &c) { c.unit = u; }); });
        auto *grid = new MenuButton("Show the calibrated grid");
        grid->setCheckable(true);
        grid->setChecked(m->cal.showGrid);
        grid->setEnabled(m->cal.valid());
        connect(grid, &QAbstractButton::toggled, this, [edit](bool on) { edit([on](Calibration &c) { c.showGrid = on; }); });
        v->addWidget(grid);
        auto *calibrate = new MenuButton("Calibrate the grid", "K");
        connect(calibrate, &QAbstractButton::clicked, this, [this] { props->dismiss(); canvas->setTool(Canvas::Calibrate); });
        v->addWidget(calibrate);
        v->addSpacing(4);
        v->addWidget(deleteButton("Remove map"));
    } else if (it->type() == LinkItem::Type) {
        auto *l = static_cast<LinkItem *>(it);
        v->addWidget(heading("Link between maps"));
        v->addWidget(ui::caption("Name"));
        auto *name = new QLineEdit(l->name);
        connect(name, &QLineEdit::textChanged, this, [this, l](const QString &n) {
            for (LinkItem *x : {l, l->partner}) if (x) { x->name = n; x->update(); }
            setDirty(true);
        });
        v->addWidget(name);
        colorRow(l->color, [l](const QColor &c) { for (LinkItem *x : {l, l->partner}) if (x) { x->color = c; x->update(); } });
        if (!l->partner) {
            auto *lonely = new QLabel("This end has no pair yet: drag one with New link (J).");
            lonely->setWordWrap(true);
            v->addWidget(lonely);
        }
        auto *go = new MenuButton("Go to the other end", "double click");
        go->setEnabled(l->partner);
        connect(go, &QAbstractButton::clicked, this, [this, l] { props->dismiss(); canvas->travel(l, l->mapToScene(l->box().center())); });
        v->addWidget(go);
        v->addSpacing(4);
        v->addWidget(deleteButton("Delete link (both ends)"));
    } else if (t) {
        v->addWidget(heading(t->note ? "Note" : "Text"));
        colorRow(t->color, [t](const QColor &c) { t->setColor(c); });
        auto *editText = new MenuButton("Edit text", "double click");
        connect(editText, &QAbstractButton::clicked, this, [this, t] { props->dismiss(); canvas->setFocus(); t->startEditing(); });
        v->addWidget(editText);
        v->addWidget(deleteButton(t->note ? "Delete note" : "Delete text"));
    } else if (s) {
        v->addWidget(heading("Stroke"));
        colorRow(s->pen().color(), [s](const QColor &c) { QPen p = s->pen(); p.setColor(c); s->setPen(p); });
        v->addWidget(deleteButton("Delete stroke"));
    }
    props->popUp(at + QPoint(2, 2));
}

void MainWindow::buildLayersPanel() {
    layersPanel = new Panel(canvas, false);
    layersPanel->setFixedSize(310, 380);
    QVBoxLayout *v = layersPanel->body;
    auto *head = new QHBoxLayout;
    head->addWidget(heading("Layers"));
    head->addStretch();
    auto *close = new MenuButton("×");
    close->setFixedWidth(30);
    connect(close, &QAbstractButton::clicked, this, &MainWindow::toggleLayers);
    head->addWidget(close);
    v->addLayout(head);
    layerList = new QTreeWidget;
    layerList->setColumnCount(3);
    layerList->setHeaderLabels({"Show", "Lock", "Name (active one highlighted)"});
    layerList->setRootIsDecorated(false);
    layerList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layerList->setFocusPolicy(Qt::NoFocus);
    layerList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    layerList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layerList->header()->setStretchLastSection(true);
    v->addWidget(layerList);
    auto *row = new QHBoxLayout;
    row->setSpacing(3);
    auto button = [&](const QString &text, const QString &tip, auto fn) {
        auto *b = new MenuButton(text);
        b->setToolTip(tip);
        connect(b, &QAbstractButton::clicked, this, fn);
        row->addWidget(b);
        return b;
    };
    button("+ New", "New layer", [this] {
        canvas->activeLayer = addLayer(QString("Layer %1").arg(layers.size() + 1));
        refreshLayers();
        setDirty(true);
    });
    button("Delete", "Delete the active layer and its content", [this] {
        LayerItem *l = canvas->activeLayer;
        if (l && l->links) { say("The Links layer stays. Delete its links instead (show them with Shift+J)."); return; }
        if (!l || layers.size() <= 2) return;
        if (!l->childItems().isEmpty()
            && QMessageBox::question(this, "Vax Maps", QString("Delete the layer \"%1\" and its %2 items?").arg(l->name).arg(l->childItems().size()))
                   != QMessageBox::Yes)
            return;
        canvas->cancelDrafts();
        layers.removeOne(l);
        delete l;
        canvas->activeLayer = layers.first();
        restackLayers();
        refreshLayers();
        setDirty(true);
    })->ink = ui::danger;
    button("↑", "Move layer up", [this] {
        const int i = layers.indexOf(canvas->activeLayer);   // the Links layer stays on top
        if (i < 0 || i == layers.size() - 1 || layers[i]->links || layers[i + 1]->links) return;
        layers.swapItemsAt(i, i + 1);
        restackLayers();
        refreshLayers();
        setDirty(true);
    });
    button("↓", "Move layer down", [this] {
        const int i = layers.indexOf(canvas->activeLayer);
        if (i <= 0 || layers[i]->links) return;
        layers.swapItemsAt(i, i - 1);
        restackLayers();
        refreshLayers();
        setDirty(true);
    });
    v->addLayout(row);
    connect(layerList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *ti, int col) {
        if (col == 2) layerList->editItem(ti, 2);
    });
    connect(layerList, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *ti, int col) {
        if (refreshing) return;
        LayerItem *l = layerOf(ti);
        if (col == 0 && l->links) canvas->showLinks(ti->checkState(0) == Qt::Checked);   // hidden links still answer clicks
        else if (col == 0) l->setVisible(ti->checkState(0) == Qt::Checked);
        else if (col == 1) l->setLocked(ti->checkState(1) == Qt::Checked);
        else if (col == 2) l->name = ti->text(2);
        setDirty(true);
    });
    connect(layerList, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *cur) {
        if (cur) canvas->activeLayer = layerOf(cur);
    });
}

void MainWindow::toggleLayers() {
    if (layersPanel->isShown()) layersPanel->dismiss();
    else layersPanel->popUp(QPoint(canvas->width() - layersPanel->width() - 4, 4), QPoint(28, 0));
    layersButton->setChecked(layersPanel->isShown());
    canvas->setFocus();
}

void MainWindow::toggleFullScreen() {
    if (isFullScreen()) {
        wasMaximized ? showMaximized() : showNormal();
    } else {
        wasMaximized = isMaximized();
        showFullScreen();
        say("Full screen. F11 leaves it.");
    }
    fullButton->setChecked(isFullScreen());
}

void MainWindow::placeOverlays() {
    if (layersPanel->isShown()) layersPanel->move(canvas->width() - layersPanel->width() - 4, 4);
    toast->place();
    zoomToast->place();
}

void MainWindow::say(const QString &msg) {
    if (!msg.isEmpty()) toast->showMessage(msg, 2400 + 40 * int(msg.size()));
}

bool MainWindow::eventFilter(QObject *o, QEvent *e) {
    if (o == canvas && e->type() == QEvent::Resize) placeOverlays();
    if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
        auto *w = qobject_cast<QWidget *>(o);
        if (!w || w->window() != this) return false;
        bool swallow = false;
        for (Panel *p : {menu, props}) {
            if (!p->isShown() || p == w || p->isAncestorOf(w)) continue;
            p->dismiss();
            // a left click that closes a popup should not also draw or select on the canvas
            swallow |= w == canvas->viewport() && static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton;
        }
        return swallow;
    }
    if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape)
        for (Panel *p : {props, menu})
            if (p->isShown()) { p->dismiss(); canvas->setFocus(); return true; }
    return false;
}

void MainWindow::setDirty(bool d) {
    dirty = d;
    setWindowTitle(QString("Vax Maps - %1%2").arg(file.isEmpty() ? "Untitled" : QFileInfo(file).fileName(), d ? " *" : ""));
}

LayerItem *MainWindow::addLayer(const QString &name, bool links) {
    auto *l = new LayerItem(name);
    l->links = links;
    scene->addItem(l);
    const bool linksOnTop = !layers.isEmpty() && layers.last()->links;
    if (linksOnTop && !links) layers.insert(layers.size() - 1, l);   // new layers go under the Links layer
    else layers << l;
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
        ti->setCheckState(0, (l->links ? linksVisible : l->isVisible()) ? Qt::Checked : Qt::Unchecked);
        ti->setCheckState(1, l->locked ? Qt::Checked : Qt::Unchecked);
        ti->setText(2, l->name);
        ti->setData(2, Qt::UserRole, QVariant::fromValue(quintptr(l)));
        ti->setSizeHint(2, QSize(0, 30));
        if (l == active || !current) current = ti;
    }
    layerList->setCurrentItem(current);
    refreshing = false;
    canvas->activeLayer = layerOf(current);
}

MapItem *MainWindow::importMap(const QString &path, QPointF scenePos) {
    if (!canvas->layerUsable()) return nullptr;
    if (!QImageReader(path).canRead()) {
        say("Unsupported image format: " + QFileInfo(path).fileName());
        return nullptr;
    }
    auto *m = new MapItem(path);
    m->setParentItem(canvas->activeLayer);
    m->setPos(scenePos - m->boundingRect().center());
    setDirty(true);
    return m;
}

void MainWindow::importDialog() {
    const QStringList files = QFileDialog::getOpenFileNames(this, "Import maps", QString(),
        "Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff *.gif);;All files (*)");
    QPointF pos = canvas->mapToScene(canvas->viewport()->rect().center());
    for (const QString &f : files)
        if (MapItem *m = importMap(f, pos)) pos.rx() += m->boundingRect().width() + 100;
}

void MainWindow::newProject() {
    canvas->setTool(Canvas::Select);   // ends drafts and a calibration in progress
    props->dismiss();
    qDeleteAll(layers);
    layers.clear();
    for (const char *name : {"Maps", "Zones", "Drawings", "Text"}) addLayer(name);
    canvas->linksLayer = addLayer("Links", true);
    canvas->showLinks(false);
    canvas->activeLayer = layers.first();
    canvas->resetTransform();
    canvas->centerOn(0, 0);
    file.clear();
    refreshLayers();
    setDirty(false);
}

bool MainWindow::openProject(const QString &f) {
    QFile in(f);
    if (!in.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Vax Maps", "Could not open " + f + ": " + in.errorString());
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll(), &err);
    if (doc.isNull() || doc.object()["format"].toString() != "vaxmaps") {
        QMessageBox::warning(this, "Vax Maps", "Not a Vax Maps project: " + f);
        return false;
    }
    canvas->setTool(Canvas::Select);   // ends drafts and a calibration in progress
    props->dismiss();
    qDeleteAll(layers);
    layers = layersFromJson(doc.object(), scene, QFileInfo(f).absoluteDir());
    if (layers.isEmpty()) addLayer("Layer 1");
    canvas->linksLayer = nullptr;
    for (LayerItem *l : layers)
        if (l->links) canvas->linksLayer = l;
    if (!canvas->linksLayer) canvas->linksLayer = addLayer("Links", true);
    layers.removeOne(canvas->linksLayer);   // always on top
    layers << canvas->linksLayer;
    restackLayers();
    canvas->showLinks(false);
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
        QMessageBox::warning(this, "Vax Maps", "Could not save " + f + ": " + out.errorString());
        return false;
    }
    file = QFileInfo(f).absoluteFilePath();
    setDirty(false);
    return true;
}

bool MainWindow::save() { return file.isEmpty() ? saveAs() : saveTo(file); }

bool MainWindow::saveAs() {
    QString f = QFileDialog::getSaveFileName(this, "Save project", file.isEmpty() ? "map.vaxmap" : file,
                                             "Vax Maps project (*.vaxmap)");
    if (f.isEmpty()) return false;
    if (!f.endsWith(".vaxmap", Qt::CaseInsensitive)) f += ".vaxmap";
    return saveTo(f);
}

void MainWindow::open() {
    if (!maybeSave()) return;
    const QString f = QFileDialog::getOpenFileName(this, "Open project", QString(), "Vax Maps project (*.vaxmap)");
    if (!f.isEmpty()) openProject(f);
}

bool MainWindow::maybeSave() {
    if (!dirty) return true;
    const auto r = QMessageBox::question(this, "Vax Maps", "Save the changes to this project?",
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
