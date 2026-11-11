#pragma once
#include <QMainWindow>
#include <functional>

class Canvas;
class LayerItem;
class MapItem;
class MenuButton;
class Panel;
class QAction;
class QGraphicsItem;
class QGraphicsScene;
class QLabel;
class QSlider;
class QTreeWidget;
class SwatchRow;
class Toast;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    MapItem *importMap(const QString &path, QPointF scenePos);
    LayerItem *addLayer(const QString &name);
    void newProject();
    bool openProject(const QString &file);
    bool saveTo(const QString &file);
    void fitAll();
    void moveSelectionToActiveLayer();
    void openMenu(QPoint at);                           // canvas coordinates
    void openProperties(QGraphicsItem *item, QPoint at);
    void toggleLayers();
    void toggleFullScreen();

    Canvas *canvas;
    QGraphicsScene *scene;
    QList<LayerItem *> layers;   // bottom first
    bool dirty = false;
    Panel *menu, *props, *layersPanel;
    Toast *toast, *zoomToast;
    QAction *fullScreenAction;

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    QAction *action(const QString &text, const QKeySequence &keys, std::function<void()> fn);
    MenuButton *menuButton(QAction *a);
    void buildMenu();
    void buildLayersPanel();
    void placeOverlays();
    void say(const QString &msg);
    void setDirty(bool d);
    void refreshLayers();
    void restackLayers();
    bool maybeSave();
    bool save();
    bool saveAs();
    void open();
    void importDialog();

    QString file;
    QTreeWidget *layerList;
    bool refreshing = false, wasMaximized = false;
    QList<MenuButton *> toolButtons;
    MenuButton *snapButton, *layersButton, *fullButton;
    SwatchRow *menuSwatches;
    QSlider *widthSlider;
    QLabel *widthLabel;
    QWidget *propsContent = nullptr;
};
