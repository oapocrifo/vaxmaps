#include "calibration.h"
#include "canvas.h"
#include "items.h"
#include "lang.h"
#include "mapitem.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QHash>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPen>
#include <QSet>
#include <QThreadPool>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

namespace {

constexpr int ANALYSIS_SIDE = 4096;          // Atlas MAX_ANALYSIS_SIDE
constexpr double HINT_RANGE = 0.13;          // Atlas HINT_SIZE_RANGE
constexpr double MIN_HINTED = 0.65;          // a measured cell needs less support than a detected one
constexpr double MIN_DETECTED = 1.3;    // lines are measured along their direction only (see analyse): noise scores near 1
constexpr double DENSER_PREFERENCE = 0.75;   // Atlas DENSER_GRID_PREFERENCE
constexpr double CLICK_TOLERANCE = 0.25;     // fitted grid must pass this close (in cells) to the clicked points
constexpr double MIN_LINED = 0.2;            // share of the periods along a profile that must show a line
constexpr double SQRT3 = 1.7320508075688772;

using Grid = Calibration::Grid;
const std::pair<int, int> neighbours4[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

// ---- folding: lines repeating at `period` pile up in one phase ----

struct Phase { double strength = 0, t = 0; };   // t: position of the lines along the normal, within [0, period)

// `profile`, when given, gets the folded line strength per bin (t·bins/period modulo bins), smoothed.
Phase fold(const float *sum, const float *count, int n, double t0, double period, std::vector<double> *profile = nullptr) {
    const int bins = std::max(8, int(period));
    if (n < 16 || period < 4 || period > n / 3.0) return {};
    std::vector<double> s(bins, 0), c(bins, 0);
    const double k = bins / period;
    double pos = std::fmod((t0 + 0.5) * k, double(bins));
    if (pos < 0) pos += bins;
    double total = 0, pixels = 0;
    for (int i = 0; i < n; ++i) {
        const int b = std::min(int(pos), bins - 1);
        s[b] += sum[i];
        c[b] += count[i];
        total += sum[i];
        pixels += count[i];
        pos += k;
        if (pos >= bins) pos -= bins;
    }
    if (pixels <= 0 || total <= 0) return {};
    const double mean = total / pixels;
    std::vector<double> v(bins), sm(bins);
    for (int b = 0; b < bins; ++b) v[b] = c[b] > 0 ? s[b] / c[b] : mean;
    for (int b = 0; b < bins; ++b) sm[b] = (v[(b + bins - 1) % bins] + 2 * v[b] + v[(b + 1) % bins]) / 4;
    if (profile) *profile = sm;
    std::vector<double> sorted = sm;
    std::nth_element(sorted.begin(), sorted.begin() + bins / 2, sorted.end());
    const double median = sorted[bins / 2];
    const int best = int(std::max_element(sm.begin(), sm.end()) - sm.begin());
    // Centre of the peak, so a thick line (two edges) gives its middle.
    const int w = std::max(3, bins / 8);   // at least ±3 px: both edges of a thick line
    double wsum = 0, dsum = 0;
    for (int d = -w; d <= w; ++d) {
        const double a = sm[(best + d + bins) % bins] - median;
        if (a > 0) { wsum += a; dsum += a * d; }
    }
    // A grid repeats: at least a fifth of the periods along the profile must have a line in the peak. One straight
    // edge (a room's wall, where the map's art turns black) piles up in a single period and scores at any spacing.
    const int periods = int(n * k / bins) + 3;
    const double cb = best + 0.5, first = std::floor(((t0 + 0.5) * k - cb) / bins);
    std::vector<double> ps(periods, 0), pc(periods, 0);
    for (int i = 0; i < n; ++i) {
        const double u = (t0 + i + 0.5) * k - cb, m = std::floor(u / bins + 0.5);
        const int j = int(m - first);
        if (std::abs(u - m * bins) <= w + 0.5 && j >= 0 && j < periods) { ps[j] += sum[i]; pc[j] += count[i]; }
    }
    double inPeak = 0, inPeakPixels = 0, fullest = 0;
    for (int j = 0; j < periods; ++j) { inPeak += ps[j]; inPeakPixels += pc[j]; fullest = std::max(fullest, pc[j]); }
    const double level = inPeakPixels > 0 ? median + 0.5 * (inPeak / inPeakPixels - median) : 0;
    int counted = 0, lined = 0;
    for (int j = 0; j < periods; ++j)
        if (pc[j] >= 0.5 * fullest) { ++counted; lined += ps[j] / pc[j] >= level; }
    if (lined < MIN_LINED * counted) return {};
    const double centre = (best + 0.5 + (wsum > 0 ? dsum / wsum : 0)) / k;
    return {(sm[best] - median) / mean, std::fmod(centre + period, period)};
}

Phase fold(const Analysis::Projection &p, double period) {
    return fold(p.sum.constData(), p.count.constData(), int(p.sum.size()), p.t0, period);
}

// Line families of each grid type: projection index and spacing of its lines (analysis px) for a cell of `cell` image px.
// Squares: horizontal and vertical lines one cell apart. Hexes: edges in three directions, half a cell apart.
struct Family { int k; double period; };

QVector<Family> families(const Analysis &a, Grid g, double cell) {
    const double avg = (a.sx + a.sy) / 2;
    auto scale = [&](int k) { return k == 0 ? a.sy : k == 3 ? a.sx : avg; };
    if (g == Calibration::Square) return {{0, cell * a.sy}, {3, cell * a.sx}};
    const int ks[3] = {g == Calibration::HexFlat ? 0 : 3, g == Calibration::HexFlat ? 2 : 1, g == Calibration::HexFlat ? 4 : 5};
    QVector<Family> f;
    for (int k : ks) f.push_back({k, cell / 2 * scale(k)});
    return f;
}

struct Score { double strength = 0, cell = 0; QVector<double> t; int regions = 0; };   // regions: bit r for a.proj[r]

// The six line directions summed over the regions in `mask` (bits 1-4: quarters). The quarters' bins share the
// whole map's grid of t (integer t0), so all four together give the whole map.
struct Lines { Analysis::Projection proj[6]; };

Lines linesOf(const Analysis &a, int mask) {
    Lines l;
    for (int k = 0; k < 6; ++k) {
        double t0 = 1e300, t1 = -1e300;
        for (int r = 1; r <= 4; ++r)
            if (mask & (1 << r)) {
                t0 = std::min(t0, a.proj[r][k].t0);
                t1 = std::max(t1, a.proj[r][k].t0 + a.proj[r][k].sum.size());
            }
        Analysis::Projection &m = l.proj[k];
        m.n = a.proj[0][k].n;
        if (t1 <= t0) continue;
        m.t0 = t0;
        m.sum = m.count = QVector<float>(int(t1 - t0), 0);
        for (int r = 1; r <= 4; ++r)
            if (mask & (1 << r)) {
                const Analysis::Projection &p = a.proj[r][k];
                const int off = int(p.t0 - t0);
                for (int i = 0; i < p.sum.size(); ++i) {
                    m.sum[off + i] += p.sum[i];
                    m.count[off + i] += p.count[i];
                }
            }
    }
    return l;
}

Score score(const Analysis &a, const Analysis::Projection *proj, Grid g, double cell) {
    Score s{1e9, cell, {}};
    for (const Family &f : families(a, g, cell)) {
        const Phase ph = fold(proj[f.k], f.period);
        s.strength = std::min(s.strength, ph.strength);
        s.t << ph.t;
    }
    return s;
}

Score search(const Analysis &a, const Analysis::Projection *proj, Grid g, double lo, double hi, double stepRel) {
    Score best;
    for (double c = lo; c <= hi; c *= 1 + stepRel) {
        const Score s = score(a, proj, g, c);
        if (s.strength > best.strength) best = s;
    }
    return best;
}

int profileLength(const Analysis::Projection *proj) {
    int n = 0;
    for (int k = 0; k < 6; ++k) n = std::max(n, int(proj[k].sum.size()));
    return n;
}

// On a dungeon map the room walls line up every few cells and outscore the faint squares drawn between them, so a
// multiple of the cell wins the search (Arden Vul, Level 6: 545 px for squares of 91). Returns the largest m (2 to 8)
// such that, in every family, a line shows at each m-th of s's spacing (at least SUB_LINE of s's own lines above the
// background) with background halfway between them; 1 when there is none.
constexpr double SUB_LINE = 0.25, SUB_GAP = 0.15;   // on 42 maps: lines in between 0.4-0.9, gaps below 0.1, none ~0

int subdivision(const Analysis &a, const Analysis::Projection *proj, Grid g, const Score &s, double lo) {
    const QVector<Family> fams = families(a, g, s.cell);
    std::vector<std::vector<double>> profiles(fams.size());
    for (int f = 0; f < fams.size(); ++f) {
        const Analysis::Projection &p = proj[fams[f].k];
        fold(p.sum.constData(), p.count.constData(), int(p.sum.size()), p.t0, fams[f].period, &profiles[f]);
        if (profiles[f].empty()) return 1;
    }
    for (int m = 8; m >= 2; --m) {
        bool finer = s.cell / m >= lo;
        for (int f = 0; f < fams.size() && finer; ++f) {
            const std::vector<double> &b = profiles[f];
            const int bins = int(b.size());
            const double step = double(bins) / m;   // in bins
            if (step < 4) { finer = false; break; }   // too few bins to tell a line from the gap beside it
            auto around = [&](double x, int r, bool highest) {   // the highest or lowest bin within r of bin x
                double v = highest ? -1e300 : 1e300;
                for (int d = -r; d <= r; ++d) {
                    const double w = b[((int(std::floor(x)) + d) % bins + bins) % bins];
                    v = highest ? std::max(v, w) : std::min(v, w);
                }
                return v;
            };
            // From the strongest bin, not s's phase: that is a centroid, which texture beside a wide period pulls.
            const int top = int(std::max_element(b.begin(), b.end()) - b.begin());
            std::vector<double> sorted = b;
            std::nth_element(sorted.begin(), sorted.begin() + bins / 2, sorted.end());
            const double median = sorted[bins / 2], line = b[top] - median;
            finer = line > 0;
            for (int j = 0; j < m && finer; ++j) {
                const double x = top + 0.5 + j * step;
                finer = (j == 0 || around(x, 2, true) - median >= SUB_LINE * line) && around(x + step / 2, 1, false) - median <= SUB_GAP * line;
            }
        }
        if (finer) return m;
    }
    return 1;
}

// s, or the finest grid whose lines include all of s's (see subdivision), refined. It keeps s's strength: its lines
// are s's lines and more.
Score finest(const Analysis &a, const Analysis::Projection *proj, Grid g, Score s, double lo) {
    for (int m; s.cell > 0 && (m = subdivision(a, proj, g, s, lo)) > 1;) {
        const double c = s.cell / m;
        Score f = search(a, proj, g, c * 0.98, c * 1.02, 0.25 / profileLength(proj));
        if (f.cell <= 0) {   // the folds at c miss MIN_LINED (few rooms show the squares): s's lines are on the grid
            f.cell = c;
            const QVector<Family> fams = families(a, g, c);
            f.t.clear();
            for (int i = 0; i < fams.size(); ++i) f.t << std::fmod(s.t[i], fams[i].period);
        }
        f.strength = std::max(f.strength, s.strength);
        f.regions = s.regions;
        s = f;
    }
    return s;
}

// Best grid of type g with a cell in [lo, hi] (image px) inside one region: coarse pass, then each peak refined;
// a multiple of the grid has all its lines on the map's lines too, so the densest well-supported peak wins.
Score detectIn(const Analysis &a, Grid g, double lo, double hi, int region) {
    const Analysis::Projection *proj = a.proj[region];
    const int n = profileLength(proj);
    std::vector<Score> coarse;
    for (double c = lo; c <= hi; c *= 1 + 3.0 / n) coarse.push_back(score(a, proj, g, c));
    double top = 0;
    for (const Score &s : coarse) top = std::max(top, s.strength);
    std::vector<Score> peaks;
    for (size_t i = 0; i < coarse.size(); ++i) {
        const double s = coarse[i].strength;
        if (s < 0.5 * top) continue;
        bool isPeak = true;
        for (size_t j = (i > 3 ? i - 3 : 0); j <= std::min(coarse.size() - 1, i + 3) && isPeak; ++j)
            isPeak = j == i || coarse[j].strength <= s;
        if (isPeak) peaks.push_back(search(a, proj, g, coarse[i].cell * (1 - 3.0 / n), coarse[i].cell * (1 + 3.0 / n), 0.25 / n));
    }
    double best = 0;
    for (const Score &s : peaks) best = std::max(best, s.strength);
    Score pick;
    for (const Score &s : peaks)
        if (s.strength >= DENSER_PREFERENCE * best && (pick.cell == 0 || s.cell < pick.cell)) pick = s;
    return finest(a, proj, g, pick, lo);
}

// One cell from the four quarters (quarters[i] is region i + 1). A map may show its grid in only some of them (a
// border, a title, rooms on black): the quarters whose lines stand out at least half as much as the best one are
// grouped by cell size (within 8 %: a grid that drifts stays one group), and the group with the most strength wins.
// Detecting, a quarter needs only a quarter of the best one (and half the minimum): a quarter almost all black has
// little line strength on average, so its few lines score twice the others' and left them out (rooms on every 4th
// line of 70 px squares: 3 quarters at 70 px dropped). Measured, every quarter is searched near the same size, so
// agreeing on it proves nothing and the stricter half stays.
// A grid seen in one quarter only needs more evidence: twice the strength and, when detected, at least 8 lines
// across the quarter (a few walls of a room can line up for 3 or 4); in two quarters, at least 6 lines across
// each. The group's lines together, without the
// quarters that show no grid, then refine the cell when they agree (a regular grid: more lines, more precision).
// A grid that drifts keeps the median, the local corrections do the rest.
Score combine(const Analysis &a, Grid g, const std::vector<Score> &quarters, bool detecting) {
    double top = 0;
    for (const Score &q : quarters) top = std::max(top, q.strength);
    const double least = detecting ? std::max(0.25 * top, 0.5 * MIN_DETECTED) : 0.5 * top;
    std::vector<int> kept;
    for (int i = 0; i < int(quarters.size()); ++i)
        if (quarters[i].cell > 0 && quarters[i].strength > 0 && quarters[i].strength >= least) kept.push_back(i);
    std::vector<std::pair<double, std::vector<int>>> groups;   // strength, members
    for (int i : kept) {
        std::vector<int> members;
        double sum = 0;
        for (int j : kept)
            if (std::abs(quarters[j].cell / quarters[i].cell - 1) <= 0.08) { members.push_back(j); sum += quarters[j].strength; }
        groups.push_back({sum, members});
    }
    // The strongest group that has the evidence its size asks for: a legend or a title in two quarters (City Ruins:
    // the rows of its table, 705 px) may outscore the squares of the other two and still not count as a grid.
    std::stable_sort(groups.begin(), groups.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
    const double side = std::min(a.gray.width(), a.gray.height()) / 2.0 / ((a.sx + a.sy) / 2);   // a quarter's, image px
    for (const auto &[sum, group] : groups) {
        std::vector<double> cells, strengths;
        int mask = 0;
        for (int i : group) {
            cells.push_back(quarters[i].cell);
            strengths.push_back(quarters[i].strength);
            mask |= 1 << (i + 1);
        }
        const size_t mid = (cells.size() - 1) / 2;   // the lower median strength: in a group of two, both must show the grid
        std::nth_element(cells.begin(), cells.begin() + cells.size() / 2, cells.end());
        std::nth_element(strengths.begin(), strengths.begin() + mid, strengths.end());
        const double cell = cells[cells.size() / 2], strength = strengths[mid];
        if (group.size() == 1 && (detecting ? strength < 2 * MIN_DETECTED || cell > side / 8 : strength < 2 * MIN_HINTED)) continue;
        if (group.size() == 2 && detecting && cell > side / 6) continue;
        const Lines lines = linesOf(a, mask);
        Score refined = search(a, lines.proj, g, cell * 0.98, cell * 1.02, 0.25 / profileLength(lines.proj));
        if (refined.strength < 0.6 * strength) {
            refined = score(a, lines.proj, g, cell);
            refined.strength = strength;
        }
        refined.regions = mask;
        return refined;
    }
    return {};
}

double wrap(double v, double period) {   // into [-period/2, period/2)
    v = std::fmod(v + period / 2, period);
    if (v < 0) v += period;
    return v - period / 2;
}

bool solve2(QPointF n0, QPointF n1, double r0, double r1, QPointF *out) {
    const double det = n0.x() * n1.y() - n0.y() * n1.x();
    if (std::abs(det) < 1e-9) return false;
    *out = QPointF((r0 * n1.y() - n0.y() * r1) / det, (n0.x() * r1 - r0 * n1.x()) / det);
    return true;
}

// Mean edge strength in a small square around p (analysis px).
double energyAt(const QImage &e, QPointF p, int r) {
    const int cx = int(p.x()), cy = int(p.y());
    double s = 0;
    int n = 0;
    for (int y = std::max(0, cy - r); y <= std::min(e.height() - 1, cy + r); ++y) {
        const uchar *l = e.constScanLine(y);
        for (int x = std::max(0, cx - r); x <= std::min(e.width() - 1, cx + r); ++x) { s += l[x]; ++n; }
    }
    return n ? s / n : 1e9;
}

// Grid from a scored cell: the line phases fix the position; for hexes the centres are the empty third of the
// points where the three edge families cross (the other two thirds are vertices).
Calibration lattice(const Analysis &a, Grid g, const Score &s) {
    const QVector<Family> fams = families(a, g, s.cell);
    QPointF o;
    solve2(a.proj[0][fams[0].k].n, a.proj[0][fams[1].k].n, s.t[0], s.t[1], &o);   // analysis px
    if (g == Calibration::Square) return Calibration::square(s.cell, QPointF(o.x() / a.sx, o.y() / a.sy));
    const double avg = (a.sx + a.sy) / 2;
    const Calibration unit = Calibration::hex(g, s.cell * avg, o);   // in analysis px
    const QPointF third = (unit.a + unit.b) / 3;
    const int r = std::max(1, int(s.cell * avg / 6));
    double best = 1e300;
    QPointF centre = o;
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
    for (QPointF q : {QPointF(0, 0), QPointF(a.gray.width(), 0), QPointF(0, a.gray.height()), QPointF(a.gray.width(), a.gray.height())}) {
        const QPointF uv = unit.toGrid(q);
        u0 = std::min(u0, uv.x()); u1 = std::max(u1, uv.x());
        v0 = std::min(v0, uv.y()); v1 = std::max(v1, uv.y());
    }
    const int stride = std::max(1, int(std::sqrt((u1 - u0) * (v1 - v0) / 400)));   // about 400 sample hexes
    for (int m = 0; m < 3; ++m) {
        const QPointF c = o + m * third;
        double e = 0;
        int n = 0;
        for (int i = int(u0) - 1; i <= int(u1) + 1; i += stride)
            for (int j = int(v0) - 1; j <= int(v1) + 1; j += stride) {
                const QPointF p = c + i * unit.a + j * unit.b;
                if (p.x() < r || p.y() < r || p.x() >= a.gray.width() - r || p.y() >= a.gray.height() - r) continue;
                e += energyAt(a.energy, p, r);
                ++n;
            }
        if (n && e / n < best) { best = e / n; centre = c; }
    }
    return Calibration::hex(g, s.cell, QPointF(centre.x() / a.sx, centre.y() / a.sy));
}

// ---- local corrections ----

// Measures, block by block, how far the map's lines sit from the regular grid and stores it in c.field.
// Returns the number of blocks measured from the lines (the others are filled from their neighbours).
int localField(const Analysis &a, Calibration &c, double globalStrength) {
    const int W = a.gray.width(), H = a.gray.height();
    const double avg = (a.sx + a.sy) / 2, cellA = c.cellSize() * avg;
    const QVector<Family> fams = families(a, c.grid, c.cellSize());
    const QPointF oA(c.origin.x() * a.sx, c.origin.y() * a.sy);
    const double B = std::max(48.0, 5 * cellA);
    const int cols = int(std::ceil(W / B)), rows = int(std::ceil(H / B));
    if (cols * rows < 4) return 0;
    const double minStrength = std::max(0.1, 0.3 * globalStrength);

    std::vector<QPointF> d(cols * rows);
    std::vector<double> weight(cols * rows, 0);
    for (int bj = 0; bj < rows; ++bj)
        for (int bi = 0; bi < cols; ++bi) {
            const int x0 = std::max(2, int(bi * B - B / 4)), x1 = std::min(W - 2, int((bi + 1) * B + B / 4));
            const int y0 = std::max(2, int(bj * B - B / 4)), y1 = std::min(H - 2, int((bj + 1) * B + B / 4));
            if (x1 - x0 < 16 || y1 - y0 < 16) continue;
            QVector<double> delta;
            double weakest = 1e9;
            for (const Family &f : fams) {
                const QPointF n = a.proj[0][f.k].n;
                double tmin = 1e300, tmax = -1e300;
                for (QPointF q : {QPointF(x0, y0), QPointF(x1, y0), QPointF(x0, y1), QPointF(x1, y1)}) {
                    tmin = std::min(tmin, QPointF::dotProduct(n, q));
                    tmax = std::max(tmax, QPointF::dotProduct(n, q));
                }
                const double t0 = std::floor(tmin);
                const int bins = int(std::ceil(tmax) - t0) + 2;
                std::vector<float> sum(bins, 0), count(bins, 0);
                const int ox = int(std::lround(2 * n.x())), oy = int(std::lround(2 * n.y()));
                for (int y = y0; y < y1; ++y) {
                    const uchar *l = a.gray.constScanLine(y), *u = a.gray.constScanLine(y - oy), *dn = a.gray.constScanLine(y + oy);
                    double t = n.x() * (x0 + 0.5) + n.y() * (y + 0.5) - t0;
                    for (int x = x0; x < x1; ++x, t += n.x()) {
                        const int b = std::min(bins - 1, std::max(0, int(t)));
                        sum[b] += std::abs(2 * l[x] - u[x - ox] - dn[x + ox]);
                        count[b] += 1;
                    }
                }
                // The region's own line spacing may differ from the map's average: take the best within 12 %.
                Phase ph;
                double period = f.period;
                for (double pl = f.period * 0.88; pl <= f.period * 1.12; pl *= 1 + 0.2 / bins) {
                    const Phase cand = fold(sum.data(), count.data(), bins, t0, pl);
                    if (cand.strength > ph.strength) { ph = cand; period = pl; }
                }
                weakest = std::min(weakest, ph.strength);
                // Offset where the region's lines are (its centre when they fill it; a room's middle when black or
                // art surrounds it, so a local period measured on a few lines is not stretched over empty space):
                // its nearest local line against the nearest regular line.
                double tw = 0, w = 0;
                for (int b = 0; b < bins; ++b) { tw += (t0 + b + 0.5) * sum[b]; w += sum[b]; }
                const double tc = w > 0 ? tw / w : QPointF::dotProduct(n, QPointF((x0 + x1) / 2.0, (y0 + y1) / 2.0));
                const double local = ph.t + std::round((tc - ph.t) / period) * period;
                delta << wrap(local - QPointF::dotProduct(n, oA), f.period);
            }
            if (weakest < minStrength) continue;
            QPointF shift;
            if (!solve2(a.proj[0][fams[0].k].n, a.proj[0][fams[1].k].n, delta[0], delta[1], &shift)) continue;
            if (fams.size() == 3 && std::abs(wrap(QPointF::dotProduct(a.proj[0][fams[2].k].n, shift) - delta[2], fams[2].period)) > fams[2].period / 4)
                continue;   // the third hex edge family disagrees
            d[bj * cols + bi] = shift;
            weight[bj * cols + bi] = weakest;
        }

    // A block's shift is known only up to whole line spacings: pick, from the strongest block outwards,
    // the value closest to the neighbours already placed.
    QPointF t1, t2;
    solve2(a.proj[0][fams[0].k].n, a.proj[0][fams[1].k].n, fams[0].period, 0, &t1);
    solve2(a.proj[0][fams[0].k].n, a.proj[0][fams[1].k].n, 0, fams[1].period, &t2);
    std::vector<int> state(cols * rows, 0);   // 0 unplaced, 1 placed
    int measured = 0;
    for (double w : weight) measured += w > 0;
    if (measured < std::max(3, cols * rows / 4)) return 0;
    // A regular grid, as most maps drawn on a computer have, needs no corrections. When 6 in 10 of the measured blocks
    // sit within a tenth of a cell of it, the others are a title, a compass, a side view or an inset drawn at another
    // scale, and corrections taken from them bent the grid around them by up to half a cell (Arden Vul, Levels 3 and
    // 10). A grid that drifts, or separate plans each on its own phase (a tower's floors: 5 in 10), keep them.
    int onGrid = 0;
    for (int i = 0; i < cols * rows; ++i) onGrid += weight[i] > 0 && QLineF(QPointF(), d[i]).length() <= 0.1 * cellA;
    if (onGrid >= 0.6 * measured) return 0;
    for (;;) {
        int start = -1;
        for (int i = 0; i < cols * rows; ++i)
            if (weight[i] > 0 && !state[i] && (start < 0 || weight[i] > weight[start])) start = i;
        if (start < 0) break;
        state[start] = 1;
        std::queue<int> q;
        q.push(start);
        while (!q.empty()) {
            const int cur = q.front();
            q.pop();
            const int ci = cur % cols, cj = cur / cols;
            for (auto [di, dj] : neighbours4) {
                const int ni = ci + di, nj = cj + dj;
                if (ni < 0 || nj < 0 || ni >= cols || nj >= rows) continue;
                const int nb = nj * cols + ni;
                if (weight[nb] <= 0 || state[nb]) continue;
                QPointF best = d[nb];
                double bestDist = 1e300;
                for (int i = -2; i <= 2; ++i)
                    for (int j = -2; j <= 2; ++j) {
                        const QPointF cand = d[nb] + i * t1 + j * t2;
                        const double dist = QLineF(cand, d[cur]).length();
                        if (dist < bestDist) { bestDist = dist; best = cand; }
                    }
                d[nb] = best;
                state[nb] = 1;
                q.push(nb);
            }
        }
    }
    // Outliers out (3x3 median of the measured blocks), then the unmeasured blocks filled from their neighbours.
    std::vector<QPointF> med = d;
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            if (weight[j * cols + i] <= 0) continue;
            std::vector<double> xs, ys;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    const int ni = i + di, nj = j + dj;
                    if (ni < 0 || nj < 0 || ni >= cols || nj >= rows || weight[nj * cols + ni] <= 0) continue;
                    xs.push_back(d[nj * cols + ni].x());
                    ys.push_back(d[nj * cols + ni].y());
                }
            std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end());
            std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
            med[j * cols + i] = QPointF(xs[xs.size() / 2], ys[ys.size() / 2]);
        }
    d = med;
    std::vector<bool> known(cols * rows);
    for (int i = 0; i < cols * rows; ++i) known[i] = weight[i] > 0;
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<bool> next = known;
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i < cols; ++i) {
                if (known[j * cols + i]) continue;
                QPointF s;
                int n = 0;
                for (auto [di, dj] : neighbours4) {
                    const int ni = i + di, nj = j + dj;
                    if (ni < 0 || nj < 0 || ni >= cols || nj >= rows || !known[nj * cols + ni]) continue;
                    s += d[nj * cols + ni];
                    ++n;
                }
                if (n) { d[j * cols + i] = s / n; next[j * cols + i] = true; changed = true; }
            }
        known = next;
    }
    c.fieldCols = cols;
    c.fieldRows = rows;
    c.fieldStep = QPointF(B / a.sx, B / a.sy);
    c.fieldOrigin = QPointF(B / 2 / a.sx, B / 2 / a.sy);
    c.field.resize(cols * rows);
    for (int i = 0; i < cols * rows; ++i) c.field[i] = QPointF(d[i].x() / a.sx, d[i].y() / a.sy);
    return measured;
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

