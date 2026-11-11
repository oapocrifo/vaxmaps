#pragma once
#include <QLockFile>
#include <QMainWindow>
#include <QTimer>
#include <functional>

class Canvas;
class LayerItem;
class MapItem;
class MenuButton;
class Panel;
class QAction;
class QDir;
class QGraphicsItem;
class QGraphicsScene;
class QJsonObject;
class QLabel;
class QSlider;
class QTreeWidget;
class QVBoxLayout;
class SwatchRow;
class Toast;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    MapItem *importMap(const QString &path, QPointF scenePos);
    void importMaps(const QStringList &files, QPointF scenePos);   // rows and columns, the first map centred on scenePos
    LayerItem *addLayer(const QString &name, bool links = false);
    void newProject();
    bool openProject(const QString &file);
    bool openFromUi(const QString &file);                // openProject, then asks for the images it did not find
    bool saveTo(const QString &file);
    void fitAll();
    void moveSelectionToActiveLayer();
    void openMenu(QPoint at);                           // canvas coordinates
    void openProperties(QGraphicsItem *item, QPoint at);
    void toggleLayers();
    void toggleFullScreen();
    void undo();
    void redo();
    void copySelection();
    void paste();                                       // at the mouse pointer
    void duplicate();
    bool exportView(const QString &file);               // what the canvas shows, without the selection marks
    QList<QGraphicsItem *> findNamed(const QString &text) const;   // zones and links whose name contains it
    void goTo(QGraphicsItem *item);
    int locateImage(MapItem *map, const QString &path); // also the other missing images found in that folder; returns how many
    void clearCache(std::function<void(qint64)> done = nullptr);
    // Recovery copy: written every minute while there are unsaved changes, removed when they are saved or discarded.
    static QString recoveryFile();
    bool offerRecovery();                               // call once after show(): restores what a crash left, if the user wants
    void autosave();
    QStringList recentFiles() const;
    void setLanguage(bool portuguese);

    Canvas *canvas;
    QGraphicsScene *scene;
    QList<LayerItem *> layers;   // bottom first
    bool dirty = false;
    Panel *menu, *props, *layersPanel;
    Toast *toast, *zoomToast;
    QAction *fullScreenAction;

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;
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
    void exportDialog();
    void askMissingImages();
    QVBoxLayout *newPopup();     // empties the properties popup for other content
    void showRecent();
    void showFind();
    void setLayers(const QJsonObject &o, const QDir &base);
    QJsonObject toJson(const QDir &base) const;
    QList<QByteArray> state() const;   // the layers and their items: one undo step
    void snapshot();
    void restore(const QList<QByteArray> &state);
    void resetHistory();
    QList<QGraphicsItem *> selectionInOrder() const;
    void pasteAt(const QByteArray &json, QPointF sceneCentre);

    QString file;
    QTreeWidget *layerList = nullptr;
    bool refreshing = false, wasMaximized = false, ownsRecovery = false;
    QList<MenuButton *> toolButtons;
    MenuButton *snapButton, *layersButton, *fullButton, *linksButton, *languageButton;
    SwatchRow *menuSwatches;
    QSlider *widthSlider;
    QLabel *widthLabel;
    QWidget *propsContent = nullptr;
    QPoint menuAt;               // where the menu was opened: Paste puts the items there
    QList<QList<QByteArray>> undoStack, redoStack;   // the last of undoStack is the current state
    QTimer undoTimer, autosaveTimer;
    QLockFile recoveryLock;
};
