#pragma once
// Minimal overlay UI: floating panels with enter/exit animations, buttons with animated hover and press,
// colour swatches and fading notices. All of them are child widgets drawn over the canvas.
#include <QAbstractButton>
#include <QLabel>
#include <QPointer>
#include <QVariantAnimation>
#include <QWidget>

class QGraphicsOpacityEffect;
class QVBoxLayout;
class QTimer;

namespace ui {
const QColor text(232, 232, 236), muted(146, 148, 156), accent(92, 140, 255), danger(235, 90, 90);
constexpr int shadow = 10;   // px of drop shadow around a panel
// Animates `target` towards `to` over `ms`, starting from wherever it is now.
void glide(QVariantAnimation &a, double from, double to, int ms);
QWidget *caption(const QString &english);   // small section title
} // namespace ui

// A label whose text follows the interface language.
class Label : public QLabel {
public:
    explicit Label(const QString &english, bool upper = false);
protected:
    void changeEvent(QEvent *e) override;
private:
    QString key;
    bool upper;
};

class Panel : public QWidget {
    Q_OBJECT
public:
    explicit Panel(QWidget *parent, bool autoClose = true);
    // Shows the panel with its top-left at `pos` (kept inside the parent), sliding in from pos + `from`.
    void popUp(QPoint pos, QPoint from = QPoint(0, 10));
    void dismiss();
    bool isShown() const { return shown; }
    const bool autoClose;   // closes on a click outside it
    QVBoxLayout *body;
signals:
    void dismissed();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
private:
    void animate(bool in, QPoint from, QPoint to);
    QGraphicsOpacityEffect *fx;
    QPointer<QAbstractAnimation> running;
    QPoint slide;
    bool shown = false;
};

// The text is English and is shown in the interface language (see lang.h), unless `literal` is set.
class MenuButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit MenuButton(const QString &text, const QString &hint = {}, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    void setTip(const QString &english);
    QColor tint = ui::accent;   // background when checked
    QColor ink = ui::text;      // label colour (ui::danger for destructive actions)
    bool literal = false;       // a file or item name: shown as it is
protected:
    void paintEvent(QPaintEvent *) override;
    void changeEvent(QEvent *e) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
private:
    QString label() const;
    QString hint, tip;
    double hover = 0, press = 0, check = 0;
    QVariantAnimation hoverA, pressA, checkA;
};

class SwatchRow : public QWidget {
    Q_OBJECT
public:
    explicit SwatchRow(QWidget *parent = nullptr);
    void setCurrent(const QColor &c);
    QSize sizeHint() const override;
signals:
    void picked(const QColor &c);
protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
private:
    int indexAt(QPoint p) const;
    QList<QColor> colors;   // the last slot opens the colour dialog
    QColor current;
    int hovered = -1;
    QVector<double> grow;
    QTimer *tick;
};

class Toast : public QWidget {
    Q_OBJECT
public:
    enum Anchor { BottomCenter, BottomRight };
    Toast(QWidget *parent, Anchor anchor);
    void showMessage(const QString &text, int ms = 2800);
    void place();
protected:
    void paintEvent(QPaintEvent *) override;
private:
    Anchor anchor;
    QString message;
    QGraphicsOpacityEffect *fx;
    QVariantAnimation fade;
    QTimer *hold;
};
