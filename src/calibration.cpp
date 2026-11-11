#include "calibration.h"
#include "canvas.h"
#include "items.h"
#include "mapitem.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QHash>
#include <QImageReader>
#include <QKeyEvent>
#include <QLocale>
#include <QLabel>
#include <QLineEdit>
#include <QPen>
#include <QSet>
#include <QThreadPool>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int ANALYSIS_SIDE = 4096;     // Atlas MAX_ANALYSIS_SIDE
constexpr double HINT_RANGE = 0.13;     // Atlas HINT_SIZE_RANGE
constexpr double MIN_HINTED = 0.15;     // a measured cell needs less support than a detected one
constexpr double MIN_DETECTED = 0.35;
constexpr double DENSER_PREFERENCE = 0.75;   // Atlas DENSER_GRID_PREFERENCE
constexpr double CLICK_TOLERANCE = 0.25;     // fitted lines must pass this close (in cells) to the clicked corners

struct Phase { double strength = 0, offset = 0; };   // offset in profile px, within [0, period)

// Folds a profile over `period` px: lines repeating at that period pile up in one phase.
Phase fold(const QVector<float> &p, double period) {
    const int n = p.size(), bins = std::max(8, int(period));
    if (n < 16 || period < 4 || period > n / 3.0) return {};
    std::vector<double> sum(bins, 0), count(bins, 0);
    const double k = bins / period;
    double pos = 2.5 * k, total = 0;   // sample x sits at x + 0.5; the first 2 px have no probe
    pos = std::fmod(pos, bins);
    for (int x = 2; x < n - 2; ++x) {
        const int b = std::min(int(pos), bins - 1);
        sum[b] += p[x];
        count[b] += 1;
        total += p[x];
        pos += k;
        if (pos >= bins) pos -= bins;
    }
    const double mean = total / (n - 4);
    if (mean <= 0) return {};
    std::vector<double> v(bins), s(bins);
    for (int b = 0; b < bins; ++b) v[b] = count[b] > 0 ? sum[b] / count[b] : mean;
    for (int b = 0; b < bins; ++b) s[b] = (v[(b + bins - 1) % bins] + 2 * v[b] + v[(b + 1) % bins]) / 4;
    std::vector<double> sorted = s;
    std::nth_element(sorted.begin(), sorted.begin() + bins / 2, sorted.end());
    const double median = sorted[bins / 2];
    const int best = int(std::max_element(s.begin(), s.end()) - s.begin());
    // Centre of the peak, so a thick line (two edges) gives its middle.
    const int w = std::max(1, bins / 8);
    double wsum = 0, dsum = 0;
    for (int d = -w; d <= w; ++d) {
        const double a = s[(best + d + bins) % bins] - median;
        if (a > 0) { wsum += a; dsum += a * d; }
    }
    const double centre = best + 0.5 + (wsum > 0 ? dsum / wsum : 0);
    return {(s[best] - median) / mean, std::fmod(centre / k + period, period)};
}

// A square grid shows in both directions: its support is the weaker of the two.
GridFit score(const LineProfiles &p, double cell) {
    const Phase x = fold(p.x, cell * p.sx), y = fold(p.y, cell * p.sy);
    return {cell, QPointF(x.offset / p.sx, y.offset / p.sy), std::min(x.strength, y.strength)};
}

// Best cell in [lo, hi], sampled finely enough that the lines drift at most a quarter pixel over the map.
GridFit search(const LineProfiles &p, double lo, double hi) {
    const int n = std::max(p.x.size(), p.y.size());
    GridFit best;
    for (double c = lo; c <= hi; c *= 1 + 0.25 / n) {
        const GridFit g = score(p, c);
        if (g.strength > best.strength) best = g;
    }
    return best;
}

double circularMean(const QVector<double> &values, double period) {
    double s = 0, c = 0;
    for (double v : values) {
        s += std::sin(v / period * 2 * M_PI);
        c += std::cos(v / period * 2 * M_PI);
    }
    const double r = std::atan2(s, c) / (2 * M_PI) * period;
    return r < 0 ? r + period : r;
}

double distanceToLine(double v, double origin, double cell) {
    const double r = std::fmod(std::fmod(v - origin, cell) + cell, cell);
    return std::min(r, cell - r);
}

QHash<QString, QSharedPointer<const LineProfiles>> profileCache;
QSet<QString> profilesLoading;

} // namespace

