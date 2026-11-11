#include "ui.h"
#include "lang.h"

#include <QColorDialog>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>

namespace ui {

void glide(QVariantAnimation &a, double from, double to, int ms) {
    a.stop();
    a.setStartValue(from);
    a.setEndValue(to);
    a.setDuration(ms);
    a.start();
}

QWidget *caption(const QString &t) {
    auto *l = new Label(t, true);
    QFont f = l->font();
    f.setPixelSize(10);
    f.setBold(true);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    l->setFont(f);
    l->setStyleSheet(QString("color: %1; padding-top: 6px;").arg(muted.name()));
    return l;
}

} // namespace ui

// ---- Label ----

Label::Label(const QString &english, bool upper) : key(english), upper(upper) {
    setText(upper ? T(key).toUpper() : T(key));
}

void Label::changeEvent(QEvent *e) {
    if (e->type() == QEvent::LanguageChange) setText(upper ? T(key).toUpper() : T(key));
    QLabel::changeEvent(e);
}

// ---- Panel ----

Panel::Panel(QWidget *parent, bool autoClose) : QWidget(parent), autoClose(autoClose), fx(new QGraphicsOpacityEffect(this)) {
    fx->setOpacity(0);
    fx->setEnabled(false);
    setGraphicsEffect(fx);
    body = new QVBoxLayout(this);
    body->setContentsMargins(ui::shadow + 12, ui::shadow + 10, ui::shadow + 12, ui::shadow + 12);
    body->setSpacing(4);
    hide();
}

void Panel::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(ui::shadow, ui::shadow, -ui::shadow, -ui::shadow);
    p.setPen(Qt::NoPen);
    for (int i = ui::shadow; i > 0; i -= 2) {   // soft shadow, a little lower than the panel
        p.setBrush(QColor(0, 0, 0, 7));
        p.drawRoundedRect(r.adjusted(-i, -i + 3, i, i + 3), 12 + i, 12 + i);
    }
    p.setBrush(QColor(29, 30, 35));
    p.setPen(QPen(QColor(255, 255, 255, 26), 1));
    p.drawRoundedRect(r, 12, 12);
}

void Panel::mousePressEvent(QMouseEvent *e) { e->accept(); }   // clicks on the panel never reach the canvas
void Panel::wheelEvent(QWheelEvent *e) {
    e->accept();   // the wheel over a panel never zooms the canvas
    // A panel taller than the window (the menu on a small screen) scrolls with it instead of staying cut off.
    const int hidden = height() - parentWidget()->height();
    if (hidden > 0) move(x(), qBound(-hidden, y() + e->angleDelta().y() / 2, 0));
}

void Panel::popUp(QPoint pos, QPoint from) {
    adjustSize();
    const QWidget *p = parentWidget();
    pos.setX(qBound(0, pos.x(), qMax(0, p->width() - width())));
    pos.setY(qBound(0, pos.y(), qMax(0, p->height() - height())));
    slide = from;
    shown = true;
    show();
    raise();
    animate(true, pos + from, pos);
}

void Panel::dismiss() {
    if (!shown) return;
    shown = false;
    animate(false, this->pos(), this->pos() + QPoint(slide.x() / 2, slide.y() / 2));
    emit dismissed();
}