bool nearLattice(const Calibration &c, QPointF p) {
    const QPointF uv = c.toGrid(p);
    return std::abs(uv.x() - std::round(uv.x())) <= CLICK_TOLERANCE && std::abs(uv.y() - std::round(uv.y())) <= CLICK_TOLERANCE;
}

// The lines of the last 3 maps read (about 24 MB each): enough to go back and forth between a few maps.
constexpr int CACHED_ANALYSES = 3;
QHash<QString, QSharedPointer<const Analysis>> analysisCache;
QStringList analysisOrder;   // least recently used first
QSet<QString> analysisLoading;

void keepAnalysis(const QString &path, QSharedPointer<const Analysis> a = {}) {
    if (a) analysisCache.insert(path, a);
    analysisOrder.removeAll(path);
    analysisOrder << path;
    while (analysisOrder.size() > CACHED_ANALYSES) analysisCache.remove(analysisOrder.takeFirst());
}

} // namespace

// ---- engine ----

Analysis analyse(const QImage &image, QSize full) {
    Analysis a;
    a.gray = image.convertToFormat(QImage::Format_Grayscale8);
    const int w = a.gray.width(), h = a.gray.height();
    if (w < 32 || h < 32 || full.isEmpty()) return a;
    a.sx = double(w) / full.width();
    a.sy = double(h) / full.height();
    const QRect regions[5] = {QRect(0, 0, w, h), QRect(0, 0, w / 2, h / 2), QRect(w / 2, 0, w - w / 2, h / 2),
                              QRect(0, h / 2, w / 2, h - h / 2), QRect(w / 2, h / 2, w - w / 2, h - h / 2)};
    for (int k = 0; k < 6; ++k) {
        const double ang = k * M_PI / 6;   // direction of the lines measured
        const QPointF n(-std::sin(ang), std::cos(ang));
        for (int r = 0; r < 5; ++r) {
            Analysis::Projection &p = a.proj[r][k];
            p.n = n;
            double tmin = 1e300, tmax = -1e300;
            const QRect &rc = regions[r];
            for (QPointF q : {QPointF(rc.left(), rc.top()), QPointF(rc.right() + 1, rc.top()), QPointF(rc.left(), rc.bottom() + 1),
                              QPointF(rc.right() + 1, rc.bottom() + 1)}) {
                tmin = std::min(tmin, QPointF::dotProduct(n, q));
                tmax = std::max(tmax, QPointF::dotProduct(n, q));
            }
            p.t0 = std::floor(tmin);
            const int bins = int(std::ceil(tmax) - p.t0) + 2;
            p.sum = QVector<float>(bins, 0);
            p.count = QVector<float>(bins, 0);
        }
        // |2·centre − both sides| across the line, minus twice the same along it: strong on thin lines in this
        // direction, dark or light, weak on flat areas and on lines in other directions. Without the second part the
        // slanted edges of pointy-top hexes made bands that looked like flat-top hexes √3 times larger.
        const int ox = int(std::lround(2 * n.x())), oy = int(std::lround(2 * n.y()));
        const int ax = int(std::lround(2 * std::cos(ang))), ay = int(std::lround(2 * std::sin(ang)));
        for (int y = 2; y < h - 2; ++y) {
            const uchar *l = a.gray.constScanLine(y), *u = a.gray.constScanLine(y - oy), *dn = a.gray.constScanLine(y + oy);
            const uchar *ua = a.gray.constScanLine(y - ay), *da = a.gray.constScanLine(y + ay);
            const double ty = n.y() * (y + 0.5);
            Analysis::Projection &whole = a.proj[0][k];
            Analysis::Projection &left = a.proj[y < h / 2 ? 1 : 3][k], &right = a.proj[y < h / 2 ? 2 : 4][k];
            for (int x = 2; x < w - 2; ++x) {
                const double t = n.x() * (x + 0.5) + ty;
                const float v = float(std::max(0, std::abs(2 * l[x] - u[x - ox] - dn[x + ox]) - 2 * std::abs(2 * l[x] - ua[x - ax] - da[x + ax])));
                const int b = std::min(int(whole.sum.size()) - 1, std::max(0, int(t - whole.t0)));
                whole.sum[b] += v;
                whole.count[b] += 1;
                Analysis::Projection &q = x < w / 2 ? left : right;
                const int bq = std::min(int(q.sum.size()) - 1, std::max(0, int(t - q.t0)));
                q.sum[bq] += v;
                q.count[bq] += 1;
            }
        }
    }
    a.energy = QImage(w, h, QImage::Format_Grayscale8);
    a.energy.fill(0);
    for (int y = 1; y < h - 1; ++y) {
        const uchar *u = a.gray.constScanLine(y - 1), *l = a.gray.constScanLine(y), *d = a.gray.constScanLine(y + 1);
        uchar *e = a.energy.scanLine(y);
        for (int x = 1; x < w - 1; ++x) e[x] = uchar(std::min(255, std::abs(l[x + 1] - l[x - 1]) + std::abs(d[x] - u[x])));
    }
    return a;
}