LineProfiles lineProfiles(const QImage &image, QSize fullSize) {
    const QImage g = image.convertToFormat(QImage::Format_Grayscale8);
    const int w = g.width(), h = g.height(), r = 2;
    LineProfiles p;
    if (w < 16 || h < 16 || fullSize.isEmpty()) return p;
    p.x = QVector<float>(w, 0);
    p.y = QVector<float>(h, 0);
    p.sx = double(w) / fullSize.width();
    p.sy = double(h) / fullSize.height();
    // |2·centre − both sides|: strong on thin lines, dark or light, weak on flat areas.
    for (int y = 0; y < h; ++y) {
        const uchar *l = g.constScanLine(y);
        for (int x = r; x < w - r; ++x) p.x[x] += std::abs(2 * l[x] - l[x - r] - l[x + r]);
    }
    for (int y = r; y < h - r; ++y) {
        const uchar *u = g.constScanLine(y - r), *l = g.constScanLine(y), *d = g.constScanLine(y + r);
        float s = 0;
        for (int x = 0; x < w; ++x) s += std::abs(2 * l[x] - u[x] - d[x]);
        p.y[y] = s;
    }
    return p;
}

LineProfiles loadLineProfiles(const QString &path) {
    QImageReader reader(path);
    const QSize full = reader.size();
    if (full.isValid() && std::max(full.width(), full.height()) > ANALYSIS_SIDE)
        reader.setScaledSize(full.scaled(ANALYSIS_SIDE, ANALYSIS_SIDE, Qt::KeepAspectRatio));
    const QImage img = reader.read();
    return img.isNull() ? LineProfiles{} : lineProfiles(img, full.isValid() ? full : img.size());
}

std::optional<GridFit> fitGrid(const LineProfiles &p, double cellHint) {
    if (p.x.isEmpty() || !(cellHint > 0)) return {};
    const GridFit g = search(p, cellHint * (1 - HINT_RANGE), cellHint * (1 + HINT_RANGE));
    if (g.strength < MIN_HINTED) return {};
    return g;
}

std::optional<GridFit> detectGrid(const LineProfiles &p) {
    if (p.x.isEmpty()) return {};
    const int n = std::max(p.x.size(), p.y.size());
    const double lo = 8 / std::min(p.sx, p.sy);   // Atlas MIN_CELL_SIZE: 8 analysis px
    const double hi = std::min(p.x.size() / p.sx, p.y.size() / p.sy) / 4;
    if (hi <= lo) return {};
    // Coarse pass (lines may drift 2 px), then the peaks refined.
    std::vector<GridFit> coarse;
    for (double c = lo; c <= hi; c *= 1 + 2.0 / n) coarse.push_back(score(p, c));
    double top = 0;
    for (const GridFit &g : coarse) top = std::max(top, g.strength);
    std::vector<GridFit> peaks;
    for (size_t i = 0; i < coarse.size(); ++i) {
        const double s = coarse[i].strength;
        if (s < 0.5 * top) continue;
        bool isPeak = true;
        for (size_t j = (i > 3 ? i - 3 : 0); j <= std::min(coarse.size() - 1, i + 3) && isPeak; ++j)
            isPeak = j == i || coarse[j].strength <= s;
        if (isPeak) peaks.push_back(search(p, coarse[i].cell * (1 - 2.0 / n), coarse[i].cell * (1 + 2.0 / n)));
    }
    if (peaks.empty()) return {};
    double best = 0;
    for (const GridFit &g : peaks) best = std::max(best, g.strength);
    if (best < MIN_DETECTED) return {};
    // A multiple of the grid has all its lines on the map's lines too: the densest well-supported one is the grid.
    std::optional<GridFit> chosen;
    for (const GridFit &g : peaks)
        if (g.strength >= DENSER_PREFERENCE * best && (!chosen || g.cell < chosen->cell)) chosen = g;
    return chosen;
}

GridFit gridFromMeasurements(const QVector<Measurement> &m) {
    GridFit g;
    if (m.isEmpty()) return g;
    for (const Measurement &p : m) g.cell += std::max(std::abs(p.b.x() - p.a.x()), std::abs(p.b.y() - p.a.y()));
    g.cell /= m.size();
    QVector<double> xs, ys;
    for (const Measurement &p : m) xs << p.a.x() << p.b.x(), ys << p.a.y() << p.b.y();
    g.origin = QPointF(circularMean(xs, g.cell), circularMean(ys, g.cell));
    return g;
}

