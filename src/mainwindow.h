#pragma once
#include <QMainWindow>

class Canvas;
class LayerItem;
class MapItem;
class QCheckBox;
class QDoubleSpinBox;
class QFormLayout;
class QGraphicsItem;
class QGraphicsScene;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

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

    Canvas *canvas;
    QGraphicsScene *scene;
    QList<LayerItem *> layers;   // bottom first
    bool dirty = false;

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void setDirty(bool d);
    void refreshLayers();
    void restackLayers();
    void onSelection();
    void pickColor();
    bool maybeSave();
    bool save();
    bool saveAs();
    void open();
    void importDialog();

    QString file;
    QTreeWidget *layerList;
    bool refreshing = false;
    QLabel *statusText, *zoomText;
    QPushButton *colorButton;
    // properties panel
    QGraphicsItem *propItem = nullptr;
    QFormLayout *props;
    QLabel *propKind;
    QLineEdit *nameEdit, *unitEdit;
    QPlainTextEdit *descEdit;
    QPushButton *propColor;
    QDoubleSpinBox *cellSpin, *perCellSpin;
    QCheckBox *gridCheck;
    QLabel *calHint;
};
