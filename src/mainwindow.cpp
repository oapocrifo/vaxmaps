#include "mainwindow.h"
#include "canvas.h"
#include "items.h"
#include "lang.h"
#include "mapitem.h"
#include "project.h"
#include "ui.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
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
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QtGui/private/qzipreader_p.h>
#include <QtGui/private/qzipwriter_p.h>
#include <QPointer>
#include <QSaveFile>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>

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

QLabel *heading(QLabel *l) {
    QFont f = l->font();
    f.setPixelSize(14);
    f.setBold(true);
    l->setFont(f);
    return l;
}

const char *clipboardFormat = "application/x-vaxmaps-items";

QString imageFilter() { return T("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff *.gif);;All files (*)"); }

bool writeJson(const QString &f, const QJsonObject &o, QString *error = nullptr) {
    QSaveFile out(f);   // writes to a temp file and renames: a failed save never truncates the old project
    const bool ok = out.open(QIODevice::WriteOnly) && out.write(QJsonDocument(o).toJson(QJsonDocument::Compact)) >= 0 && out.commit();
    if (!ok && error) *error = out.errorString();
    return ok;
}

template <class Fn> void eachMap(const QList<LayerItem *> &layers, Fn fn) {
    for (LayerItem *l : layers)
        for (QGraphicsItem *c : l->childItems())
            if (auto *m = qgraphicsitem_cast<MapItem *>(c)) fn(m);
}

bool imageMissing(const MapItem *m) { return !QFileInfo(m->src->path).isFile(); }

// What Find looks in: names of zones, links and tokens, descriptions of zones, the text of texts and notes.
QString searchText(QGraphicsItem *c) {
    if (c->type() == ZoneItem::Type || c->type() == LinkItem::Type) {
        auto *z = static_cast<ZoneItem *>(c);
        return z->name + '\n' + z->desc;
    }
    if (auto *k = qgraphicsitem_cast<TokenItem *>(c)) return k->name;
    if (auto *t = qgraphicsitem_cast<TextItem *>(c)) return t->toPlainText();
    return {};
}

QString searchLabel(QGraphicsItem *c) {
    QString s = searchText(c).trimmed();
    if (c->type() == ZoneItem::Type || c->type() == LinkItem::Type) {
        const QString name = static_cast<ZoneItem *>(c)->name.trimmed();
        if (!name.isEmpty()) s = name;
    }
    return s.section('\n', 0, 0).simplified();   // first line
}

void addRecent(const QString &f) {
    QSettings settings;
    QStringList recent = settings.value("recent").toStringList();
    recent.removeAll(f);
    recent.prepend(f);
    settings.setValue("recent", recent.mid(0, 5));
}

QByteArray clipboardItems() {
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    return mime ? mime->data(clipboardFormat) : QByteArray();
}

// The whole project, small, in a corner, with the visible area outlined. A click or a drag moves the view there.
class Minimap : public QWidget {
public:
    Minimap(Canvas *c, std::function<QRectF()> content) : QWidget(c), canvas(c), content(std::move(content)) {
        setFixedSize(220, 150);
        setCursor(Qt::PointingHandCursor);
        setProperty("wanted", true);
        connect(c->scene(), &QGraphicsScene::changed, this, [this] { stale = true; });
        for (QScrollBar *b : {c->horizontalScrollBar(), c->verticalScrollBar()})
            connect(b, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
        connect(c, &Canvas::zoomChanged, this, qOverload<>(&QWidget::update));
        timer.setInterval(1000);   // redrawn at most once a second: a map being dragged changes the scene 60 times a second
        connect(&timer, &QTimer::timeout, this, [this] {
            if (!stale) return;
            if (isVisible()) render();
            else { stale = false; area = this->content(); }   // hidden: only whether there is anything to show
            if (areaChanged) areaChanged();
        });
        timer.start();
    }
    QRectF area;   // scene rect shown
    std::function<void()> areaChanged;
    void render() {
        stale = false;
        area = content();
        image = QImage();
        if (!area.isEmpty()) {
            const QRectF r = target();
            image = QImage((r.size() * devicePixelRatioF()).toSize(), QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(devicePixelRatioF());
            image.fill(Qt::transparent);
            QPainter p(&image);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            canvas->scene()->render(&p, QRectF(QPointF(), r.size()), area);
        }
        update();
    }
protected:
    QRectF target() const {   // where `area` is drawn, keeping its proportions
        const QRectF box = rect().adjusted(6, 6, -6, -6);
        if (area.isEmpty()) return box;
        QSizeF s = area.size();
        s.scale(box.size(), Qt::KeepAspectRatio);
        return QRectF(box.center() - QPointF(s.width(), s.height()) / 2, s);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QColor(255, 255, 255, 40));
        p.setBrush(QColor(24, 25, 28, 225));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        if (image.isNull()) return;
        const QRectF t = target();
        p.drawImage(t.topLeft(), image);
        const QRectF seen = canvas->mapToScene(canvas->viewport()->rect()).boundingRect();
        const double k = t.width() / area.width();
        QRectF box((seen.left() - area.left()) * k + t.left(), (seen.top() - area.top()) * k + t.top(), seen.width() * k, seen.height() * k);
        p.setClipRect(rect().adjusted(2, 2, -2, -2));
        p.setPen(QPen(QColor(255, 210, 60), 1.5));
        p.setBrush(QColor(255, 210, 60, 30));
        if (box.width() < 4) box.adjust(-2, 0, 2, 0);   // zoomed far in: still a visible mark
        if (box.height() < 4) box.adjust(0, -2, 0, 2);
        p.drawRect(box);
    }
    void mousePressEvent(QMouseEvent *e) override { moveTo(e->position()); }
    void mouseMoveEvent(QMouseEvent *e) override { if (e->buttons() & Qt::LeftButton) moveTo(e->position()); }
    void wheelEvent(QWheelEvent *e) override { e->accept(); }
    void showEvent(QShowEvent *) override { render(); }
private:
    void moveTo(QPointF at) {
        if (area.isEmpty()) return;
        const QRectF t = target();
        const double k = area.width() / t.width();
        canvas->centerOn(area.topLeft() + (at - t.topLeft()) * k);
        update();
    }
    Canvas *canvas;
    std::function<QRectF()> content;
    QImage image;
    QTimer timer;
    bool stale = true;
};

} // namespace