// ---- panel ----

Calibrator::Calibrator(Canvas *c) : Panel(c, false), canvas(c) {
    setFixedWidth(360);
    QVBoxLayout *v = body;
    title = new QLabel;
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    v->addWidget(title);
    detectButton = new MenuButton("Encontrar grade automaticamente", "F");
    v->addWidget(detectButton);
    instruction = new QLabel;
    instruction->setWordWrap(true);
    instruction->setMinimumHeight(54);
    instruction->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    v->addWidget(instruction);
    progress = new QLabel;
    v->addWidget(progress);
    auto *distance = new QHBoxLayout;
    distance->addWidget(new QLabel("1 quadrado vale"));
    perCell = new QDoubleSpinBox;
    perCell->setRange(0.001, 1e6);
    perCell->setDecimals(2);
    distance->addWidget(perCell);
    unit = new QLineEdit;
    unit->setMaximumWidth(50);
    distance->addWidget(unit);
    v->addLayout(distance);
    auto *buttons = new QHBoxLayout;
    applyButton = new MenuButton("Aplicar", "Enter");
    againButton = new MenuButton("Medir de novo");
    auto *cancel = new MenuButton("Cancelar", "Esc");
    applyButton->setCheckable(true);   // shown lit while a result waits to be applied
    buttons->addWidget(applyButton);
    buttons->addWidget(againButton);
    buttons->addWidget(cancel);
    v->addLayout(buttons);
    connect(detectButton, &QAbstractButton::clicked, this, &Calibrator::autoDetect);
    connect(applyButton, &QAbstractButton::clicked, this, &Calibrator::apply);
    connect(againButton, &QAbstractButton::clicked, this, &Calibrator::measureAgain);
    connect(cancel, &QAbstractButton::clicked, this, [this] { canvas->setTool(Canvas::Select); });
    connect(unit, &QLineEdit::editingFinished, canvas, [this] { canvas->setFocus(); });
    connect(perCell, &QDoubleSpinBox::editingFinished, canvas, [this] { canvas->setFocus(); });
    instruction->setStyleSheet(QString("color: %1;").arg(ui::text.name()));
    progress->setStyleSheet(QString("color: %1;").arg(ui::muted.name()));
}

void Calibrator::setText(const QString &s) {
    instruction->setText(s);
    adjustSize();   // the panel floats over the canvas, outside any layout
}

void Calibrator::start(MapItem *selected) {
    stop();
    popUp(QPoint(6, 6), QPoint(-14, 0));
    title->setText("Calibrar grade");
    progress->clear();
    applyButton->setEnabled(false);
    againButton->setEnabled(false);
    detectButton->setEnabled(false);
    perCell->setValue(1.5);
    unit->setText("m");
    if (selected) choose(selected);
    else setText("Clique no mapa que você quer calibrar.");
}

void Calibrator::stop() {
    clearMarks();
    if (map) { map->preview.reset(); map->update(); }
    map = nullptr;
    measurements.clear();
    pointA.reset();
    result.reset();
    waitingFit = waitingDetect = false;
    applyButton->setChecked(false);
    dismiss();
}

void Calibrator::choose(MapItem *m) {
    map = m;
    title->setText("Calibrar grade: " + QFileInfo(m->src->path).fileName());
    if (m->cal.valid()) {
        perCell->setValue(m->cal.unitsPerCell);
        unit->setText(m->cal.unit);
    }
    detectButton->setEnabled(true);
    againButton->setEnabled(true);
    requestProfiles();
    measureAgain();
}

void Calibrator::reset() {
    clearMarks();
    measurements.clear();
    pointA.reset();
    result.reset();
    waitingFit = waitingDetect = false;
    map->preview.reset();
    map->update();
    applyButton->setEnabled(false);
    applyButton->setChecked(false);
}

void Calibrator::measureAgain() {
    if (!map) return;
    reset();
    showStep();
}

QRectF Calibrator::quarter(int q) const {
    const QRectF r = map->boundingRect();
    return QRectF(r.left() + (q % 2) * r.width() / 2, r.top() + (q / 2) * r.height() / 2, r.width() / 2, r.height() / 2);
}