void Panel::animate(bool in, QPoint from, QPoint to) {
    if (running) running->stop();
    fx->setEnabled(true);
    auto *group = new QParallelAnimationGroup(this);
    auto *op = new QPropertyAnimation(fx, "opacity");
    op->setStartValue(fx->opacity());
    op->setEndValue(in ? 1.0 : 0.0);
    auto *mv = new QPropertyAnimation(this, "pos");
    mv->setStartValue(from);
    mv->setEndValue(to);
    for (QPropertyAnimation *a : {op, mv}) {
        a->setDuration(in ? 190 : 140);
        a->setEasingCurve(in ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
        group->addAnimation(a);
    }
    connect(group, &QAbstractAnimation::finished, this, [this] {
        if (shown) fx->setEnabled(false);   // no offscreen rendering while the panel just sits there
        else hide();
    });
    running = group;
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

// ---- MenuButton ----

MenuButton::MenuButton(const QString &text, const QString &hint, QWidget *parent) : QAbstractButton(parent), hint(hint) {
    setText(text);
    setFocusPolicy(Qt::NoFocus);   // keys stay on the canvas
    setCursor(Qt::PointingHandCursor);
    for (auto [anim, value] : {std::pair{&hoverA, &hover}, std::pair{&pressA, &press}, std::pair{&checkA, &check}}) {
        anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(anim, &QVariantAnimation::valueChanged, this, [this, value](const QVariant &v) { *value = v.toDouble(); update(); });
    }
    connect(this, &QAbstractButton::toggled, this, [this](bool on) { ui::glide(checkA, check, on ? 1 : 0, 180); });
}

QString MenuButton::label() const { return literal ? text() : T(text()); }

QSize MenuButton::sizeHint() const {
    const QFontMetrics fm(font());
    return QSize(fm.horizontalAdvance(label()) + (hint.isEmpty() ? 0 : fm.horizontalAdvance(hint) + 18) + 22, 30);
}

void MenuButton::setTip(const QString &english) {
    tip = english;
    setToolTip(T(tip));
}

void MenuButton::changeEvent(QEvent *e) {
    if (e->type() == QEvent::LanguageChange) {
        if (!tip.isEmpty()) setToolTip(T(tip));
        updateGeometry();
        update();
    }
    QAbstractButton::changeEvent(e);
}

void MenuButton::enterEvent(QEnterEvent *) { ui::glide(hoverA, hover, 1, 140); }
void MenuButton::leaveEvent(QEvent *) { ui::glide(hoverA, hover, 0, 220); }

void MenuButton::mousePressEvent(QMouseEvent *e) {
    ui::glide(pressA, press, 1, 70);
    QAbstractButton::mousePressEvent(e);
}

void MenuButton::mouseReleaseEvent(QMouseEvent *e) {
    ui::glide(pressA, press, 0, 260);
    QAbstractButton::mouseReleaseEvent(e);
}

void MenuButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    p.translate(r.center());
    p.scale(1 - 0.04 * press, 1 - 0.04 * press);   // pressed buttons sink a little
    p.translate(-r.center());
    p.setPen(Qt::NoPen);
    QColor tinted = tint;
    tinted.setAlphaF(0.75 * check);
    p.setBrush(tinted);
    p.drawRoundedRect(r, 7, 7);
    p.setBrush(QColor(255, 255, 255, int(16 * hover + 34 * press)));
    p.drawRoundedRect(r, 7, 7);
    QFont f = font();
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(isEnabled() ? ink : ui::muted.darker(130));
    const QRectF tr = r.adjusted(10, 0, -10, 0);
    QFont hf = f;
    hf.setPixelSize(11);
    const int room = int(tr.width()) - (hint.isEmpty() ? 0 : QFontMetrics(hf).horizontalAdvance(hint) + 8);   // long names are cut in the middle
    p.drawText(tr, Qt::AlignVCenter | (hint.isEmpty() ? Qt::AlignHCenter : Qt::AlignLeft), QFontMetrics(f).elidedText(label(), Qt::ElideMiddle, room));
    if (!hint.isEmpty()) {
        p.setPen(check > 0.5 ? ui::text : ui::muted);
        p.setFont(hf);
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignRight, hint);
    }
}

// ---- SwatchRow ----

SwatchRow::SwatchRow(QWidget *parent) : QWidget(parent), tick(new QTimer(this)) {
    colors = {QColor(230, 60, 60), QColor(240, 140, 40), QColor(240, 210, 60), QColor(70, 190, 90), QColor(40, 180, 170),
              QColor(60, 120, 240), QColor(150, 90, 220), QColor(230, 90, 170), QColor(245, 245, 245), QColor(125, 125, 130),
              QColor(25, 25, 28), QColor()};
    grow = QVector<double>(colors.size(), 0);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    tick->setInterval(16);
    connect(tick, &QTimer::timeout, this, [this] {
        bool moving = false;
        for (int i = 0; i < grow.size(); ++i) {
            const double target = i == hovered ? 1 : 0;
            grow[i] += (target - grow[i]) * 0.3;
            if (std::abs(target - grow[i]) > 0.01) moving = true;
            else grow[i] = target;
        }
        update();
        if (!moving) tick->stop();
    });
}