MainWindow::MainWindow() : recoveryLock(recoveryFile() + ".lock") {
    scene = new QGraphicsScene(this);
    // "Infinite": ±5 million px stays inside the scrollbars' int range even at 64x zoom.
    scene->setSceneRect(-5e6, -5e6, 1e7, 1e7);
    canvas = new Canvas(scene, this);
    setCentralWidget(canvas);
    onSceneEdited = [this] { setDirty(true); };
    qApp->setStyleSheet(panelStyle);

    toast = new Toast(canvas, Toast::BottomCenter);
    zoomToast = new Toast(canvas, Toast::BottomRight);
    minimap = new Minimap(canvas, [this] {
        QRectF r;
        for (LayerItem *l : layers)
            if (l->isVisible() && !l->links) r |= l->childrenBoundingRect();
        return r;
    });
    static_cast<Minimap *>(minimap)->areaChanged = [this] { placeOverlays(); };
    minimap->hide();
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
    // Queued: this may ask to save, and a dialog shown inside the drop freezes the window the files came from.
    connect(canvas, &Canvas::filesDropped, this, [this](const QStringList &files, QPointF pos) {
        for (const QString &f : files)
            if (f.endsWith(".vaxmap", Qt::CaseInsensitive) || f.endsWith(".zip", Qt::CaseInsensitive)) { if (maybeSave()) openFromUi(f); return; }
        importMaps(files, pos);
    }, Qt::QueuedConnection);
    qApp->installEventFilter(this);   // closes popups on outside clicks and Esc, keeps overlays placed

    undoTimer.setSingleShot(true);
    undoTimer.setInterval(400);   // edits in a row (typing a name, dragging a slider) are one undo step
    connect(&undoTimer, &QTimer::timeout, this, [this] {
        if (QGuiApplication::mouseButtons()) undoTimer.start();   // not half way through a drag
        else snapshot();
    });
    autosaveTimer.setInterval(60000);
    connect(&autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    autosaveTimer.start();

    newProject();
    resize(1500, 950);
    say(T("Right click opens the menu. L shows the layers, F11 toggles full screen."));
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
    onSceneEdited = nullptr;
    scene->disconnect(this);   // it is destroyed after this object's members: deleting a selected item would call back into them
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
        {Canvas::ZoneEllipse, "Circle", "O"}, {Canvas::ZonePoly, "Vertices", "G"}, {Canvas::Link, "New link", "J"},
        {Canvas::Token, "Token", "B"}, {Canvas::Fog, "Fog brush", "F"}};
    auto *group = new QButtonGroup(this);
    QGridLayout *grid = nullptr, *toolsGrid = nullptr;
    for (int i = 0; i < int(std::size(tools)); ++i) {
        if (i == 0 || i == 8 || i == 11) {
            v->addWidget(ui::caption(i == 0 ? "Tools" : i == 8 ? "Zones" : "Links between maps"));
            grid = new QGridLayout;
            grid->setSpacing(3);
            v->addLayout(grid);
            if (i == 0) toolsGrid = grid;
        }
        const ToolDef t = tools[i];
        auto *b = new MenuButton(t.name, t.key);
        b->setCheckable(true);
        group->addButton(b);
        const int k = i < 8 ? i : i < 11 ? i - 8 : i == 11 ? 0 : i - 4;   // Token and Fog brush go after Calibrate
        (i >= 12 ? toolsGrid : grid)->addWidget(b, k / 3, k % 3);
        connect(b, &QAbstractButton::clicked, this, [this, t] { menu->dismiss(); canvas->setTool(t.tool); canvas->setFocus(); });
        toolButtons << b;
    }
    linksButton = new MenuButton("Show links", "Shift+J");
    linksButton->setCheckable(true);
    connect(linksButton, &QAbstractButton::clicked, this, [this](bool on) { menu->dismiss(); canvas->showLinks(on); });
    // Queued: the signal can come from the layer list's own itemChanged, and rebuilding the list there frees the item Qt is using.
    connect(canvas, &Canvas::linksShown, this, [this](bool on) { linksButton->setChecked(on); refreshLayers(); }, Qt::QueuedConnection);
    grid->addWidget(linksButton, 0, 1, 1, 2);
    fogButton = new MenuButton("Opaque fog");
    fogButton->setCheckable(true);
    fogButton->setTip("On: the fog hides the map, as the players would see it. Off: it only darkens the map.");
    connect(fogButton, &QAbstractButton::toggled, this, [this](bool on) {
        if (fogOpaque == on) return;
        fogOpaque = on;
        scene->update();
        setDirty(true);   // saved with the project
    });
    toolsGrid->addWidget(fogButton, 3, 1, 1, 2);
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
    grid->addWidget(menuButton(action("Find…", QKeySequence("Ctrl+F"), [this] { showFind(); })), 1, 2);
    minimapButton = new MenuButton("Minimap");
    minimapButton->setCheckable(true);
    minimapButton->setChecked(true);
    connect(minimapButton, &QAbstractButton::toggled, this, [this](bool on) { minimap->setProperty("wanted", on); placeOverlays(); });
    grid->addWidget(minimapButton, 2, 0);
    grid->addWidget(menuButton(action("Shortcuts", QKeySequence(Qt::Key_F1), [this] { showShortcuts(); })), 2, 1);
    action("Zoom in", QKeySequence::ZoomIn, [this] { canvas->setTransformationAnchor(QGraphicsView::AnchorViewCenter); canvas->zoomBy(1.25); });
    action("Zoom out", QKeySequence::ZoomOut, [this] { canvas->setTransformationAnchor(QGraphicsView::AnchorViewCenter); canvas->zoomBy(0.8); });
    v->addLayout(grid);

    v->addWidget(ui::caption("Project"));
    grid = new QGridLayout;
    grid->setSpacing(3);
    auto plain = [this](const QString &text, std::function<void()> fn) {   // a command without a shortcut
        auto *b = new MenuButton(text, " ");   // the empty hint aligns the text left, like its neighbours
        connect(b, &QAbstractButton::clicked, this, [this, fn] { menu->dismiss(); fn(); });
        return b;
    };
    // two columns: with their shortcuts, three Portuguese names do not fit in a row
    grid->addWidget(menuButton(action("New", QKeySequence::New, [this] { if (maybeSave()) newProject(); })), 0, 0);
    grid->addWidget(menuButton(action("Open…", QKeySequence::Open, [this] { open(); })), 0, 1);
    grid->addWidget(menuButton(action("Save", QKeySequence::Save, [this] { save(); })), 1, 0);
    grid->addWidget(menuButton(action("Save as…", QKeySequence("Ctrl+Shift+S"), [this] { saveAs(); })), 1, 1);
    grid->addWidget(plain("Open recent…", [this] { showRecent(); }), 2, 0);
    MenuButton *package = plain("Save package…", [this] { packageDialog(); });
    package->setTip("The project and all its images in one .zip file, to take to another computer");
    grid->addWidget(package, 2, 1);
    grid->addWidget(menuButton(action("Import maps…", QKeySequence("Ctrl+I"), [this] { importDialog(); })), 3, 0);
    grid->addWidget(menuButton(action("Export view…", QKeySequence("Ctrl+E"), [this] { exportDialog(); })), 3, 1);
    MenuButton *full = plain("Export full size…", [this] { exportFullDialog(); });
    full->setTip("The selection, or the visible area, at the maps' own resolution");
    grid->addWidget(full, 4, 0);
    MenuButton *clear = plain("Clear cache", [this] { clearCache(); });
    clear->setTip("Deletes the prepared copies of the maps not opened since the program started");
    grid->addWidget(clear, 4, 1);
    languageButton = plain(portuguese() ? "English" : "Português", [this] { setLanguage(!portuguese()); });
    languageButton->literal = true;   // each language's own name
    grid->addWidget(languageButton, 5, 0);
    grid->addWidget(menuButton(action("Quit", QKeySequence("Ctrl+Q"), [this] { close(); })), 5, 1);
    v->addLayout(grid);

    v->addWidget(ui::caption("Edit"));
    grid = new QGridLayout;
    grid->setSpacing(3);
    grid->addWidget(menuButton(action("Undo", QKeySequence("Ctrl+Z"), [this] { undo(); })), 0, 0);
    QAction *redoAction = action("Redo", QKeySequence("Ctrl+Y"), [this] { redo(); });
    redoAction->setShortcuts({QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")});
    grid->addWidget(menuButton(redoAction), 0, 1);
    action("Paste", QKeySequence("Ctrl+V"), [this] { paste(); });
    action("Copy", QKeySequence("Ctrl+C"), [this] { copySelection(); });       // these two are buttons in the properties popups
    action("Duplicate", QKeySequence("Ctrl+D"), [this] { duplicate(); });
    auto *pasteButton = new MenuButton("Paste", "Ctrl+V");   // from the menu the items go where the menu was opened
    connect(pasteButton, &QAbstractButton::clicked, this, [this] {
        menu->dismiss();
        pasteAt(clipboardItems(), canvas->mapToScene(canvas->viewport()->mapFrom(canvas, menuAt)));
    });
    grid->addWidget(pasteButton, 0, 2);
    auto *del = new MenuButton("Delete", "Del");
    del->ink = ui::danger;
    connect(del, &QAbstractButton::clicked, this, [this] { menu->dismiss(); canvas->deleteSelected(); });
    grid->addWidget(del, 1, 2);
    grid->addWidget(menuButton(action("Move to the active layer", QKeySequence("Ctrl+M"), [this] { moveSelectionToActiveLayer(); })), 1, 0, 1, 2);
    v->addLayout(grid);
}

void MainWindow::openMenu(QPoint at) {
    menuAt = at;
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

QVBoxLayout *MainWindow::newPopup() {
    menu->dismiss();
    props->hide();   // still fading out from a moment ago: content put in a visible panel was not measured, the popup came out empty
    props->setFixedWidth(330);   // the shortcuts list widens it
    delete propsContent;
    propsContent = new QWidget;
    auto *v = new QVBoxLayout(propsContent);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    props->body->addWidget(propsContent);
    return v;
}

void MainWindow::openProperties(QGraphicsItem *it, QPoint at) {
    QVBoxLayout *v = newPopup();
    auto title = [v](const QString &text) { v->addWidget(heading(new QLabel(text))); };
    auto deleteButton = [this](const QString &text) {
        auto *b = new MenuButton(text, "Del");
        b->ink = ui::danger;
        connect(b, &QAbstractButton::clicked, this, [this] { props->dismiss(); canvas->deleteSelected(); });
        return b;
    };
    auto copyRow = [this, v] {
        auto *row = new QHBoxLayout;
        row->setSpacing(3);
        for (bool copy : {true, false}) {
            auto *b = new MenuButton(copy ? "Copy" : "Duplicate", copy ? "Ctrl+C" : "Ctrl+D");
            connect(b, &QAbstractButton::clicked, this, [this, copy] { props->dismiss(); copy ? copySelection() : duplicate(); });
            row->addWidget(b);
        }
        v->addLayout(row);
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
        title(T("%1 items selected").arg(sel.size()));
        auto *move = new MenuButton("Move to the active layer", "Ctrl+M");
        connect(move, &QAbstractButton::clicked, this, [this] { props->dismiss(); moveSelectionToActiveLayer(); });
        v->addWidget(move);
        copyRow();
        v->addWidget(deleteButton("Delete selection"));
    } else if (z) {
        title(T(z->shapeKind == ZoneItem::Rect ? "Rectangle zone" : z->shapeKind == ZoneItem::Ellipse ? "Circle zone" : "Vertex zone"));
        v->addWidget(ui::caption("Name"));
        auto *name = new QLineEdit(z->name);
        name->setPlaceholderText(T("Unnamed"));
        connect(name, &QLineEdit::textChanged, this, [this, z](const QString &n) { z->name = n; z->update(); setDirty(true); });
        v->addWidget(name);
        colorRow(z->color, [z](const QColor &c) { z->color = c; z->update(); });
        v->addWidget(ui::caption("Description"));
        auto *desc = new QPlainTextEdit(z->desc);
        desc->setFixedHeight(96);
        desc->setPlaceholderText(T("Notes about this area"));
        connect(desc, &QPlainTextEdit::textChanged, this, [this, z, desc] { z->setDesc(desc->toPlainText()); setDirty(true); });
        v->addWidget(desc);
        v->addSpacing(4);
        copyRow();
        v->addWidget(deleteButton("Delete zone"));
    } else if (m) {
        title(T("Map") + " · " + QFileInfo(m->src->path).fileName());
        if (!m->src->error.isEmpty()) {   // the image moved or was renamed
            auto *locate = new MenuButton("Locate image…");
            connect(locate, &QAbstractButton::clicked, this, [this, m] {
                props->dismiss();
                const QString f = QFileDialog::getOpenFileName(this, T("Locate %1").arg(QFileInfo(m->src->path).fileName()), QString(), imageFilter());
                if (!f.isEmpty()) say(T("%1 image(s) located.").arg(locateImage(m, f)));
            });
            v->addWidget(locate);
        }
        const Calibration &cal = m->cal;
        v->addWidget(ui::caption("Grid"));
        auto *kind = new QLabel(!cal.valid() ? T("Not calibrated yet.")
                                : T("%1, cell of %2 px%3").arg(T(cal.grid == Calibration::Square ? "Squares" : cal.grid == Calibration::HexPointy ? "Hexes, pointy top" : "Hexes, flat top"))
                                      .arg(cal.cellSize(), 0, 'f', 2)
                                      .arg(cal.field.isEmpty() ? QString() : T(", follows local variations")));
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
        form->addWidget(new QLabel(T(cal.isHex() ? "1 hex" : "1 square")), 0, 0);
        form->addWidget(cell, 0, 1, 1, 2);
        form->addWidget(new QLabel(T("Is worth")), 1, 0);
        form->addWidget(per, 1, 1);
        form->addWidget(unit, 1, 2);
        v->addLayout(form);
        auto edit = [this, m](auto fn) { fn(m->cal); m->update(); setDirty(true); };
        // editingFinished also comes when the field just loses the focus: an untouched value must not drop the local corrections
        connect(cell, &QDoubleSpinBox::editingFinished, this, [edit, cell, shown = cell->value()]() mutable {
            if (cell->value() == shown) return;
            shown = cell->value();
            edit([cell](Calibration &c) { c.scaleCell(cell->value()); });
        });
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
        if (cal.valid() && !cal.isHex()) {
            v->addWidget(ui::caption("Diagonals"));
            auto *rules = new QHBoxLayout;
            rules->setSpacing(3);
            auto *group = new QButtonGroup(propsContent);
            const char *names[] = {"Count 1", "1-2-1", "Straight line"};
            const char *tips[] = {"Each diagonal step counts 1 square (D&D 5e)", "Diagonal steps count 1, 2, 1, 2... (5, 10, 5 ft)",
                                  "The straight-line distance between the squares"};
            for (int i = 0; i < 3; ++i) {
                auto *b = new MenuButton(names[i]);
                b->setTip(tips[i]);
                b->setCheckable(true);
                b->setChecked(cal.diagonal == i);
                group->addButton(b);
                connect(b, &QAbstractButton::clicked, this, [edit, i] { edit([i](Calibration &c) { c.diagonal = Calibration::Diagonal(i); }); });
                rules->addWidget(b);
            }
            v->addLayout(rules);
        }
        v->addWidget(ui::caption("Fog of war"));
        auto *fog = new MenuButton("Cover the map with fog");
        fog->setCheckable(true);
        fog->setChecked(!m->fog.isNull());
        fog->setEnabled(m->src->ready);
        fog->setTip("Then reveal parts of it with the fog brush (F). Off removes the fog.");
        connect(fog, &QAbstractButton::toggled, this, [this, m](bool on) {
            if (on) m->enableFog(true);
            else m->setFog(QImage());
            m->update();
            setDirty(true);
        });
        v->addWidget(fog);
        v->addSpacing(4);
        copyRow();
        v->addWidget(deleteButton("Remove map"));
    } else if (it->type() == LinkItem::Type) {
        auto *l = static_cast<LinkItem *>(it);
        title(T("Link between maps"));
        v->addWidget(ui::caption("Name"));
        auto *name = new QLineEdit(l->name);
        connect(name, &QLineEdit::textChanged, this, [this, l](const QString &n) {
            for (LinkItem *x : {l, l->partner}) if (x) { x->name = n; x->update(); }
            setDirty(true);
        });
        v->addWidget(name);
        colorRow(l->color, [l](const QColor &c) { for (LinkItem *x : {l, l->partner}) if (x) { x->color = c; x->update(); } });
        if (!l->partner) {
            auto *lonely = new QLabel(T("This end has no pair yet: drag one with New link (J)."));
            lonely->setWordWrap(true);
            v->addWidget(lonely);
        }
        auto *go = new MenuButton("Go to the other end", T("double click"));
        go->setEnabled(l->partner);
        connect(go, &QAbstractButton::clicked, this, [this, l] { props->dismiss(); canvas->travel(l, l->mapToScene(l->box().center())); });
        v->addWidget(go);
        v->addSpacing(4);
        v->addWidget(deleteButton("Delete link (both ends)"));
    } else if (auto *k = qgraphicsitem_cast<TokenItem *>(it)) {
        title(T("Token"));
        v->addWidget(ui::caption("Name"));
        auto *name = new QLineEdit(k->name);
        connect(name, &QLineEdit::textChanged, this, [this, k](const QString &n) { k->name = n; k->update(); setDirty(true); });
        v->addWidget(name);
        colorRow(k->color, [k](const QColor &c) { k->color = c; k->update(); });
        v->addWidget(ui::caption("Size in squares"));
        auto *sizes = new QHBoxLayout;
        sizes->setSpacing(3);
        auto *group = new QButtonGroup(propsContent);
        for (int cells = 1; cells <= 4; ++cells) {
            auto *b = new MenuButton(QString("%1×%1").arg(cells));
            b->literal = true;
            b->setCheckable(true);
            b->setChecked(k->cells == cells);
            group->addButton(b);
            connect(b, &QAbstractButton::clicked, this, [this, k, cells] {
                k->setDiameter(k->diameter * cells / k->cells);   // where no map is calibrated; snapToken sizes it to the grid
                k->cells = cells;
                snapToken(k);
                setDirty(true);
            });
            sizes->addWidget(b);
        }
        v->addLayout(sizes);
        v->addSpacing(4);
        copyRow();
        v->addWidget(deleteButton("Delete token"));
        props->popUp(at + QPoint(2, 2));
        name->setFocus();
        name->selectAll();
        return;
    } else if (t) {
        title(T(t->note ? "Note" : "Text"));
        colorRow(t->color, [t](const QColor &c) { t->setColor(c); });
        v->addWidget(ui::caption("Size"));
        auto *size = new QSlider(Qt::Horizontal);
        size->setRange(-60, 80);   // scale = 2^(value / 10): the whole zoom range a text can be made at
        size->setValue(qRound(10 * std::log2(t->scale())));
        size->setFocusPolicy(Qt::NoFocus);
        connect(size, &QSlider::valueChanged, this, [this, t](int x) { t->setScale(std::pow(2.0, x / 10.0)); setDirty(true); });
        v->addWidget(size);
        auto *editText = new MenuButton("Edit text", T("double click"));
        connect(editText, &QAbstractButton::clicked, this, [this, t] { props->dismiss(); canvas->setFocus(); t->startEditing(); });
        v->addWidget(editText);
        copyRow();
        v->addWidget(deleteButton(t->note ? "Delete note" : "Delete text"));
    } else if (s) {
        title(T("Stroke"));
        colorRow(s->pen().color(), [s](const QColor &c) { QPen p = s->pen(); p.setColor(c); s->setPen(p); });
        copyRow();
        v->addWidget(deleteButton("Delete stroke"));
    }
    props->popUp(at + QPoint(2, 2));
}

void MainWindow::buildLayersPanel() {
    layersPanel = new Panel(canvas, false);
    layersPanel->setFixedSize(310, 380);
    QVBoxLayout *v = layersPanel->body;
    auto *head = new QHBoxLayout;
    head->addWidget(heading(new Label("Layers")));
    head->addStretch();
    auto *close = new MenuButton("×");
    close->setFixedWidth(30);
    connect(close, &QAbstractButton::clicked, this, &MainWindow::toggleLayers);
    head->addWidget(close);
    v->addLayout(head);
    layerList = new QTreeWidget;
    layerList->setColumnCount(3);
    layerList->setHeaderLabels({T("Show"), T("Lock"), T("Name (active one highlighted)")});
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
        b->setTip(tip);
        connect(b, &QAbstractButton::clicked, this, fn);
        row->addWidget(b);
        return b;
    };
    button("+ New", "New layer", [this] {
        canvas->activeLayer = addLayer(T("Layer %1").arg(layers.size() + 1));
        refreshLayers();
        setDirty(true);
    });
    button("Delete", "Delete the active layer and its content", [this] {
        LayerItem *l = canvas->activeLayer;
        if (l && l->links) { say(T("The Links layer stays. Delete its links instead (show them with Shift+J).")); return; }
        if (!l || layers.size() <= 2) return;
        if (!l->childItems().isEmpty()
            && QMessageBox::question(this, "Vax Maps", T("Delete the layer \"%1\" and its %2 items?").arg(l->name, QString::number(l->childItems().size())))
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
    // The list takes no focus, so after a rename nothing had it and the tool keys were dead until a click on the canvas.
    connect(layerList->itemDelegate(), &QAbstractItemDelegate::closeEditor, canvas, [this] { if (!QApplication::focusWidget()) canvas->setFocus(); });
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
        say(T("Full screen. F11 leaves it."));
    }
    fullButton->setChecked(isFullScreen());
}

void MainWindow::placeOverlays() {
    if (layersPanel->isShown()) layersPanel->move(canvas->width() - layersPanel->width() - 4, 4);
    auto *mm = static_cast<Minimap *>(minimap);
    const bool show = minimap->property("wanted").toBool() && !mm->area.isEmpty() && canvas->height() > 400;
    minimap->move(8, canvas->height() - minimap->height() - 8);
    if (show != minimap->isVisible()) minimap->setVisible(show);
    if (show) minimap->raise();
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
    if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
        auto *w = qobject_cast<QWidget *>(o);
        if (!w || w->window() != this) return false;   // Esc in a dialog over the popup (the colour picker) is for the dialog
        for (Panel *p : {props, menu})
            if (p->isShown()) { p->dismiss(); canvas->setFocus(); return true; }
    }
    return false;
}

void MainWindow::setDirty(bool d) {
    dirty = d;
    setWindowTitle(QString("Vax Maps - %1%2").arg(file.isEmpty() ? T("Untitled") : QFileInfo(file).fileName(), d ? " *" : ""));
    if (d) undoTimer.start();                               // one undo step once the edits pause
    else if (ownsRecovery) QFile::remove(recoveryFile());   // saved, or given up on purpose
}

void MainWindow::changeEvent(QEvent *e) {
    if (e->type() == QEvent::LanguageChange && layerList) {
        layerList->setHeaderLabels({T("Show"), T("Lock"), T("Name (active one highlighted)")});
        setDirty(dirty);   // "Untitled" in the title
    }
    QMainWindow::changeEvent(e);
}

void MainWindow::setLanguage(bool pt) {
    canvas->setTool(Canvas::Select);   // the calibration panel writes its texts as its steps go
    scene->clearSelection();           // handles carry tooltips
    props->dismiss();
    setPortuguese(pt);                 // every widget gets a LanguageChange event
    QSettings().setValue("language", pt ? "pt" : "en");
    languageButton->setText(pt ? "English" : "Português");
    say(T("Right click opens the menu. L shows the layers, F11 toggles full screen."));
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
        say(T("Unsupported image format: %1").arg(QFileInfo(path).fileName()));
        return nullptr;
    }
    auto *m = new MapItem(path);
    m->setParentItem(canvas->activeLayer);
    m->setPos(scenePos - m->boundingRect().center());
    setDirty(true);
    return m;
}

void MainWindow::importMaps(const QStringList &files, QPointF at) {
    // Rows and columns, 100 px apart. One long row made 30 maps of 6000 px a canvas 183 000 px wide.
    const int columns = int(std::ceil(std::sqrt(double(files.size()))));
    QPointF origin;
    double x = 0, y = 0, rowHeight = 0;
    int placed = 0;
    for (const QString &f : files) {
        MapItem *m = importMap(f, at);
        if (!m) continue;
        if (!placed) origin = m->pos();   // the first map stays centred on the drop point
        m->setPos(origin + QPointF(x, y));
        x += m->boundingRect().width() + 100;
        rowHeight = qMax(rowHeight, m->boundingRect().height());
        if (++placed % columns == 0) {
            x = 0;
            y += rowHeight + 100;
            rowHeight = 0;
        }
    }
}

void MainWindow::importDialog() {
    importMaps(QFileDialog::getOpenFileNames(this, T("Import maps"), QString(), imageFilter()),
               canvas->mapToScene(canvas->viewport()->rect().center()));
}

void MainWindow::newProject() {
    canvas->setTool(Canvas::Select);   // ends drafts and a calibration in progress
    canvas->clearRulers();             // they are not layer items: they would stay over the new project
    props->dismiss();
    qDeleteAll(layers);
    layers.clear();
    for (const char *name : {"Maps", "Zones", "Drawings", "Text"}) addLayer(T(name));
    fogOpaque = false;
    fogButton->setChecked(false);
    canvas->linksLayer = addLayer(T("Links"), true);
    canvas->showLinks(false);
    canvas->activeLayer = layers.first();
    canvas->resetTransform();
    canvas->centerOn(0, 0);
    file.clear();
    refreshLayers();
    setDirty(false);
    resetHistory();
}

// Replaces every layer by the ones stored in `o`: a project file or an undo step.
void MainWindow::setLayers(const QJsonObject &o, const QDir &base) {
    props->dismiss();
    qDeleteAll(layers);
    layers = layersFromJson(o, scene, base);
    if (layers.isEmpty()) addLayer(T("Layer %1").arg(1));
    canvas->linksLayer = nullptr;
    for (LayerItem *l : layers)
        if (l->links) canvas->linksLayer = l;
    if (!canvas->linksLayer) canvas->linksLayer = addLayer(T("Links"), true);
    layers.removeOne(canvas->linksLayer);   // always on top
    layers << canvas->linksLayer;
    restackLayers();
}

bool MainWindow::openProject(const QString &f) {
    QFile in(f);
    if (!in.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Vax Maps", T("Could not open %1: %2").arg(f, in.errorString()));
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll());
    if (doc.isNull() || doc.object()["format"].toString() != "vaxmaps") {
        QMessageBox::warning(this, "Vax Maps", T("Not a Vax Maps project: %1").arg(f));
        return false;
    }
    canvas->setTool(Canvas::Select);   // ends drafts and a calibration in progress
    canvas->clearRulers();
    setLayers(doc.object(), QFileInfo(f).absoluteDir());
    canvas->showLinks(false);
    canvas->activeLayer = layers.first();
    const QJsonObject v = doc.object()["view"].toObject();
    canvas->resetTransform();
    const double z = qBound(0.005, v["zoom"].toDouble(1), 64.0);
    canvas->scale(z, z);
    canvas->centerOn(v["cx"].toDouble(), v["cy"].toDouble());
    emit canvas->zoomChanged(z);
    fogOpaque = v["fogOpaque"].toBool();
    fogButton->setChecked(fogOpaque);
    // The recovery copy opens as the project it came from, with its changes still unsaved.
    // Only that copy may name another file: any other project could point Save at a file of its choice.
    const bool recovered = QFileInfo(f) == QFileInfo(recoveryFile());
    file = recovered ? doc.object()["recoveredFrom"].toString() : QFileInfo(f).absoluteFilePath();
    if (!recovered) addRecent(file);
    refreshLayers();
    setDirty(recovered);
    resetHistory();
    return true;
}

bool MainWindow::openFromUi(const QString &f) {
    if (f.endsWith(".zip", Qt::CaseInsensitive)) return openPackage(f);
    if (!openProject(f)) return false;
    askMissingImages();
    return true;
}

// ---- missing images ----

int MainWindow::locateImage(MapItem *map, const QString &path) {
    // The other missing images are looked for beside the one just found: a moved folder is fixed in one go.
    const QDir dir = QFileInfo(path).absoluteDir();
    int found = 0;
    eachMap(layers, [&](MapItem *m) {
        if (m != map && !imageMissing(m)) return;
        const QString candidate = m == map ? path : dir.filePath(QFileInfo(m->src->path).fileName());
        if (!QFileInfo(candidate).isFile()) return;
        m->setSource(candidate);
        ++found;
    });
    if (found) setDirty(true);
    return found;
}

void MainWindow::askMissingImages() {
    for (;;) {
        MapItem *first = nullptr;
        int missing = 0;
        eachMap(layers, [&](MapItem *m) {
            if (!imageMissing(m)) return;
            if (!first) first = m;
            ++missing;
        });
        if (!first) return;
        const QString name = QFileInfo(first->src->path).fileName();
        if (QMessageBox::question(this, "Vax Maps", T("%1 image(s) of this project were not found. The first one is %2. Locate it now?").arg(missing).arg(name))
            != QMessageBox::Yes)
            return;
        const QString f = QFileDialog::getOpenFileName(this, T("Locate %1").arg(name), QString(), imageFilter());
        if (f.isEmpty()) return;
        say(T("%1 image(s) located.").arg(locateImage(first, f)));
    }
}

// ---- saving, recovery copy ----

QJsonObject MainWindow::toJson(const QDir &base) const {
    QJsonObject o = layersToJson(layers, base);
    const QPointF c = canvas->mapToScene(canvas->viewport()->rect().center());
    o["view"] = QJsonObject{{"cx", c.x()}, {"cy", c.y()}, {"zoom", canvas->zoom()}, {"fogOpaque", fogOpaque}};
    return o;
}

bool MainWindow::saveTo(const QString &f) {
    QString error;
    if (!writeJson(f, toJson(QFileInfo(f).absoluteDir()), &error)) {
        QMessageBox::warning(this, "Vax Maps", T("Could not save %1: %2").arg(f, error));
        return false;
    }
    file = QFileInfo(f).absoluteFilePath();
    addRecent(file);
    setDirty(false);
    return true;
}

bool MainWindow::save() { return file.isEmpty() ? saveAs() : saveTo(file); }

bool MainWindow::saveAs() {
    QString f = QFileDialog::getSaveFileName(this, T("Save project"), file.isEmpty() ? T("map.vaxmap") : file,
                                             T("Vax Maps project (*.vaxmap)"));
    if (f.isEmpty()) return false;
    if (!f.endsWith(".vaxmap", Qt::CaseInsensitive)) f += ".vaxmap";
    return saveTo(f);
}

void MainWindow::open() {
    if (!maybeSave()) return;
    const QString f = QFileDialog::getOpenFileName(this, T("Open project"), QString(), T("Vax Maps project or package (*.vaxmap *.zip)"));
    if (!f.isEmpty()) openFromUi(f);
}

bool MainWindow::maybeSave() {
    if (!dirty) return true;
    const auto r = QMessageBox::question(this, "Vax Maps", T("Save the changes to this project?"),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Save) return save();
    return r == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (!maybeSave()) return e->ignore();
    if (ownsRecovery) QFile::remove(recoveryFile());   // closed on purpose: nothing to recover
    saveSession();
    e->accept();
}

QString MainWindow::recoveryFile() {
    return (dataDir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : dataDir) + "/recovery.vaxmap";
}

bool MainWindow::offerRecovery() {
    QDir().mkpath(QFileInfo(recoveryFile()).path());
    recoveryLock.setStaleLockTime(0);
    ownsRecovery = recoveryLock.tryLock();   // a second window of the program leaves the first one's copy alone
    const QFileInfo copy(recoveryFile());
    if (!ownsRecovery || !copy.exists()) return false;
    const bool restore = QMessageBox::question(this, "Vax Maps",
                             T("Vax Maps did not close normally and left unsaved work from %1. Restore it? No discards that work.")
                                 .arg(QLocale().toString(copy.lastModified(), QLocale::ShortFormat))) == QMessageBox::Yes;
    if (restore && openProject(recoveryFile())) {
        askMissingImages();
        return true;
    }
    QFile::remove(recoveryFile());
    return false;
}

void MainWindow::autosave() {
    if (!ownsRecovery || !dirty || QGuiApplication::mouseButtons()) return;   // not half way through a drag: the next minute will do
    QJsonObject o = toJson(QFileInfo(recoveryFile()).absoluteDir());
    o["recoveredFrom"] = file;
    writeJson(recoveryFile(), o);   // a failure stays silent: it is tried again in a minute
}

QStringList MainWindow::recentFiles() const { return QSettings().value("recent").toStringList(); }

void MainWindow::showRecent() {
    const QPoint at = menu->pos();
    QVBoxLayout *v = newPopup();
    v->addWidget(heading(new QLabel(T("Recent projects"))));
    bool any = false;
    for (const QString &f : recentFiles()) {
        if (!QFileInfo(f).isFile()) continue;
        auto *b = new MenuButton(QFileInfo(f).fileName(), QFileInfo(f).dir().dirName());
        b->literal = true;
        b->setToolTip(QDir::toNativeSeparators(f));
        connect(b, &QAbstractButton::clicked, this, [this, f] {
            props->dismiss();
            if (maybeSave()) openFromUi(f);
        });
        v->addWidget(b);
        any = true;
    }
    if (!any) v->addWidget(new QLabel(T("No project opened or saved yet.")));
    props->popUp(at);
}

// ---- undo ----

// A step is the whole project, as pieces shared with the step before where nothing changed (see layersToParts).
QList<QByteArray> MainWindow::state() const { return layersToParts(layers); }

void MainWindow::resetHistory() {
    undoTimer.stop();
    undoStack = {state()};
    redoStack.clear();
}

void MainWindow::snapshot() {
    undoTimer.stop();
    const QList<QByteArray> now = state();
    if (now == undoStack.last()) return;   // marked as changed, with nothing different
    undoStack << now;
    redoStack.clear();
    if (undoStack.size() > 100) undoStack.removeFirst();
}

void MainWindow::restore(const QList<QByteArray> &step) {
    scene->clearSelection();
    canvas->cancelDrafts();
    if (canvas->tool() == Canvas::Calibrate) canvas->setTool(Canvas::Calibrate);   // the map it was measuring is replaced
    const int active = layers.indexOf(canvas->activeLayer);
    setLayers(layersFromParts(step), QDir::root());
    reuseParts(layers, step);
    canvas->activeLayer = layers.value(active, layers.first());
    refreshLayers();
    setDirty(true);
    undoTimer.stop();   // loading the step is not an edit
}

void MainWindow::undo() {
    if (undoTimer.isActive()) snapshot();   // an edit made in the last 0.4 s
    if (undoStack.size() < 2) return say(T("Nothing to undo."));
    redoStack << undoStack.takeLast();
    restore(undoStack.last());
}

void MainWindow::redo() {
    if (undoTimer.isActive()) snapshot();
    if (redoStack.isEmpty()) return say(T("Nothing to redo."));
    undoStack << redoStack.takeLast();
    restore(undoStack.last());
}

// ---- copy, paste, duplicate ----

QList<QGraphicsItem *> MainWindow::selectionInOrder() const {
    QList<QGraphicsItem *> items;   // bottom first, so the copies stack as the originals do
    for (LayerItem *l : layers)
        for (QGraphicsItem *c : l->childItems())
            if (c->isSelected() && c->type() != LinkItem::Type) items << c;   // a link is a pair: one end alone means nothing
    return items;
}

void MainWindow::copySelection() {
    const QList<QGraphicsItem *> items = selectionInOrder();
    if (items.isEmpty()) return;
    auto *mime = new QMimeData;
    mime->setData(clipboardFormat, QJsonDocument(itemsToJson(items, QDir::root())).toJson(QJsonDocument::Compact));
    QGuiApplication::clipboard()->setMimeData(mime);
    say(T("%1 item(s) copied.").arg(items.size()));
}

void MainWindow::pasteAt(const QByteArray &json, QPointF centre) {
    if (json.isEmpty() || !canvas->layerUsable()) return;
    QList<QGraphicsItem *> items;
    for (QGraphicsItem *it : itemsFromJson(QJsonDocument::fromJson(json).array(), QDir::root())) {
        if (it->type() == LinkItem::Type) delete it;   // links live only in their layer, in pairs
        else items << it;
    }
    scene->clearSelection();
    QRectF box;
    for (QGraphicsItem *it : items) {
        it->setParentItem(canvas->activeLayer);
        box |= it->sceneBoundingRect();
    }
    for (QGraphicsItem *it : items) {
        it->moveBy(centre.x() - box.center().x(), centre.y() - box.center().y());
        it->setSelected(true);
    }
    if (!items.isEmpty()) setDirty(true);
}

void MainWindow::paste() {
    QPoint at = canvas->viewport()->mapFromGlobal(QCursor::pos());
    if (!canvas->viewport()->rect().contains(at)) at = canvas->viewport()->rect().center();
    pasteAt(clipboardItems(), canvas->mapToScene(at));
}

void MainWindow::duplicate() {
    const QList<QGraphicsItem *> items = selectionInOrder();
    if (items.isEmpty()) return;
    QRectF box;
    for (QGraphicsItem *it : items) box |= it->sceneBoundingRect();
    pasteAt(QJsonDocument(itemsToJson(items, QDir::root())).toJson(QJsonDocument::Compact), box.center() + QPointF(20, 20) / canvas->zoom());
}

// ---- find, export, cache ----

QList<QGraphicsItem *> MainWindow::findNamed(const QString &text) const {
    QList<QGraphicsItem *> found;
    const QString part = text.trimmed();
    if (part.isEmpty()) return found;
    for (LayerItem *l : layers)
        if (l->isVisible())
            for (QGraphicsItem *c : l->childItems())
                if (searchText(c).contains(part, Qt::CaseInsensitive)) found << c;
    return found;
}

void MainWindow::goTo(QGraphicsItem *it) {
    canvas->glideTo(it->sceneBoundingRect().center());
    scene->clearSelection();
    it->setSelected(true);   // ignored for hidden links and locked layers: the view still goes there
}

void MainWindow::showFind() {
    QVBoxLayout *v = newPopup();
    v->addWidget(heading(new QLabel(T("Find"))));
    auto *edit = new QLineEdit;
    edit->setPlaceholderText(T("Name or text"));
    v->addWidget(edit);
    auto *results = new QVBoxLayout;
    results->setSpacing(2);
    v->addLayout(results);
    connect(edit, &QLineEdit::textChanged, this, [this, edit, results] {
        while (QLayoutItem *old = results->takeAt(0)) {
            delete old->widget();
            delete old;
        }
        const QList<QGraphicsItem *> found = findNamed(edit->text());
        for (QGraphicsItem *it : found.mid(0, 8)) {
            auto *b = new MenuButton(searchLabel(it), static_cast<LayerItem *>(it->parentItem())->name);
            b->literal = true;
            connect(b, &QAbstractButton::clicked, this, [this, it] { props->dismiss(); goTo(it); });
            results->addWidget(b);
        }
        if (found.isEmpty() && !edit->text().trimmed().isEmpty()) results->addWidget(new QLabel(T("Nothing found.")));
        for (int i = 0; i < results->count(); ++i) results->itemAt(i)->widget()->show();   // Qt shows them only later, and unshown they are not measured
        props->adjustSize();
    });
    connect(edit, &QLineEdit::returnPressed, this, [this, edit] {
        const QList<QGraphicsItem *> found = findNamed(edit->text());
        if (found.isEmpty()) return;
        props->dismiss();
        goTo(found.first());
    });
    props->popUp(QPoint((canvas->width() - props->width()) / 2, 40));
    edit->setFocus();
}

bool MainWindow::exportView(const QString &f) {
    scene->clearSelection();   // no handles or dashed outlines in the picture
    // shortcut: screen resolution. Render the scene at the maps' own resolution if exports need more detail.
    return canvas->viewport()->grab().save(f);
}

void MainWindow::exportDialog() {
    const QFileInfo project(file);
    QString f = QFileDialog::getSaveFileName(this, T("Export view"), file.isEmpty() ? T("view.png") : project.dir().filePath(project.completeBaseName() + ".png"),
                                             T("PNG image (*.png);;JPEG image (*.jpg)"));
    if (f.isEmpty()) return;
    if (QFileInfo(f).suffix().isEmpty()) f += ".png";
    if (exportView(f)) say(T("View exported to %1").arg(QFileInfo(f).fileName()));
    else QMessageBox::warning(this, "Vax Maps", T("Could not write the image %1").arg(f));
}

// ---- full size export, package, session, shortcuts ----

void MainWindow::exportFullDialog() {
    QRectF area;
    for (QGraphicsItem *it : scene->selectedItems()) area |= it->sceneBoundingRect();
    if (area.isEmpty()) area = canvas->mapToScene(canvas->viewport()->rect()).boundingRect();
    bool preparing = false;
    eachMap(layers, [&](MapItem *m) { preparing = preparing || (m->isVisible() && !m->src->ready && m->src->error.isEmpty() && m->sceneBoundingRect().intersects(area)); });
    if (preparing) return say(T("Some maps there are still being prepared. Try again in a moment."));
    const QFileInfo project(file);
    QString f = QFileDialog::getSaveFileName(this, T("Export full size"), file.isEmpty() ? T("view.png") : project.dir().filePath(project.completeBaseName() + ".png"),
                                             T("PNG image (*.png);;JPEG image (*.jpg)"));
    if (f.isEmpty()) return;
    if (QFileInfo(f).suffix().isEmpty()) f += ".png";
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QSize size = exportFull(f, area);
    QApplication::restoreOverrideCursor();
    if (size.isEmpty()) QMessageBox::warning(this, "Vax Maps", T("Could not write the image %1").arg(f));
    else say(T("Exported %1 (%2 x %3 px)").arg(QFileInfo(f).fileName()).arg(size.width()).arg(size.height()));
}

QSize MainWindow::exportFull(const QString &f, const QRectF &area) {
    if (area.isEmpty()) return {};
    // 1 image px per scene unit: maps are at their own size in the scene. At most 100 megapixels (400 MB in memory).
    double k = std::min(1.0, std::sqrt(1e8 / (area.width() * area.height())));
    k = std::min(k, 30000 / std::max(area.width(), area.height()));
    const QSize size = (area.size() * k).toSize().expandedTo(QSize(1, 1));
    QImage img(size, QImage::Format_RGB32);
    if (img.isNull()) return {};
    img.fill(canvas->backgroundBrush().color());
    scene->clearSelection();   // no handles or dashed outlines
    syncTiles = true;          // every tile at full detail before render() returns
    {
        QPainter p(&img);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
        scene->render(&p, QRectF(QPointF(), size), area);
    }
    syncTiles = false;
    return img.save(f, nullptr, 92) ? size : QSize();
}

void MainWindow::packageDialog() {
    const QFileInfo project(file);
    QString f = QFileDialog::getSaveFileName(this, T("Save package"), file.isEmpty() ? T("map.zip") : project.dir().filePath(project.completeBaseName() + ".zip"),
                                             T("Vax Maps package (*.zip)"));
    if (f.isEmpty()) return;
    if (!f.endsWith(".zip", Qt::CaseInsensitive)) f += ".zip";
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = savePackage(f);
    QApplication::restoreOverrideCursor();
    if (ok) say(T("Package saved: %1").arg(QFileInfo(f).fileName()));
}

bool MainWindow::savePackage(const QString &zipFile) {
    QHash<QString, QString> inside;   // image file -> its path in the package
    QSet<QString> taken;
    QStringList missing;
    eachMap(layers, [&](MapItem *m) {
        const QString path = m->src->path;
        if (inside.contains(path)) return;
        const QFileInfo fi(path);
        if (!fi.isFile()) { missing << fi.fileName(); return; }
        QString name = fi.fileName();
        for (int n = 2; taken.contains(name.toLower()); ++n) name = QString("%1 (%2).%3").arg(fi.completeBaseName()).arg(n).arg(fi.suffix());
        taken.insert(name.toLower());
        inside.insert(path, "images/" + name);
    });
    QJsonObject o = toJson(QDir::root());
    QJsonArray ls = o["layers"].toArray();
    for (int i = 0; i < ls.size(); ++i) {   // the maps point at their copies, relative to the project
        QJsonObject l = ls[i].toObject();
        QJsonArray items = l["items"].toArray();
        for (int j = 0; j < items.size(); ++j) {
            QJsonObject it = items[j].toObject();
            if (it["type"].toString() != "map") continue;
            const QString abs = QDir::cleanPath(QDir::root().absoluteFilePath(it["path"].toString()));
            if (inside.contains(abs)) it["path"] = inside.value(abs);
            items[j] = it;
        }
        l["items"] = items;
        ls[i] = l;
    }
    o["layers"] = ls;
    // Written beside it and renamed at the end: a failure never leaves a broken package under the real name.
    // Not QSaveFile: QZipWriter closes the device itself, which QSaveFile does not allow.
    const QString part = zipFile + ".part";
    QFile out(part);
    bool ok = out.open(QIODevice::WriteOnly);
    if (ok) {
        QZipWriter zip(&out);
        zip.setCompressionPolicy(QZipWriter::AlwaysCompress);
        zip.addFile(QFileInfo(file.isEmpty() ? T("map.vaxmap") : file).completeBaseName() + ".vaxmap", QJsonDocument(o).toJson(QJsonDocument::Compact));
        zip.setCompressionPolicy(QZipWriter::NeverCompress);   // images are compressed already
        for (auto it = inside.cbegin(); ok && it != inside.cend(); ++it) {
            QFile img(it.key());
            ok = img.open(QIODevice::ReadOnly);
            if (ok) zip.addFile(it.value(), &img);
        }
        zip.close();
        ok = ok && zip.status() == QZipWriter::NoError && out.error() == QFileDevice::NoError;
        if (ok && QFile::exists(zipFile)) ok = QFile::remove(zipFile);
        ok = ok && QFile::rename(part, zipFile);
    }
    if (!ok) {
        const QString why = out.errorString();
        QFile::remove(part);
        QMessageBox::warning(this, "Vax Maps", T("Could not save %1: %2").arg(zipFile, why));
        return false;
    }
    if (!missing.isEmpty())
        QMessageBox::warning(this, "Vax Maps", T("These images were not found and are not in the package: %1").arg(missing.join(", ")));
    return true;
}

bool MainWindow::openPackage(const QString &zipFile) {
    QZipReader zip(zipFile);
    QString project;
    const QList<QZipReader::FileInfo> entries = zip.isReadable() ? zip.fileInfoList() : QList<QZipReader::FileInfo>();
    for (const QZipReader::FileInfo &e : entries)
        if (e.isFile && !e.filePath.contains('/') && e.filePath.endsWith(".vaxmap", Qt::CaseInsensitive)) project = e.filePath;
    if (project.isEmpty()) {
        QMessageBox::warning(this, "Vax Maps", T("Not a Vax Maps package: %1").arg(zipFile));
        return false;
    }
    // Extracted beside the .zip, into a new folder named after it.
    const QFileInfo zi(zipFile);
    QString dir = zi.absoluteDir().filePath(zi.completeBaseName());
    for (int n = 2; QFileInfo::exists(dir); ++n) dir = zi.absoluteDir().filePath(QString("%1 (%2)").arg(zi.completeBaseName()).arg(n));
    for (const QZipReader::FileInfo &e : entries) {
        if (!e.isFile) continue;
        // Names come from the file: none may lead outside the folder (../, absolute paths, drive letters).
        const QString rel = QDir::cleanPath(QString(e.filePath).replace('\\', '/'));
        if (rel.isEmpty() || rel.startsWith("..") || rel.startsWith('/') || rel.contains(':')) continue;
        const QString target = dir + '/' + rel;
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile out(target);
        if (!out.open(QIODevice::WriteOnly) || out.write(zip.fileData(e.filePath)) < 0 || !out.commit()) {
            QMessageBox::warning(this, "Vax Maps", T("Could not save %1: %2").arg(target, out.errorString()));
            return false;
        }
    }
    if (!openProject(dir + '/' + project)) return false;
    say(T("Package extracted to %1").arg(QDir::toNativeSeparators(dir)));
    askMissingImages();
    return true;
}

void MainWindow::restoreSession() {
    QSettings s;
    s.beginGroup("session");
    if (s.contains("geometry")) restoreGeometry(s.value("geometry").toByteArray());
    const QColor c(s.value("color").toString());
    if (c.isValid()) {
        canvas->color = c;
        menuSwatches->setCurrent(c);
    }
    widthSlider->setValue(s.value("width", 4).toInt());
    minimapButton->setChecked(s.value("minimap", true).toBool());
    const int tool = s.value("tool", Canvas::Select).toInt();
    show();
    if (tool >= Canvas::Select && tool <= Canvas::Fog && tool != Canvas::Calibrate && tool != Canvas::Link) canvas->setTool(Canvas::Tool(tool));
    if (s.value("fullScreen").toBool()) toggleFullScreen();
    if (s.value("layers").toBool()) toggleLayers();
}

void MainWindow::saveSession() {
    const bool full = isFullScreen();
    if (full) toggleFullScreen();   // the normal window is what geometry keeps
    QSettings s;
    s.beginGroup("session");
    s.setValue("fullScreen", full);
    s.setValue("geometry", saveGeometry());
    s.setValue("layers", layersPanel->isShown());
    s.setValue("minimap", minimapButton->isChecked());
    s.setValue("tool", int(canvas->tool()));
    s.setValue("color", canvas->color.name(QColor::HexArgb));
    s.setValue("width", widthSlider->value());
}

void MainWindow::showShortcuts() {
    static const char *const keys[][2] = {
        {"V", "Select"}, {"H", "Pan"}, {"P", "Pen"}, {"E", "Eraser"}, {"T", "Text"}, {"N", "Note"}, {"M", "Ruler"},
        {"K", "Calibrate"}, {"B", "Token"}, {"F", "Fog brush"}, {"Alt + drag", "Cover with the fog brush"}, {"R", "Rectangle"},
        {"O", "Circle"}, {"G", "Vertices"}, {"J", "New link"}, {"Shift+J", "Show links"}, {"S", "Snap to grid"}, {"L", "Layers"},
        {"F11", "Full screen"}, {"Ctrl+0", "Fit all"}, {"Ctrl+1", "Zoom 100%"}, {"Wheel", "Zoom at the pointer"},
        {"Space + drag", "Pan (also the middle button)"}, {"Right click", "Menu, or the properties of the selected item"},
        {"Double click", "Edit a text, add a vertex, jump through a shown link"}, {"Del", "Delete the selection"},
        {"Arrows", "Move the selection 1 px (Shift: 10 px)"}, {"Esc", "Cancel, clear the rulers"}, {"Ctrl+Z / Ctrl+Y", "Undo / Redo"},
        {"Ctrl+C / Ctrl+V", "Copy / Paste"}, {"Ctrl+D", "Duplicate"}, {"Ctrl+M", "Move to the active layer"}, {"Ctrl+F", "Find…"},
        {"Ctrl+N / Ctrl+O", "New / Open…"}, {"Ctrl+S", "Save"}, {"Ctrl+I", "Import maps…"}, {"Ctrl+E", "Export view…"},
        {"Ctrl+Q", "Quit"}};
    QVBoxLayout *v = newPopup();
    props->setFixedWidth(720);
    v->addWidget(heading(new QLabel(T("Shortcuts"))));
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(3);
    const int rows = (int(std::size(keys)) + 1) / 2;
    for (int i = 0; i < int(std::size(keys)); ++i) {
        const QString k = keys[i][0];
        static const QStringList words{"Alt + drag", "Wheel", "Space + drag", "Right click", "Double click", "Arrows"};
        auto *key = new QLabel(words.contains(k) ? T(keys[i][0]) : k);   // key names stay as on the keyboard
        key->setStyleSheet(QString("color: %1;").arg(ui::muted.name()));
        auto *what = new QLabel(T(keys[i][1]));
        grid->addWidget(key, i % rows, (i / rows) * 2, Qt::AlignTop);
        grid->addWidget(what, i % rows, (i / rows) * 2 + 1, Qt::AlignTop);
    }
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    v->addLayout(grid);
    props->popUp(QPoint((canvas->width() - 720) / 2, 30));
}

void MainWindow::clearCache(std::function<void(qint64)> done) {
    say(T("Clearing the tile cache…"));
    MapData::clearCache([self = QPointer<MainWindow>(this), done](qint64 freed) {
        if (self) self->say(T("Tile cache cleared: %1 MB freed. The maps opened since the program started keep theirs.").arg(freed / 1048576));
        if (done) done(freed);
    });
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
        if (it->type() == LinkItem::Type) continue;   // links live only in the Links layer: elsewhere they are dropped when the project is opened
        const QPointF sp = it->scenePos();
        it->setParentItem(active);
        it->setPos(active->mapFromScene(sp));
        active->applyLock(it);
    }
    setDirty(true);
}