Analysis loadAnalysis(const QString &path) {
    QImageReader reader(path);
    const QSize full = reader.size();
    if (full.isValid() && std::max(full.width(), full.height()) > ANALYSIS_SIDE)
        reader.setScaledSize(full.scaled(ANALYSIS_SIDE, ANALYSIS_SIDE, Qt::KeepAspectRatio));
    const QImage img = reader.read();
    return img.isNull() ? Analysis{} : analyse(img, full.isValid() ? full : img.size());
}

std::optional<GridFit> fitGrid(const Analysis &a, Grid type, double hint) {
    if (!a.valid() || !(hint > 0)) return {};
    std::vector<Score> quarters;
    for (int r = 1; r <= 4; ++r)
        quarters.push_back(search(a, a.proj[r], type, hint * (1 - HINT_RANGE), hint * (1 + HINT_RANGE), 0.25 / profileLength(a.proj[r])));
    const Score s = combine(a, type, quarters, false);
    if (s.strength < MIN_HINTED || s.t.size() < 2) return {};
    GridFit g{lattice(a, type, s), s.strength, 0, s.regions >> 1};
    g.regions = localField(a, g.cal, s.strength);
    return g;
}

std::optional<GridFit> detectGrid(const Analysis &a) {
    if (!a.valid()) return {};
    const double avg = (a.sx + a.sy) / 2;
    const double side = std::min(a.gray.width(), a.gray.height()) / 2.0 / avg;   // a quarter's shorter side, image px
    struct Candidate { Grid g; Score s; bool alias = false; };
    std::vector<Candidate> found;
    for (Grid g : {Calibration::Square, Calibration::HexPointy, Calibration::HexFlat}) {
        const bool hex = g != Calibration::Square;
        const double lo = (hex ? 14 : 8) / avg, hi = side / 3.5;
        if (hi <= lo) continue;
        std::vector<Score> quarters;
        for (int r = 1; r <= 4; ++r) quarters.push_back(detectIn(a, g, lo, hi, r));
        Score s = combine(a, g, quarters, true);
        if (s.regions) s = finest(a, linesOf(a, s.regions).proj, g, s, lo);   // quarters that all found the same multiple
        // Faint squares among strong cave walls score low (Thicket Caverns: 1.25), but 3 or 4 quarters that find the
        // same cell on their own, within 1 %, do not happen by chance.
        int agree = 0;
        for (const Score &q : quarters) agree += s.cell > 0 && q.cell > 0 && std::abs(q.cell / s.cell - 1) <= 0.01;
        if (s.strength >= (agree >= 3 ? 0.75 : 1.0) * MIN_DETECTED && s.t.size() >= 2) found.push_back({g, s});
    }
    // The edges of hexes w wide also line up as hexes of the other orientation √3·w wide: try the smaller one.
    for (size_t i = 0, n = found.size(); i < n; ++i)
        if (found[i].g != Calibration::Square) {
            const Grid other = found[i].g == Calibration::HexPointy ? Calibration::HexFlat : Calibration::HexPointy;
            const double c = found[i].s.cell / SQRT3;
            const Lines lines = linesOf(a, found[i].s.regions);   // where the parent's grid is
            Score s = search(a, lines.proj, other, c * 0.97, c * 1.03, 0.25 / profileLength(lines.proj));
            s.regions = found[i].s.regions;
            if (s.t.size() >= 2) found.push_back({other, s, true});   // judged against its parent below
        }
    // Bigger cells score higher (a line takes a smaller share of a longer period), so the densest grid whose lines
    // stand out at least half as much as the best one wins (a third for the √3 smaller hexes: thick edges weaken
    // them more). Hex edges are half a cell apart, square lines one cell.
    double best = 0;
    for (const Candidate &c : found) best = std::max(best, c.s.strength);
    const Candidate *pick = nullptr;
    auto spacing = [](const Candidate &c) { return c.g == Calibration::Square ? c.s.cell : c.s.cell / 2; };
    for (const Candidate &c : found)
        if (c.s.strength >= (c.alias ? 0.35 : 0.5) * best && (!pick || spacing(c) < spacing(*pick))) pick = &c;
    // Faint lines need many of them: walls of a few rooms can repeat for 8 or 10 lines across a map, never for 14.
    if (!pick || (pick->s.strength < 2 * MIN_DETECTED && spacing(*pick) > 2 * side / 14)) return {};
    GridFit chosen{lattice(a, pick->g, pick->s), pick->s.strength, 0, pick->s.regions >> 1};
    chosen.regions = localField(a, chosen.cal, chosen.strength);
    return chosen;
}