QSize SwatchRow::sizeHint() const { return QSize(colors.size() * 24, 28); }

void SwatchRow::setCurrent(const QColor &c) { current = c; update(); }

int SwatchRow::indexAt(QPoint p) const {
    const int i = p.x() / 24;
    return i >= 0 && i < colors.size() ? i : -1;
}

void SwatchRow::mouseMoveEvent(QMouseEvent *e) {
    const int i = indexAt(e->position().toPoint());
    if (i != hovered) { hovered = i; tick->start(); }
}

void SwatchRow::leaveEvent(QEvent *) { hovered = -1; tick->start(); }

void SwatchRow::mousePressEvent(QMouseEvent *e) {
    const int i = indexAt(e->position().toPoint());
    if (i < 0) return;
    grow[i] = -0.4;   // a quick shrink that springs back
    tick->start();
    QColor c = colors[i];
    if (!c.isValid()) c = QColorDialog::getColor(current.isValid() ? current : Qt::white, this, T("Colour"));
    if (!c.isValid()) return;
    current = c;
    emit picked(c);
}

void SwatchRow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < colors.size(); ++i) {
        const QPointF c(i * 24 + 12, height() / 2.0);
        const double r = 8 + 2.5 * grow[i];
        const bool isCurrent = colors[i].isValid() && colors[i] == current;
        if (isCurrent || (!colors[i].isValid() && current.isValid() && !colors.contains(current))) {
            p.setPen(QPen(ui::text, 1.6));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, r + 3, r + 3);
        }
        if (colors[i].isValid()) {
            p.setPen(QPen(QColor(255, 255, 255, 40), 1));
            p.setBrush(colors[i]);
            p.drawEllipse(c, r, r);
        } else {   // "+": any other colour
            QConicalGradient g(c, 0);
            for (int k = 0; k <= 6; ++k) g.setColorAt(k / 6.0, QColor::fromHsv((k * 60) % 360, 200, 240));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(c, r, r);
            p.setPen(QPen(Qt::white, 1.8));
            p.drawLine(c + QPointF(-4, 0), c + QPointF(4, 0));
            p.drawLine(c + QPointF(0, -4), c + QPointF(0, 4));
        }
    }
}

// ---- Toast ----

Toast::Toast(QWidget *parent, Anchor anchor) : QWidget(parent), anchor(anchor), fx(new QGraphicsOpacityEffect(this)), hold(new QTimer(this)) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    fx->setOpacity(0);
    setGraphicsEffect(fx);
    hold->setSingleShot(true);
    fade.setEasingCurve(QEasingCurve::InOutQuad);
    connect(&fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { fx->setOpacity(v.toDouble()); });
    connect(&fade, &QVariantAnimation::finished, this, [this] { if (fx->opacity() < 0.01) hide(); });
    connect(hold, &QTimer::timeout, this, [this] { ui::glide(fade, fx->opacity(), 0, 450); });
    hide();
}

void Toast::showMessage(const QString &text, int ms) {
    message = text;
    QFont f = font();
    f.setPixelSize(13);
    setFont(f);
    const QFontMetrics fm(f);
    const int maxW = qMax(200, parentWidget()->width() - 80);
    const QRect tr = fm.boundingRect(QRect(0, 0, maxW - 32, 1000), Qt::TextWordWrap, text);
    resize(tr.width() + 32, tr.height() + 18);
    place();
    show();   // not raised: popups stay above notices
    ui::glide(fade, fx->opacity(), 1, 160);
    hold->start(ms);
}

void Toast::place() {
    const QWidget *p = parentWidget();
    move(anchor == BottomCenter ? (p->width() - width()) / 2 : p->width() - width() - 16, p->height() - height() - 18);
}

void Toast::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 22), 1));
    p.setBrush(QColor(22, 23, 27));
    const double radius = qMin(12.0, height() / 2.0);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    p.setPen(ui::text);
    p.drawText(rect().adjusted(16, 9, -16, -9), Qt::TextWordWrap | Qt::AlignCenter, message);
}