// Atlas: one quarter of the map at a time, the others dimmed, the view zoomed onto it.
void Calibrator::showStep() {
    clearMarks();
    for (int i = 0; i < measurements.size(); ++i) {
        auto *line = new QGraphicsLineItem(QLineF(measurements[i].a, measurements[i].b), map);
        line->setPen([] { QPen p(QColor(255, 210, 60), 2); p.setCosmetic(true); return p; }());
        mark(line);
    }
    const int q = int(measurements.size());
    QPainterPath dim;
    dim.setFillRule(Qt::OddEvenFill);
    dim.addRect(map->boundingRect());
    dim.addRect(quarter(q));
    auto *shade = new QGraphicsPathItem(dim, map);
    shade->setBrush(QColor(0, 0, 0, 140));
    shade->setPen(Qt::NoPen);
    mark(shade);
    const QRectF view = map->mapRectToScene(quarter(q));
    canvas->fitInView(view.adjusted(-view.width() * 0.05, -view.height() * 0.05, view.width() * 0.05, view.height() * 0.05), Qt::KeepAspectRatio);
    emit canvas->zoomChanged(canvas->zoom());
    progress->setText(QString("Medição %1 de 4").arg(q + 1));
    setText(measurements.isEmpty()
                ? "Clique num canto de um quadrado (cruzamento das linhas da grade) no quarto destacado. Dê zoom para clicar com precisão."
                : "As linhas do mapa não confirmaram a grade. Meça mais um quadrado no quarto destacado.");
}

QGraphicsItem *Calibrator::mark(QGraphicsItem *item) {
    item->setAcceptedMouseButtons(Qt::NoButton);
    item->setZValue(10);
    marks << item;
    return item;
}

void Calibrator::clearMarks() {
    if (map) qDeleteAll(marks);   // without the map they were deleted with it
    marks.clear();
}

void Calibrator::mousePress(QPointF sp) {
    if (!map) {
        if (MapItem *m = mapAt(canvas->scene(), sp, false)) choose(m);
        return;
    }
    if (result || busy()) return;
    QPointF p = map->mapFromScene(sp);
    if (!map->boundingRect().contains(p)) return;
    if (!pointA) {
        const QRectF q = quarter(int(measurements.size()));
        if (!q.adjusted(-q.width() * 0.1, -q.height() * 0.1, q.width() * 0.1, q.height() * 0.1).contains(p)) {
            setText("Clique dentro do quarto destacado do mapa.");
            return;
        }
        pointA = p;
        QPainterPath cross;
        cross.moveTo(-9, 0); cross.lineTo(9, 0); cross.moveTo(0, -9); cross.lineTo(0, 9);
        auto *c = new QGraphicsPathItem(cross, map);
        c->setPen(QPen(QColor(255, 210, 60), 2));
        c->setFlag(QGraphicsItem::ItemIgnoresTransformations);
        c->setPos(p);
        mark(c);
        setText("Agora o canto seguinte à direita, no mesmo quadrado.");
        return;
    }
    p.setY(pointA->y());   // Atlas: the second corner is on the first one's row
    if (std::abs(p.x() - pointA->x()) < 2) return;
    measurements << Measurement{*pointA, p};
    pointA.reset();
    measured();
}

void Calibrator::measured() {
    const GridFit m = gridFromMeasurements(measurements);
    map->preview = Calibration{m.cell, m.origin};
    map->update();
    tryFit();
}

void Calibrator::tryFit() {
    if (!map || measurements.isEmpty()) return;
    const auto prof = profileCache.value(map->src->path);
    if (!prof) {
        waitingFit = true;
        setText("Lendo as linhas do mapa…");
        return;
    }
    waitingFit = false;
    const GridFit measuredGrid = gridFromMeasurements(measurements);
    const std::optional<GridFit> fit = fitGrid(*prof, measuredGrid.cell);
    // The fitted lines must run through the corners that were clicked, or they are another pattern of the map.
    bool onClicks = fit.has_value();
    for (const Measurement &m : measurements)
        for (QPointF c : {m.a, m.b})
            onClicks = onClicks && distanceToLine(c.x(), fit->origin.x(), fit->cell) <= CLICK_TOLERANCE * fit->cell
                       && distanceToLine(c.y(), fit->origin.y(), fit->cell) <= CLICK_TOLERANCE * fit->cell;
    if (onClicks) setResult(*fit, "ajustada às linhas do mapa");
    else if (measurements.size() >= 4) setResult(measuredGrid, "pela média das 4 medições (as linhas do mapa não a confirmaram)");
    else showStep();
}