Calibration gridFromMeasurements(const QVector<Measurement> &m, bool hex) {
    if (m.isEmpty()) return {};
    if (!hex) {
        double cell = 0;
        for (const Measurement &p : m) cell += std::max(std::abs(p.b.x() - p.a.x()), std::abs(p.b.y() - p.a.y()));
        cell /= m.size();
        QVector<double> xs, ys;
        for (const Measurement &p : m) xs << p.a.x() << p.b.x(), ys << p.a.y() << p.b.y();
        return Calibration::square(cell, QPointF(circularMean(xs, cell), circularMean(ys, cell)));
    }
    double w = 0, tilt = 0;
    for (const Measurement &p : m) {
        w += QLineF(p.a, p.b).length();
        const double deg = std::fmod(std::abs(std::atan2(p.b.y() - p.a.y(), p.b.x() - p.a.x())) * 180 / M_PI, 60.0);
        tilt += std::min(deg, 60 - deg);   // 0 for pointy-top neighbours, 30 for flat-top
    }
    w /= m.size();
    Calibration c = Calibration::hex(tilt / m.size() < 15 ? Calibration::HexPointy : Calibration::HexFlat, w, m[0].a);
    QVector<double> us, vs;
    for (const Measurement &p : m)
        for (QPointF q : {p.a, p.b}) {
            const QPointF uv = c.toGrid(q);
            us << uv.x();
            vs << uv.y();
        }
    c.origin += circularMean(us, 1) * c.a + circularMean(vs, 1) * c.b;
    return c;
}

int cachedAnalyses() { return int(analysisCache.size()); }

void detectGrids(const QList<MapItem *> &maps, std::function<void(int, int)> done) {
    QList<QPointer<MapItem>> todo;
    QStringList paths;
    for (MapItem *m : maps)
        if (m && !m->cal.valid()) { todo << m; paths << m->src->path; }
    if (todo.isEmpty()) return done(0, 0);
    QThreadPool::globalInstance()->start([todo, paths, done] {
        auto found = QSharedPointer<int>::create(0);
        for (int i = 0; i < paths.size() && !quitting; ++i) {   // closing the program does not wait for the rest
            const std::optional<GridFit> g = detectGrid(loadAnalysis(paths[i]));
            QMetaObject::invokeMethod(qApp, [m = todo[i], g, found] {
                if (!g || !m || m->cal.valid()) return;   // deleted, or calibrated by hand meanwhile
                m->cal = g->cal;
                m->update();
                ++*found;
                sceneEdited();
            }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(qApp, [found, total = int(paths.size()), done] { done(*found, total); }, Qt::QueuedConnection);
    });
}

// ---- panel ----

Calibrator::Calibrator(Canvas *c) : Panel(c, false), canvas(c) {
    setFixedWidth(400);
    QVBoxLayout *v = body;
    title = new QLabel;
    QFont f = title->font();
    f.setBold(true);
    f.setPixelSize(14);
    title->setFont(f);
    v->addWidget(title);
    auto *types = new QHBoxLayout;
    types->setSpacing(3);
    squaresButton = new MenuButton("Squares");
    hexButton = new MenuButton("Hexes");
    for (MenuButton *b : {squaresButton, hexButton}) {
        b->setCheckable(true);
        types->addWidget(b);
    }
    detectButton = new MenuButton("Detect", "F");
    detectButton->setTip("Finds squares or hexes in the map's lines, without clicks");
    types->addWidget(detectButton);
    v->addLayout(types);
    instruction = new QLabel;
    instruction->setWordWrap(true);
    instruction->setMinimumHeight(58);
    instruction->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    v->addWidget(instruction);
    auto *steps = new QHBoxLayout;
    progress = new QLabel;
    steps->addWidget(progress, 1);
    skipButton = new MenuButton("Skip quarter", "S");
    skipButton->setTip("This quarter has no squares or hexes: go to the next one");
    steps->addWidget(skipButton);
    v->addLayout(steps);
    auto *form = new QGridLayout;
    form->setHorizontalSpacing(6);
    form->setVerticalSpacing(4);
    form->addWidget(new Label("Cell size"), 0, 0);
    cellSpin = new QDoubleSpinBox;
    cellSpin->setRange(2, 100000);
    cellSpin->setDecimals(2);
    cellSpin->setValue(70);
    cellSpin->setSuffix(" px");
    cellSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    cellSpin->setToolTip(T("Type the cell size instead of measuring it"));
    form->addWidget(cellSpin, 0, 1);
    auto *use = new MenuButton("Use");
    form->addWidget(use, 0, 2);
    connect(use, &QAbstractButton::clicked, this, [this] { useCell(cellSpin->value()); });
    topRow = new QWidget;   // hexes typed in: which way they point
    auto *tops = new QHBoxLayout(topRow);
    tops->setContentsMargins(0, 0, 0, 0);
    tops->setSpacing(3);
    tops->addWidget(new Label("Top"));
    pointyButton = new MenuButton("Pointy");
    flatButton = new MenuButton("Flat");
    auto *topGroup = new QButtonGroup(this);
    for (MenuButton *b : {pointyButton, flatButton}) {
        b->setCheckable(true);
        topGroup->addButton(b);
        tops->addWidget(b);
    }
    pointyButton->setChecked(true);
    form->addWidget(topRow, 1, 0, 1, 3);
    unitsLabel = new QLabel;
    form->addWidget(unitsLabel, 2, 0);
    perCell = new QDoubleSpinBox;
    perCell->setRange(0.001, 1e6);
    perCell->setDecimals(2);
    perCell->setButtonSymbols(QAbstractSpinBox::NoButtons);
    form->addWidget(perCell, 2, 1);
    unit = new QLineEdit;
    unit->setMaximumWidth(70);
    form->addWidget(unit, 2, 2);
    form->setColumnStretch(1, 1);
    v->addLayout(form);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(3);
    applyButton = new MenuButton("Apply", "Enter");
    againButton = new MenuButton("Start over");
    auto *cancel = new MenuButton("Cancel", "Esc");
    applyButton->setCheckable(true);   // lit while a result waits to be applied
    buttons->addWidget(applyButton);
    buttons->addWidget(againButton);
    buttons->addWidget(cancel);
    v->addLayout(buttons);
    connect(squaresButton, &QAbstractButton::clicked, this, [this] { setHex(false); });
    connect(hexButton, &QAbstractButton::clicked, this, [this] { setHex(true); });
    connect(detectButton, &QAbstractButton::clicked, this, &Calibrator::autoDetect);
    connect(skipButton, &QAbstractButton::clicked, this, &Calibrator::skip);
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
    title->setText(T("Calibrate grid"));
    progress->clear();
    for (MenuButton *b : {applyButton, againButton, skipButton}) b->setEnabled(false);   // Detect says to pick a map first
    applyButton->setChecked(false);
    perCell->setValue(1.5);
    unit->setText("m");
    setHex(false);
    if (selected) choose(selected);
    else setText(T("Click the map you want to calibrate."));
}

void Calibrator::stop() {
    clearMarks();
    if (map) { map->preview.reset(); map->update(); }
    map = nullptr;
    measurements.clear();
    pointA.reset();
    result.reset();
    matched = waitingAnalysis = detecting = fitting = false;
    ++fitGeneration;
    applyButton->setChecked(false);
    dismiss();
}

void Calibrator::choose(MapItem *m) {
    map = m;
    title->setText(T("Calibrate grid") + ": " + QFileInfo(m->src->path).fileName());
    if (m->cal.valid()) {
        perCell->setValue(m->cal.unitsPerCell);
        unit->setText(m->cal.unit);
    }
    againButton->setEnabled(true);
    requestAnalysis();
    setHex(m->cal.valid() && m->cal.isHex());
}

void Calibrator::setHex(bool h) {
    hex = h;
    squaresButton->setChecked(!h);
    hexButton->setChecked(h);
    topRow->setVisible(h);
    unitsLabel->setText(T(h ? "1 hex =" : "1 square ="));
    if (map) measureAgain();
}

void Calibrator::reset() {
    clearMarks();
    measurements.clear();
    stepAt = skipped = lineQuarters = 0;
    pointA.reset();
    result.reset();
    matched = detecting = fitting = false;
    ++fitGeneration;
    if (map) { map->preview.reset(); map->update(); }
    applyButton->setEnabled(false);
    applyButton->setChecked(false);
}

void Calibrator::measureAgain() {
    if (!map) return;
    reset();
    showStep();
}

void Calibrator::skip() {
    if (!map || stepAt >= 4 || detecting || fitting) return;
    skipped |= 1 << stepAt;
    ++stepAt;
    pointA.reset();
    if (stepAt >= 4 && !result && !measurements.isEmpty())
        return setResult(gridFromMeasurements(measurements, hex), false, T("from the mean of your measurements (the map's lines did not confirm it)"));
    showStep();
}

QRectF Calibrator::quarter(int q) const {
    const QRectF r = map->boundingRect();
    return QRectF(r.left() + (q % 2) * r.width() / 2, r.top() + (q / 2) * r.height() / 2, r.width() / 2, r.height() / 2);
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

// Atlas: one quarter of the map per measurement. Before the grid is known the other quarters are dimmed and the
// view zooms onto it; once a grid is shown, the next quarter is only outlined so the whole preview stays visible.
void Calibrator::showStep() {
    if (!map) return;
    clearMarks();
    // Once the lines confirm a grid, the quarters where they do not show it are skipped: there is nothing to measure.
    while (matched && lineQuarters && stepAt < 4 && !(lineQuarters & (1 << stepAt))) {
        skipped |= 1 << stepAt;
        ++stepAt;
    }
    QPen yellow(QColor(255, 210, 60), 2);
    yellow.setCosmetic(true);
    for (const Measurement &m : measurements) {
        auto *line = new QGraphicsLineItem(QLineF(m.a, m.b), map);
        line->setPen(yellow);
        mark(line);
    }
    const int q = stepAt;
    const QString what = T(hex ? "the centre of a hex" : "a corner of a square");
    const QString next = T(hex ? "then the centre of the hex next to it on the right" : "then the next corner to its right");
    if (q < 4) {
        if (!result) {
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
        } else {
            auto *outline = new QGraphicsRectItem(quarter(q), map);
            QPen dash(QColor(255, 210, 60, 200), 2, Qt::DashLine);
            dash.setCosmetic(true);
            outline->setPen(dash);
            mark(outline);
        }
    }
    QString dots;   // measured, skipped, still to do
    for (int i = 0; i < 4; ++i) dots += i >= q ? "○ " : skipped & (1 << i) ? "– " : "● ";
    progress->setText(T("Step %1 of 4   %2").arg(std::min(q + 1, 4)).arg(dots.trimmed()));
    skipButton->setEnabled(q < 4);
    if (pointA) return;
    if (!result && q >= 4) {
        setText(T("No quarter was measured. Detect the grid (F), type the cell size, or Start over."));
    } else if (!result) {
        setText(measured() == 0 ? T("Click %1 in the highlighted quarter, %2. Zoom in for precision. No squares or hexes there? Skip it (S).").arg(what, next)
                                : T("The map's lines did not confirm the grid yet. Measure in the highlighted quarter: %1, %2. No grid there? Skip it (S).").arg(what, next));
    } else if (q < 4) {
        setText(T("Grid %1: cell of %2 px.\nCheck the yellow grid. Enter applies it. To refine, measure %3 in the outlined quarter, %4. "
                  "Arrows move the grid (Shift: 0.1 px).")
                    .arg(how).arg(result->cellSize(), 0, 'f', 2).arg(what, next));
    } else {
        setText(T("Grid %1: cell of %2 px.\nAll 4 steps done. Enter applies it. Arrows move the grid (Shift: 0.1 px).")
                    .arg(how).arg(result->cellSize(), 0, 'f', 2));
    }
}

void Calibrator::mousePress(QPointF sp) {
    if (!map) {
        if (MapItem *m = mapAt(canvas->scene(), sp, false)) choose(m);
        return;
    }
    if (stepAt >= 4 || detecting) return;
    QPointF p = map->mapFromScene(sp);
    if (!map->boundingRect().contains(p)) return;
    if (!pointA) {
        const QRectF q = quarter(stepAt);
        if (!q.adjusted(-q.width() * 0.1, -q.height() * 0.1, q.width() * 0.1, q.height() * 0.1).contains(p)) {
            setText(T("Click inside the highlighted quarter of the map. No squares or hexes there? Skip it (S)."));
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
        setText(T(hex ? "Now the centre of the hex next to it on the right." : "Now the next corner to its right, on the same line."));
        return;
    }
    if (!hex) p.setY(pointA->y());   // Atlas: the second corner is on the first one's row
    if (QLineF(*pointA, p).length() < 2) return;
    measurements << Measurement{*pointA, p};
    ++stepAt;
    pointA.reset();
    evaluate();
}

void Calibrator::evaluate() {
    if (!map || measurements.isEmpty()) return;
    const Calibration measuredGrid = gridFromMeasurements(measurements, hex);
    const auto a = analysisCache.value(map->src->path);
    if (!a) {   // never blocks: the measurement is shown now and refined when the lines are read
        waitingAnalysis = true;
        requestAnalysis();   // the lines may have left the cache (other maps were read meanwhile)
        setResult(measuredGrid, false, T("from your measurement (still reading the map's lines…)"));
        return;
    }
    waitingAnalysis = false;
    // The fit takes 0.5 to 1 s on a big map: it runs on a worker thread, the measured grid shows meanwhile.
    fitting = true;
    const int gen = ++fitGeneration;
    if (!result) {
        map->preview = measuredGrid;
        map->update();
    }
    QPointer<Calibrator> self(this);
    const QString path = map->src->path;
    QThreadPool::globalInstance()->start([self, a, measuredGrid, gen, path] {
        const std::optional<GridFit> fit = fitGrid(*a, measuredGrid.grid, measuredGrid.cellSize());
        QMetaObject::invokeMethod(qApp, [self, fit, measuredGrid, gen, path] {
            if (!self || gen != self->fitGeneration || !self->map || self->map->src->path != path) return;
            self->fitting = false;
            self->fitted(fit, measuredGrid);
        }, Qt::QueuedConnection);
    });
}

void Calibrator::fitted(const std::optional<GridFit> &fit, const Calibration &measuredGrid) {
    bool onClicks = fit.has_value();
    if (fit)
        for (const Measurement &m : measurements)
            for (QPointF c : {m.a, m.b}) onClicks = onClicks && nearLattice(fit->cal, c);
    if (onClicks) {
        lineQuarters = fit->quarters;
        setResult(fit->cal, true, fit->regions ? T("matched to the map's lines, following them in %1 regions").arg(fit->regions)
                                               : T("matched to the map's lines"));
    } else if (matched && result) {   // a grid the lines confirmed stays: the new measurement is left out
        measurements.removeLast();
        --stepAt;
        map->preview = *result;
        map->update();
        showStep();
        setText(T("That measurement did not match the grid the map's lines confirmed, so it was left out. "
                  "Measure again in the outlined quarter, skip it (S), or press Enter to apply the grid."));
    } else if (stepAt >= 4)
        setResult(measuredGrid, false, T("from the mean of your measurements (the map's lines did not confirm it)"));
    else {
        result.reset();
        matched = false;
        map->preview = measuredGrid;
        map->update();
        applyButton->setEnabled(true);   // the measurement alone can still be applied
        showStep();
    }
}

void Calibrator::useCell(double size) {
    if (!map) return setText(T("Click the map you want to calibrate."));
    ++fitGeneration;
    fitting = false;
    measurements.clear();
    stepAt = skipped = 0;
    pointA.reset();
    setResult(hex ? Calibration::hex(flatButton->isChecked() ? Calibration::HexFlat : Calibration::HexPointy, size, QPointF(size / 2, size / 2))
                  : Calibration::square(size, QPointF(0, 0)),
              false, T("typed in"));
    canvas->setFocus();   // the arrows move it now
}

void Calibrator::setResult(const Calibration &c, bool fromLines, const QString &text) {
    const bool first = !result;
    result = c;
    matched = fromLines;
    how = text;
    map->preview = c;
    map->update();
    if (first) {
        const QRectF r = map->sceneBoundingRect();
        canvas->fitInView(r.adjusted(-r.width() * 0.05, -r.height() * 0.05, r.width() * 0.05, r.height() * 0.05), Qt::KeepAspectRatio);
        emit canvas->zoomChanged(canvas->zoom());
    }
    applyButton->setEnabled(true);
    applyButton->setChecked(true);
    squaresButton->setChecked(!c.isHex());
    hexButton->setChecked(c.isHex());
    hex = c.isHex();
    topRow->setVisible(hex);
    if (hex) (c.grid == Calibration::HexFlat ? flatButton : pointyButton)->setChecked(true);
    unitsLabel->setText(T(hex ? "1 hex =" : "1 square ="));
    showStep();
}

void Calibrator::autoDetect() {
    if (!map) return setText(T("Click the map you want to calibrate."));
    if (detecting) return;
    reset();
    detecting = true;
    progress->clear();
    setText(T("Looking for squares or hexes over the whole map…"));
    // Read again when the lines left the cache: 0.9 waited for a reading that never came and F did nothing after.
    if (analysisCache.contains(map->src->path)) runDetection();
    else requestAnalysis();   // analysisReady() starts the detection
}

void Calibrator::runDetection() {
    const auto a = analysisCache.value(map->src->path);
    QPointer<Calibrator> self(this);
    const QString path = map->src->path;
    QThreadPool::globalInstance()->start([self, a, path] {
        const std::optional<GridFit> g = detectGrid(*a);
        QMetaObject::invokeMethod(qApp, [self, g, path] {
            if (!self || !self->map || self->map->src->path != path || !self->detecting) return;
            self->detecting = false;
            if (g) {
                self->measurements.clear();
                self->stepAt = self->skipped = 0;
                self->lineQuarters = g->quarters;
                self->setResult(g->cal, true, T("of %1 found automatically%2").arg(T(g->cal.isHex() ? "hexes" : "squares"))
                                                  .arg(g->regions ? T(", following the lines in %1 regions").arg(g->regions) : QString()));
            } else {
                self->showStep();
                self->setText(T("No grid found in the map's lines. Measure it: click a corner of a square (or a hex centre) in the highlighted quarter."));
            }
        }, Qt::QueuedConnection);
    });
}

void Calibrator::apply() {
    if (!map || !map->preview) return;
    Calibration c = *map->preview;
    c.unitsPerCell = perCell->value();
    c.unit = unit->text().trimmed().isEmpty() ? "m" : unit->text().trimmed();
    c.showGrid = map->cal.showGrid;   // the grid stays as the user had it: hidden unless they turned it on
    c.diagonal = map->cal.diagonal;
    map->cal = c;
    map->preview.reset();
    map->update();
    result.reset();
    sceneEdited();
    emit canvas->scene()->selectionChanged();   // refreshes a properties popup showing this map
    const QString msg = T("Map calibrated: 1 %1 = %2 px = %3 %4.")
                            .arg(T(c.noun(1))).arg(c.cellSize(), 0, 'f', 2).arg(c.unitsPerCell).arg(c.unit);
    canvas->setTool(Canvas::Select);
    emit canvas->status(msg);
}

bool Calibrator::key(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Escape: canvas->setTool(Canvas::Select); return true;
    case Qt::Key_Return: case Qt::Key_Enter: apply(); return true;
    case Qt::Key_F: autoDetect(); return true;
    case Qt::Key_S: skip(); return true;
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down: {
        if (!map || !map->preview) return true;
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 0.1 : 1;
        const int k = e->key();
        const QPointF d(k == Qt::Key_Left ? -step : k == Qt::Key_Right ? step : 0, k == Qt::Key_Up ? -step : k == Qt::Key_Down ? step : 0);
        map->preview->origin += d;
        if (result) result->origin += d;
        map->update();
        return true;
    }
    default: return false;
    }
}

void Calibrator::requestAnalysis() {
    const QString path = map->src->path;
    if (analysisCache.contains(path)) return keepAnalysis(path);
    if (analysisLoading.contains(path)) return;
    analysisLoading.insert(path);
    QPointer<Calibrator> self(this);
    QThreadPool::globalInstance()->start([self, path] {
        const auto a = QSharedPointer<const Analysis>::create(loadAnalysis(path));
        QMetaObject::invokeMethod(qApp, [self, path, a] {
            analysisLoading.remove(path);
            keepAnalysis(path, a);
            if (self) self->analysisReady(path);
        }, Qt::QueuedConnection);
    });
}

void Calibrator::analysisReady(const QString &path) {
    if (!map || map->src->path != path) return;
    if (detecting) runDetection();
    else if (waitingAnalysis) evaluate();
}