void Calibrator::autoDetect() {
    if (!map || waitingDetect) return;
    const auto prof = profileCache.value(map->src->path);
    reset();
    progress->clear();
    waitingDetect = true;
    setText("Procurando a grade nas linhas do mapa…");
    if (!prof) return;   // profilesReady() calls back
    QPointer<Calibrator> self(this);
    const QString path = map->src->path;
    QThreadPool::globalInstance()->start([self, prof, path] {
        const std::optional<GridFit> g = detectGrid(*prof);
        QMetaObject::invokeMethod(qApp, [self, g, path] {
            if (!self || !self->map || self->map->src->path != path || !self->waitingDetect) return;
            self->waitingDetect = false;
            if (g) self->setResult(*g, "encontrada automaticamente");
            else { self->showStep(); self->setText("Nenhuma grade encontrada nas linhas do mapa. Meça um quadrado: clique num canto no quarto destacado."); }
        }, Qt::QueuedConnection);
    });
}

void Calibrator::setResult(const GridFit &g, const QString &how) {
    result = g;
    clearMarks();
    map->preview = Calibration{g.cell, g.origin};
    map->update();
    const QRectF r = map->sceneBoundingRect();
    canvas->fitInView(r.adjusted(-r.width() * 0.05, -r.height() * 0.05, r.width() * 0.05, r.height() * 0.05), Qt::KeepAspectRatio);
    emit canvas->zoomChanged(canvas->zoom());
    progress->clear();
    applyButton->setEnabled(true);
    applyButton->setChecked(true);
    setText(QString("Grade %1: quadrado de %2 px.\nConfira a grade amarela (dê zoom). Setas movem a grade, Shift + setas em 0,1 px. Enter aplica.")
                .arg(how, QLocale(QLocale::Portuguese, QLocale::Brazil).toString(g.cell, 'f', 2)));
}

void Calibrator::apply() {
    if (!map || !result) return;
    const bool grid = map->cal.showGrid;   // the grid stays as the user had it: hidden unless they turned it on
    map->cal = Calibration{result->cell, result->origin, perCell->value(), unit->text().trimmed().isEmpty() ? "m" : unit->text().trimmed(), grid};
    map->preview.reset();
    map->update();
    result.reset();
    sceneEdited();
    emit canvas->scene()->selectionChanged();   // refreshes the Properties panel showing this map
    const QString msg = QString("Mapa calibrado: 1 quadrado = %1 px = %2 %3.")
                            .arg(QLocale(QLocale::Portuguese, QLocale::Brazil).toString(map->cal.cellPx, 'f', 2))
                            .arg(map->cal.unitsPerCell).arg(map->cal.unit);
    canvas->setTool(Canvas::Select);
    emit canvas->status(msg);
}

bool Calibrator::key(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Escape: canvas->setTool(Canvas::Select); return true;
    case Qt::Key_Return: case Qt::Key_Enter: apply(); return true;
    case Qt::Key_F: autoDetect(); return true;
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down: {
        if (!result || !map) return true;
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 0.1 : 1;
        const int k = e->key();
        result->origin += QPointF(k == Qt::Key_Left ? -step : k == Qt::Key_Right ? step : 0,
                                  k == Qt::Key_Up ? -step : k == Qt::Key_Down ? step : 0);
        map->preview = Calibration{result->cell, result->origin};
        map->update();
        return true;
    }
    default: return false;
    }
}

void Calibrator::requestProfiles() {
    const QString path = map->src->path;
    if (profileCache.contains(path) || profilesLoading.contains(path)) return;
    profilesLoading.insert(path);
    QPointer<Calibrator> self(this);
    QThreadPool::globalInstance()->start([self, path] {
        const auto p = QSharedPointer<const LineProfiles>::create(loadLineProfiles(path));
        QMetaObject::invokeMethod(qApp, [self, path, p] {
            profilesLoading.remove(path);
            profileCache.insert(path, p);
            if (self) self->profilesReady(path);
        }, Qt::QueuedConnection);
    });
}

void Calibrator::profilesReady(const QString &path) {
    if (!map || map->src->path != path) return;
    if (waitingFit) tryFit();
    if (waitingDetect) { waitingDetect = false; autoDetect(); }
}
