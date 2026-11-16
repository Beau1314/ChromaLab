// The light table's arithmetic (task 6.4) — the statement-for-statement port
// of app/SpektraLab/Sources/LightTable/LightTableModel.swift (with
// LightTableStore.swift's StoredTable, LightTableMacMath.swift's
// MacActualSize and CatalogSourcesStore.swift's MacLightTableCarry.takes, and
// the sizing arithmetic of LightTableSizing.swift over the engine's own
// format table). A comment says where a statement differs and why; the
// header records what is deliberately not here. tests/lighttable.cpp holds
// every number against the Mac's own self-test values.
#include "LightTableModel.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QQuickWindow>
#include <QSaveFile>
#include <QScreen>

#include <cmath>

namespace spk::desktop::lighttable {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = kPi * 2.0;
constexpr int kSchemaVersion = 2;
constexpr int kDebounceMs = 300; // the website's 300 ms

bool contains(const std::vector<QUuid>& ids, QUuid id) {
    for (const QUuid& candidate : ids)
        if (candidate == id) return true;
    return false;
}
} // namespace

// --- rounding and angles ------------------------------------------------------

double jsRound(double v) { return std::floor(v + 0.5); }

double settle(double a, bool steps, double snap) {
    if (steps) return jsRound(a / Lt::step) * Lt::step;
    const double q = jsRound(a / Lt::quarter) * Lt::quarter;
    return std::abs(a - q) < snap ? q : a;
}

double unwind(double d) { return d - jsRound(d / kTwoPi) * kTwoPi; }

QString degreesText(double a) {
    double d = std::fmod(std::fmod((a / Lt::deg) + 180.0, 360.0) + 360.0, 360.0) - 180.0;
    if (std::abs(d + 180.0) < 1e-6) d = 180.0;
    const double r = jsRound(d * 10.0) / 10.0;
    if (r == std::round(r)) return QString::number(int(r)) + QChar(0x00B0);
    return QString::number(r, 'f', 1) + QChar(0x00B0);
}

QString mmText(double v) { return QString::number(jsRound(v * 10.0) / 10.0, 'f', 1); }

// --- the table's size -----------------------------------------------------------

QSizeF halfSize(Paper paper, bool portrait) {
    const QSizeF s = paperSize(paper);
    return portrait ? QSizeF(s.height() / 2.0, s.width() / 2.0)
                    : QSizeF(s.width() / 2.0, s.height() / 2.0);
}

QString paperName(Paper paper) {
    switch (paper) {
    case Paper::A4: return QStringLiteral("a4");
    case Paper::A5: return QStringLiteral("a5");
    case Paper::A6: return QStringLiteral("a6");
    }
    return QStringLiteral("a4");
}

std::optional<Paper> paperFromName(const QString& name) {
    if (name == QLatin1String("a4")) return Paper::A4;
    if (name == QLatin1String("a5")) return Paper::A5;
    if (name == QLatin1String("a6")) return Paper::A6;
    return std::nullopt;
}

QSizeF paperSize(Paper paper) {
    switch (paper) {
    case Paper::A4: return QSizeF(Lt::a4Width, Lt::a4Height);
    case Paper::A5: return QSizeF(210.0, 148.0);
    case Paper::A6: return QSizeF(148.0, 105.0);
    }
    return QSizeF(Lt::a4Width, Lt::a4Height);
}

// --- a seeded random ---------------------------------------------------------------

quint64 Random::next() {
    state += 0x9E3779B97F4A7C15ULL;
    quint64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double Random::unit() { return double(next() >> 11) / double(1ULL << 53); }

// --- a film -------------------------------------------------------------------------

int Item::quarter() const {
    const int q = int(jsRound(rot / Lt::quarter)) % 4;
    return (q + 4) % 4;
}

QSizeF Item::lying() const {
    return quarter() & 1 ? QSizeF(h(), w()) : QSizeF(w(), h());
}

QSizeF Item::extent() const {
    const double c = std::abs(std::cos(rot));
    const double n = std::abs(std::sin(rot));
    return QSizeF(w() * c + h() * n, w() * n + h() * c);
}

// --- the table ------------------------------------------------------------------------

Board::ItemList Board::pile() const {
    ItemList out;
    out.reserve(order.size());
    for (const QUuid& id : order)
        if (const Item* it = item(id)) out.push_back(*it);
    return out;
}

const Item* Board::item(QUuid id) const {
    for (const Item& it : items)
        if (it.id == id) return &it;
    return nullptr;
}

Item* Board::item(QUuid id) {
    for (Item& it : items)
        if (it.id == id) return &it;
    return nullptr;
}

QPointF Board::freeSpot(double w, double h, QPointF at, const ItemList* othersIn) const {
    const QSizeF hlf = half();
    const double x0 = -hlf.width() + Lt::clear + w / 2.0;
    const double x1 = hlf.width() - Lt::gap - w / 2.0;
    const double y0 = -hlf.height() + Lt::gap + h / 2.0;
    const double y1 = hlf.height() - Lt::clear - h / 2.0;
    const double fx = clampd(at.x(), std::min(x0, x1), std::max(x0, x1));
    const double fy = clampd(at.y(), std::min(y0, y1), std::max(y0, y1));
    if (x0 > x1 || y0 > y1) return QPointF(fx, fy);
    const ItemList pileList = pile();
    const ItemList& others = othersIn ? *othersIn : pileList;
    struct Box {
        double x, y, hw, hh;
    };
    std::vector<Box> boxes;
    boxes.reserve(others.size());
    for (const Item& o : others) {
        const QSizeF e = o.extent();
        boxes.push_back({o.x, o.y, e.width() / 2.0 + Lt::gap, e.height() / 2.0 + Lt::gap});
    }
    const auto isFree = [&boxes, w, h](double x, double y) {
        for (const Box& b : boxes)
            if (!(std::abs(x - b.x) >= b.hw + w / 2.0 || std::abs(y - b.y) >= b.hh + h / 2.0))
                return false;
        return true;
    };
    if (isFree(fx, fy)) return QPointF(fx, fy);
    QPointF best(fx, fy);
    double bd = std::numeric_limits<double>::infinity();
    bool found = false;
    for (double y = y0; y <= y1; y += 2.5)
        for (double x = x0; x <= x1; x += 2.5) {
            const double d = (x - at.x()) * (x - at.x()) + (y - at.y()) * (y - at.y());
            if (d < bd && isFree(x, y)) {
                bd = d;
                best = QPointF(x, y);
                found = true;
            }
        }
    return found ? best : QPointF(fx, fy);
}

QPointF Board::leastCovered(const Item& itIn, QPointF at, const ItemList* othersIn) const {
    const QSizeF e = itIn.extent();
    const QSizeF hlf = half();
    const ItemList pileList = pile();
    const ItemList& others = othersIn ? *othersIn : pileList;
    const double rx = std::max(0.0, hlf.width() - e.width() / 2.0);
    const double ry = std::max(0.0, hlf.height() - e.height() / 2.0);
    QPointF best(clampd(at.x(), -rx, rx), clampd(at.y(), -ry, ry));
    double least = std::numeric_limits<double>::infinity();
    double near = std::numeric_limits<double>::infinity();
    Item c = itIn;
    for (double y = -ry; y <= ry + 1e-9; y += 5.0)
        for (double x = -rx; x <= rx + 1e-9; x += 5.0) {
            c.x = x;
            c.y = y;
            double o = 0.0;
            for (const Item& other : others) {
                const double a = overlap(c, other);
                o += a * a;
            }
            const double d = (x - at.x()) * (x - at.x()) + (y - at.y()) * (y - at.y());
            if (o < least - 1e-6 || (std::abs(o - least) <= 1e-6 && d < near)) {
                least = o;
                near = d;
                best = QPointF(x, y);
            }
        }
    return best;
}

Item Board::keptOn(Item it) const {
    const QSizeF e = it.extent();
    const QSizeF hlf = half();
    it.x = clampd(it.x, std::min(0.0, -hlf.width() + e.width() / 2.0),
                  std::max(0.0, hlf.width() - e.width() / 2.0));
    it.y = clampd(it.y, std::min(0.0, -hlf.height() + e.height() / 2.0),
                  std::max(0.0, hlf.height() - e.height() / 2.0));
    return it;
}

QPointF Board::carried(const Item& it, QSizeF was, QSizeF now) {
    // the same table: exactly where it lay, not where a division and a
    // product round it to
    if (was == now) return it.centre();
    const QSizeF e = it.extent();
    const auto axis = [](double v, double a, double b, double size) {
        const double r0 = a - size / 2.0;
        const double r1 = b - size / 2.0;
        if (!(r0 > 1e-9)) return 0.0;
        return clampd(v / r0, -1.0, 1.0) * std::max(0.0, r1);
    };
    return QPointF(axis(it.x, was.width(), now.width(), e.width()),
                   axis(it.y, was.height(), now.height(), e.height()));
}

QHash<QUuid, QPointF> Board::carried(const ItemList& films, QSizeF was, QSizeF now) {
    QHash<QUuid, QPointF> out;
    if (was == now || films.empty()) {
        for (const Item& f : films) out.insert(f.id, f.centre());
        return out;
    }
    // one axis: the centres `c` and the films' extents `e` along it, the
    // room's half then and now
    const auto axis = [](const std::vector<double>& c, const std::vector<double>& e,
                         double r0, double r1) {
        double lo = std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        double cMax = -std::numeric_limits<double>::infinity();
        double cMin = std::numeric_limits<double>::infinity();
        const size_t n = c.size();
        for (size_t i = 0; i < n; ++i) {
            lo = std::min(lo, c[i] - e[i] / 2.0);
            hi = std::max(hi, c[i] + e[i] / 2.0);
            cMax = std::max(cMax, c[i]);
            cMin = std::min(cMin, c[i]);
        }
        const double span = hi - lo;
        const double mid = (lo + hi) / 2.0;
        const double cs = cMax - cMin;
        const double beyond = span - cs;
        const double k = (span <= 2.0 * r1 || cs < 1e-9) ? 1.0
                                                         : std::max(0.0, 2.0 * r1 - beyond) / cs;
        const double span1 = cs * k + beyond;
        const double f0 = r0 - span / 2.0;
        const double f1 = std::max(0.0, r1 - span1 / 2.0);
        const double mid1 = f0 > 1e-9 ? clampd(mid / f0, -1.0, 1.0) * f1 : 0.0;
        std::vector<double> mapped;
        mapped.reserve(n);
        for (double v : c) mapped.push_back(mid1 + (v - mid) * k);
        return mapped;
    };
    std::vector<double> ew;
    std::vector<double> eh;
    std::vector<double> xs0;
    std::vector<double> ys0;
    ew.reserve(films.size());
    eh.reserve(films.size());
    xs0.reserve(films.size());
    ys0.reserve(films.size());
    for (const Item& f : films) {
        const QSizeF e = f.extent();
        ew.push_back(e.width());
        eh.push_back(e.height());
        xs0.push_back(f.x);
        ys0.push_back(f.y);
    }
    const std::vector<double> xs = axis(xs0, ew, was.width(), now.width());
    const std::vector<double> ys = axis(ys0, eh, was.height(), now.height());
    for (size_t i = 0; i < films.size(); ++i) out.insert(films[i].id, QPointF(xs[i], ys[i]));
    return out;
}

bool Board::place(QUuid id, QSizeF size, std::optional<QPointF> at, const Kept* kept) {
    if (contains(order, id)) {
        setSize(id, size);
        toFront(id);
        return true;
    }
    if (isFull()) return false;
    tidied.reset();
    const Item* known = item(id);
    Item it = known ? *known : Item(id, size);
    it.id = id;
    it.size = size;
    const QPointF aim = at.value_or(QPointF(0.0, 0.0));
    if (kept) {
        it.rot = kept->rot;
        it.x = kept->x;
        it.y = kept->y;
        const QPointF p = carried(it, halfSize(kept->paper, kept->portrait), half());
        it.x = p.x();
        it.y = p.y();
        it = keptOn(it);
    } else {
        it.rot = 0.0;
        const QSizeF l = it.lying();
        QPointF p = freeSpot(l.width(), l.height(), aim);
        it.x = p.x();
        it.y = p.y();
        // No free spot left: not on the middle, where every film after would
        // land on the same place, but where it overlaps the films already
        // down least, nearest `at` among equals.
        bool overlaps = false;
        for (const Item& o : pile())
            if (overlap(it, o) > 0.0) {
                overlaps = true;
                break;
            }
        if (overlaps) {
            p = leastCovered(it, aim);
            it.x = p.x();
            it.y = p.y();
        }
    }
    it.on = true;
    put(it);
    order.push_back(id);
    return true;
}

void Board::takeOff(QUuid id) {
    Item* it = item(id);
    if (!it || !it->on) return;
    it->on = false;
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
    tidied.reset();
}

void Board::forget(QUuid id) {
    if (item(id) != nullptr) tidied.reset();
    items.erase(std::remove_if(items.begin(), items.end(),
                               [id](const Item& it) { return it.id == id; }),
                items.end());
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
}

void Board::toFront(QUuid id) {
    if (!order.empty() && order.back() == id) return;
    if (!contains(order, id)) return;
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
    order.push_back(id);
}

void Board::setSize(QUuid id, QSizeF size) {
    Item* it = item(id);
    if (!it || it->size == size) return;
    it->size = size;
    tidied.reset();
    if (it->on) *it = keptOn(*it);
}

void Board::turnQuarter(QUuid id, int dir) {
    Item* it = item(id);
    if (!it || !it->on) return;
    tidied.reset();
    it->rot += double(dir) * Lt::quarter;
    *it = keptOn(*it);
    toFront(id);
}

void Board::setRotation(QUuid id, double rot) {
    Item* it = item(id);
    if (!it || !it->on) return;
    if (it->rot != rot) tidied.reset();
    it->rot = rot;
}

void Board::moveTo(QUuid id, QPointF p, bool keepOn) {
    Item* it = item(id);
    if (!it || !it->on) return;
    if (it->x != p.x() || it->y != p.y()) tidied.reset();
    it->x = p.x();
    it->y = p.y();
    if (keepOn) *it = keptOn(*it);
}

void Board::move(QUuid id, QPointF delta) {
    const Item* it = item(id);
    if (!it || !it->on) return;
    moveTo(id, QPointF(it->x + delta.x(), it->y + delta.y()));
}

void Board::keepOn(QUuid id) {
    Item* it = item(id);
    if (!it || !it->on) return;
    *it = keptOn(*it);
}

void Board::orient(bool p) { relayout(paper, p); }

void Board::relayout(Paper newPaper, bool p) {
    if (newPaper == paper && p == portrait) return;
    const QSizeF was = half();
    const ItemList before = pile();
    // which films lay over which before
    QHash<QUuid, QSet<QUuid>> over;
    for (size_t i = 0; i < before.size(); ++i)
        for (size_t j = i + 1; j < before.size(); ++j)
            if (overlap(before[i], before[j]) > 1e-6) {
                over[before[i].id].insert(before[j].id);
                over[before[j].id].insert(before[i].id);
            }
    paper = newPaper;
    portrait = p;
    const QSizeF now = half();
    const QHash<QUuid, QPointF> to = carried(before, was, now);
    // films off the table, waiting, are carried too, so one put back lies
    // where it would have
    for (Item& it : items)
        if (!it.on) {
            const QPointF c = carried(it, was, now);
            it.x = c.x();
            it.y = c.y();
            put(it);
        }
    if (tidied) {
        tidy(*tidied);
        return;
    }
    ItemList laid;
    for (Item it : before) {
        const QPointF c = to.value(it.id, it.centre());
        it.x = c.x();
        it.y = c.y();
        it = keptOn(it);
        ItemList clear;
        for (const Item& o : laid)
            if (!over.value(o.id).contains(it.id)) clear.push_back(o);
        bool hits = false;
        for (const Item& o : clear)
            if (overlap(it, o) > 1e-6) {
                hits = true;
                break;
            }
        if (hits) {
            const QSizeF e = it.extent();
            const QPointF spot = freeSpot(e.width(), e.height(), c, &clear);
            Item q = it;
            q.x = spot.x();
            q.y = spot.y();
            bool qHits = false;
            for (const Item& o : clear)
                if (overlap(q, o) > 1e-6) {
                    qHits = true;
                    break;
                }
            if (qHits) {
                const QPointF least = leastCovered(it, c, &clear);
                q.x = least.x();
                q.y = least.y();
            }
            it = keptOn(q);
        }
        put(it);
        laid.push_back(it);
    }
}

void Board::tidy(const std::vector<QUuid>& ids) {
    const QSizeF hlf = half();
    const double space = 2.0 * Lt::gap;
    const double left = -hlf.width() + Lt::clear;
    const double right = hlf.width() - Lt::gap;
    const double top = hlf.height() - Lt::clear;
    const double bottom = -hlf.height() + Lt::gap;
    const double across = right - left;
    const double down = top - bottom;
    std::vector<Item> queue;
    for (const QUuid& id : ids) {
        if (!contains(order, id)) continue;
        const Item* found = item(id);
        if (!found) continue;
        bool dup = false;
        for (const Item& q : queue)
            if (q.id == id) {
                dup = true;
                break;
            }
        if (dup) continue;
        Item it = *found;
        it.rot = jsRound(it.rot / Lt::quarter) * Lt::quarter;
        queue.push_back(it);
    }
    for (const QUuid& id : order) {
        bool dup = false;
        for (const Item& q : queue)
            if (q.id == id) {
                dup = true;
                break;
            }
        if (dup) continue;
        const Item* found = item(id);
        if (!found) continue;
        Item it = *found;
        it.rot = jsRound(it.rot / Lt::quarter) * Lt::quarter;
        queue.push_back(it);
    }
    // The rows at a step share `f` along them: as many films as reach no
    // further than the light.
    const auto pack = [&](double f) {
        std::vector<std::vector<Item>> rows;
        std::vector<Item> row;
        double reach = 0.0;
        for (const Item& it : queue) {
            if (!row.empty()) {
                const Item& prev = row.back();
                const double at = reach + f * (prev.lying().width() + space);
                if (at + it.lying().width() <= across + 1e-9) {
                    row.push_back(it);
                    reach = at;
                    continue;
                }
                rows.push_back(row);
            }
            row.clear();
            row.push_back(it);
            reach = 0.0;
        }
        if (!row.empty()) rows.push_back(row);
        return rows;
    };
    const auto heights = [](const std::vector<std::vector<Item>>& rows) {
        std::vector<double> h;
        h.reserve(rows.size());
        for (const std::vector<Item>& r : rows) {
            double m = 0.0;
            for (const Item& it : r) m = std::max(m, it.lying().height());
            h.push_back(m);
        }
        return h;
    };
    // The share of each row's step down the light that lets the last row end
    // on the light, 1 at most.
    const auto fit = [&](const std::vector<std::vector<Item>>& rows) {
        const std::vector<double> h = heights(rows);
        double steps = 0.0;
        for (size_t i = 0; i + 1 < h.size(); ++i) steps += h[i] + space;
        if (h.empty()) return 1.0;
        const double last = h.back();
        if (steps <= 0.0) return 1.0;
        return std::min(1.0, std::max(0.0, down - last) / steps);
    };
    double f = 1.0;
    if (fit(pack(1.0)) < 1.0) {
        double best = fit(pack(1.0));
        double t = 0.95;
        while (t >= 0.2 - 1e-9) {
            const double score = std::min(t, fit(pack(t)));
            if (score > best + 1e-9) {
                best = score;
                f = t;
            }
            t -= 0.05;
        }
    }
    const std::vector<std::vector<Item>> rows = pack(f);
    const std::vector<double> h = heights(rows);
    const double g = fit(rows);
    const bool fanned = g < 1.0 || f < 1.0;
    double stepsSum = 0.0;
    for (size_t i = 0; i + 1 < h.size(); ++i) stepsSum += g * (h[i] + space);
    const double used = stepsSum + (h.empty() ? 0.0 : h.back());
    double rowTop = (top + bottom) / 2.0 + used / 2.0;
    std::vector<Item> laid;
    for (size_t r = 0; r < rows.size(); ++r) {
        const std::vector<Item>& row = rows[r];
        std::vector<double> steps;
        double reach = 0.0;
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) reach += f * (row[i - 1].lying().width() + space);
            steps.push_back(reach + row[i].lying().width() / 2.0);
        }
        const double lastWidth = row.empty() ? 0.0 : row.back().lying().width();
        const double x0 = (left + right) / 2.0 - (reach + lastWidth) / 2.0;
        for (size_t i = 0; i < row.size(); ++i) {
            Item it = row[i];
            it.x = x0 + steps[i];
            it.y = fanned ? rowTop - it.lying().height() / 2.0 : rowTop - h[r] / 2.0;
            laid.push_back(it);
        }
        rowTop -= g * (h[r] + space);
    }
    for (const Item& it : laid) put(keptOn(it));
    order.clear();
    for (const Item& it : laid) order.push_back(it.id);
    tidied = order;
}

void Board::random(quint64 seed) {
    Random rng(seed);
    const QSizeF hlf = half();
    ItemList best;
    int leastCrowded = std::numeric_limits<int>::max();
    double leastOver = std::numeric_limits<double>::infinity();
    double room = 0.0;
    for (const Item& it : pile()) room += (it.w() + Lt::gap) * (it.h() + Lt::gap);
    // A table the films cannot all lie apart on is not tried again.
    const int tries = room <= 0.6 * 4.0 * hlf.width() * hlf.height() ? 12 : 1;
    for (int t = 0; t < tries; ++t) {
        std::vector<QUuid> ids = order;
        for (int i = int(ids.size()) - 1; i >= 1; --i)
            std::swap(ids[size_t(i)], ids[size_t(rng.next() % quint64(i + 1))]);
        ItemList laid;
        double over = 0.0;
        int crowded = 0;
        for (const QUuid& id : ids) {
            const Item* found = item(id);
            if (!found) continue;
            Item it = *found;
            it.rot = 0.0;
            const QPointF aim((2.0 * rng.unit() - 1.0) * std::max(0.0, hlf.width() - it.w() / 2.0),
                              (2.0 * rng.unit() - 1.0) * std::max(0.0, hlf.height() - it.h() / 2.0));
            QPointF p = freeSpot(it.w(), it.h(), aim, &laid);
            it.x = p.x();
            it.y = p.y();
            // no free spot: `freeSpot` gives back the point asked for, which
            // may lie over a film or within its 3 mm
            bool isFree = true;
            for (const Item& o : laid)
                if (!(std::abs(it.x - o.x) >= (it.w() + o.w()) / 2.0 + Lt::gap - 1e-9 ||
                      std::abs(it.y - o.y) >= (it.h() + o.h()) / 2.0 + Lt::gap - 1e-9)) {
                    isFree = false;
                    break;
                }
            if (!isFree) {
                ++crowded;
                bool overlaps = false;
                for (const Item& o : laid)
                    if (overlap(it, o) > 0.0) {
                        overlaps = true;
                        break;
                    }
                if (overlaps) {
                    p = leastCovered(it, aim, &laid);
                    it.x = p.x();
                    it.y = p.y();
                }
            }
            it = keptOn(it);
            for (const Item& o : laid) {
                const double a = overlap(it, o);
                over += a * a;
            }
            laid.push_back(it);
        }
        if (crowded < leastCrowded || (crowded == leastCrowded && over < leastOver)) {
            leastCrowded = crowded;
            leastOver = over;
            best = laid;
        }
        if (crowded == 0) break;
    }
    for (const Item& it : best) put(it);
    order.clear();
    for (const Item& it : best) order.push_back(it.id);
    tidied.reset();
}

void Board::scatter(quint64 seed) {
    Random rng(seed);
    std::vector<QUuid> ids = order;
    for (int i = int(ids.size()) - 1; i >= 1; --i)
        std::swap(ids[size_t(i)], ids[size_t(rng.next() % quint64(i + 1))]);
    const QSizeF hlf = half();
    ItemList laid;
    for (const QUuid& id : ids) {
        const Item* found = item(id);
        if (!found) continue;
        Item it = *found;
        it.rot = jsRound(it.rot / Lt::quarter) * Lt::quarter +
                 (2.0 * rng.unit() - 1.0) * Lt::scatterTurn;
        const QSizeF e = it.extent();
        const double rx = std::max(0.0, hlf.width() - e.width() / 2.0);
        const double ry = std::max(0.0, hlf.height() - e.height() / 2.0);
        Item best = it;
        double least = std::numeric_limits<double>::infinity();
        for (int t = 0; t < 8; ++t) {
            Item c = it;
            c.x = (2.0 * rng.unit() - 1.0) * rx;
            c.y = (2.0 * rng.unit() - 1.0) * ry;
            double o = 0.0;
            for (const Item& other : laid) {
                const double a = overlap(c, other);
                o += a * a;
            }
            if (o < least) {
                least = o;
                best = c;
            }
        }
        laid.push_back(keptOn(best));
    }
    for (const Item& it : laid) put(it);
    order.clear();
    for (const Item& it : laid) order.push_back(it.id);
    tidied.reset();
}

double Board::overlap(const Item& a, const Item& b) {
    const QSizeF ea = a.extent();
    const QSizeF eb = b.extent();
    const double w = std::min(a.x + ea.width() / 2.0, b.x + eb.width() / 2.0) -
                     std::max(a.x - ea.width() / 2.0, b.x - eb.width() / 2.0);
    const double h = std::min(a.y + ea.height() / 2.0, b.y + eb.height() / 2.0) -
                     std::max(a.y - ea.height() / 2.0, b.y - eb.height() / 2.0);
    return std::max(0.0, w) * std::max(0.0, h);
}

std::optional<QUuid> Board::hit(QPointF p) const {
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const Item* f = item(*it);
        if (!f) continue;
        const double dx = p.x() - f->x;
        const double dy = p.y() - f->y;
        const double c = std::cos(f->rot);
        const double sn = std::sin(f->rot);
        if (std::abs(dx * c + dy * sn) <= f->w() / 2.0 + 0.5 &&
            std::abs(-dx * sn + dy * c) <= f->h() / 2.0 + 0.5)
            return *it;
    }
    return std::nullopt;
}

bool Board::onLight(QPointF p) const {
    const QSizeF hlf = half();
    return std::abs(p.x()) <= hlf.width() + Lt::rim && std::abs(p.y()) <= hlf.height() + Lt::rim;
}

void Board::put(const Item& it) {
    Item* known = item(it.id);
    if (known)
        *known = it;
    else
        items.push_back(it);
}

// --- the view onto it -------------------------------------------------------------------

QPointF Viewport::toWorld(QPointF p) const {
    return QPointF(cx + (p.x() - size.width() / 2.0) / s(),
                   cy - (p.y() - size.height() / 2.0) / s());
}

QPointF Viewport::toScreen(QPointF w) const {
    return QPointF(size.width() / 2.0 + (w.x() - cx) * s(),
                   size.height() / 2.0 - (w.y() - cy) * s());
}

QSizeF Viewport::box() const {
    return QSizeF(2.0 * (half.width() + Lt::rim + Lt::desk),
                  2.0 * (half.height() + Lt::rim + Lt::desk));
}

double Viewport::clampAxis(double c, double h, double view, double lo, double hi) const {
    const auto r = axisRange(h, view, lo, hi);
    return clampd(c, r.first, r.second);
}

std::pair<double, double> Viewport::axisRange(double h, double view, double lo,
                                              double hi) const {
    const double mn = -h + (view / 2.0 - lo) / s();
    const double mx = h - (view / 2.0 - hi) / s();
    if (mn > mx) {
        const double one = (hi - lo) / (2.0 * s());
        return {one, one};
    }
    return {mn, mx};
}

std::pair<std::pair<double, double>, std::pair<double, double>> Viewport::ranges() const {
    const QSizeF b = box();
    return {axisRange(b.width() / 2.0, size.width(), inset.left, inset.right),
            axisRange(b.height() / 2.0, size.height(), inset.bottom, inset.top)};
}

void Viewport::clampView() {
    const QSizeF b = box();
    cx = clampAxis(cx, b.width() / 2.0, size.width(), inset.left, inset.right);
    cy = clampAxis(cy, b.height() / 2.0, size.height(), inset.bottom, inset.top);
}

double Viewport::band(double o, double d) { return (1.0 - 1.0 / (o * 0.55 / d + 1.0)) * d; }

double Viewport::unband(double b, double d) {
    b = std::min(b, 0.999 * d);
    return b * d / ((d - b) * 0.55);
}

double Viewport::rubber(double x, double lo, double hi, double d) {
    return x < lo ? lo - band(lo - x, d) : x > hi ? hi + band(x - hi, d) : x;
}

double Viewport::unrubber(double x, double lo, double hi, double d) {
    return x < lo ? lo - unband(lo - x, d) : x > hi ? hi + unband(x - hi, d) : x;
}

void Viewport::hold(double z, QPointF w, QPointF p) {
    zoom = std::exp(rubber(std::log(std::max(z, 1e-6)), std::log(zoomFloor),
                           std::log(zoomCeiling), Lt::zoomGive));
    const auto r = ranges();
    cx = rubber(w.x() - (p.x() - size.width() / 2.0) / s(), r.first.first, r.first.second,
                size.width() / s());
    cy = rubber(w.y() + (p.y() - size.height() / 2.0) / s(), r.second.first, r.second.second,
                size.height() / s());
}

std::pair<double, QPointF> Viewport::held(QPointF p) const {
    const double z = std::exp(unrubber(std::log(zoom), std::log(zoomFloor),
                                       std::log(zoomCeiling), Lt::zoomGive));
    const auto r = ranges();
    const double x = unrubber(cx, r.first.first, r.first.second, size.width() / s());
    const double y = unrubber(cy, r.second.first, r.second.second, size.height() / s());
    return {z, QPointF(x + (p.x() - size.width() / 2.0) / s(),
                       y - (p.y() - size.height() / 2.0) / s())};
}

bool Viewport::isSettled() const {
    Viewport v = *this;
    v.zoomAt(zoom, QPointF(cx, cy), QPointF(size.width() / 2.0, size.height() / 2.0));
    return std::abs(v.zoom - zoom) < 1e-6 && std::abs(v.cx - cx) * s() < 0.01 &&
           std::abs(v.cy - cy) * s() < 0.01;
}

Viewport Viewport::settled(QPointF p) const {
    Viewport v = *this;
    v.zoomAt(clampd(zoom, zoomFloor, zoomCeiling), toWorld(p), p);
    return v;
}

void Viewport::put(double z, double x, double y) {
    zoom = z;
    cx = x;
    cy = y;
}

void Viewport::zoomAt(double z, QPointF w, QPointF p) {
    zoom = clampd(z, zoomFloor, zoomCeiling);
    cx = w.x() - (p.x() - size.width() / 2.0) / s();
    cy = w.y() + (p.y() - size.height() / 2.0) / s();
    clampView();
}

QPointF Viewport::middle() const {
    return QPointF(size.width() / 2.0 + (inset.left - inset.right) / 2.0,
                   size.height() / 2.0 + (inset.top - inset.bottom) / 2.0);
}

void Viewport::setZoom(double z, std::optional<QPointF> p) {
    const QPointF at = p.value_or(middle());
    zoomAt(z, toWorld(at), at);
}

void Viewport::pan(QPointF c0, QPointF d) {
    cx = c0.x() - d.x() / s();
    cy = c0.y() + d.y() / s();
    clampView();
}

bool Viewport::measure(QSizeF new_, Insets newInset, QSizeF newHalf) {
    if (new_.width() < 1.0 || new_.height() < 1.0) return false;
    const bool sameHalf = newHalf == half;
    half = newHalf;
    inset = newInset;
    const QSizeF b = box();
    const double fit = std::max(0.2, std::min((new_.width() - inset.left - inset.right) / b.width(),
                                              (new_.height() - inset.top - inset.bottom) / b.height()));
    const bool sized = new_ != size;
    // Nothing changed: the view stays as it is. A view at rest is already
    // within its limits.
    if (!sized && fit == sFit && measured && sameHalf) return false;
    const std::optional<QPointF> keep =
        measured ? std::optional<QPointF>(toWorld(QPointF(size.width() / 2.0, size.height() / 2.0)))
                 : std::nullopt;
    size = new_;
    sFit = fit;
    if (keep) {
        cx = keep->x();
        cy = keep->y();
    }
    clampView();
    measured = true;
    return true;
}

// --- Actual Size --------------------------------------------------------------------------

namespace ActualSize {

std::optional<double> pxPerMM(double dpiX, double dpiY, double dpr) {
    // No EDID size (a projector, a capture card): no 1:1 — no size is claimed
    // that is not known. (The Mac reads CGDisplayScreenSize's millimetres and
    // guards mm >= 20; Qt's physicalDotsPerInch is 0 or negative for exactly
    // the displays whose size nothing knows.)
    if (!(dpiX > 0.0) || !(dpiY > 0.0) || !(dpr > 0.0)) return std::nullopt;
    const double x = dpiX / 25.4;
    const double y = dpiY / 25.4;
    // Pixels are square: a size whose two axes disagree is not a real size.
    if (std::abs(x / y - 1.0) > 0.05) return std::nullopt;
    const double long_ = std::max(x, y) / dpr;
    // From a 100-inch projector (0.25 px a mm) to a panel past 20: anything
    // outside is a bad EDID.
    if (!(long_ > 0.25) || !(long_ < 20.0)) return std::nullopt;
    return long_;
}

} // namespace ActualSize

// --- admission and sizing --------------------------------------------------------------------

namespace Carry {

bool takes(bool developed, bool filmEdgeActive, bool pair) {
    return pair || filmEdgeActive || developed;
}

std::optional<FilmFormat> filmFormat(const QString& id) {
    // The engine's own table (kFormats, engine/src/pipeline/overscan.cpp):
    // 135's gate is a camera's, measured on seven strips (36.25 x 24.3), not
    // ISO's 36 x 24.
    static const FilmFormat kFormats[] = {
        {"135", 35.0, 24.3, 36.25, true},   {"135_half", 35.0, 24.0, 18.0, true},
        {"120_645", 61.0, 56.0, 41.5, false}, {"120_6x6", 61.0, 56.0, 56.0, false},
        {"120_6x7", 61.0, 56.0, 69.5, false}, {"120_6x8", 61.0, 56.0, 76.0, false},
        {"120_6x9", 61.0, 56.0, 84.0, false}, {"135_xpan", 35.0, 24.0, 65.0, true},
        {"120_6x12", 61.0, 56.0, 112.0, false}, {"120_6x17", 61.0, 56.0, 168.0, false},
    };
    for (const FilmFormat& f : kFormats)
        if (id == QLatin1String(f.id)) return f;
    return std::nullopt;
}

std::optional<QSizeF> frameSizeMM(const Spec& spec) {
    // A half-frame pair: the piece, two 18 x 24 holes and the gap
    // (HalfFramePair.pieceSizeMM; PairPiece's kHoleAlongMM/kHoleAcrossMM are
    // the same constants). The Mac's strip/filed margins (0.5 / 1.0) price a
    // pair's film edge, which this frontend does not draw, so a pair always
    // lies as its piece.
    if (spec.pair) {
        if (!(spec.pairSpacingMM >= 0.0)) return std::nullopt;
        const double along = 18.0 * 2.0 + spec.pairSpacingMM;
        const double across = 24.0;
        return spec.pairTurned ? QSizeF(across, along) : QSizeF(along, across);
    }
    // A frame in its film: the film's canvas, as FilmEdgeSettings.canvasEstimate
    // prices it — a strip ends at the film's width; filed, the gate and about
    // 1.1 mm of carrier.
    if (spec.filmEdgeActive) {
        const std::optional<FilmFormat> format = filmFormat(spec.filmEdgeFormat);
        if (!format) return std::nullopt; // no format the table knows: no size, not a guess
        const double w = std::max(spec.originalPx.width(), 1.0);
        const double h = std::max(spec.originalPx.height(), 1.0);
        if (spec.originalPx.width() <= 0.0 || spec.originalPx.height() <= 0.0) return std::nullopt;
        const double px = std::max(format->alongMM, format->acrossMM) / std::max(w, h); // mm a pixel
        // Which way the frame runs on the film (FilmEdgeFormat.runsDown).
        const bool vertical =
            std::abs(w * px - format->acrossMM) < std::abs(h * px - format->acrossMM) &&
            std::abs(w - h) * px > 1e-6;
        const double alongPx = vertical ? h : w;
        const bool strip = spec.filmEdgeMode == QLatin1String("strip");
        if (!strip && spec.filmEdgeMode != QLatin1String("filed")) return std::nullopt;
        const double marginMM =
            strip ? (spec.filmEdgeFormat == QLatin1String("135_half")
                         ? 0.5
                         : (format->perforated ? 0.95 : 2.35))
                  : 1.0;
        const double acrossMM =
            strip ? format->filmWidthMM + (format->perforated ? 0.0 : 0.15)
                  : (vertical ? w : h) * px + 2.2;
        const double alongMM = (alongPx + 2.0 * jsRound(marginMM / px)) * px;
        const double acrossLaid = jsRound(acrossMM / px) * px;
        return vertical ? QSizeF(acrossLaid, alongMM) : QSizeF(alongMM, acrossLaid);
    }
    // A bare frame: the shown picture at the uncut frame's millimetres a
    // pixel. A crop is a smaller piece of the same negative: the mm a pixel
    // are the uncut frame's.
    if (spec.developed) {
        if (!(spec.filmFormatMM > 0.0)) return std::nullopt;
        if (spec.originalPx.width() <= 0.0 || spec.originalPx.height() <= 0.0) return std::nullopt;
        if (spec.displayPx.width() <= 0.0 || spec.displayPx.height() <= 0.0) return std::nullopt;
        const double k =
            spec.filmFormatMM / std::max(std::max(spec.originalPx.width(), spec.originalPx.height()), 1.0);
        return QSizeF(std::max(spec.displayPx.width(), 1.0) * k,
                      std::max(spec.displayPx.height(), 1.0) * k);
    }
    return std::nullopt;
}

} // namespace Carry

// --- the table as it is written -----------------------------------------------------------------

StoredTable StoredTable::from(const Board& board) {
    StoredTable stored;
    stored.portrait = board.portrait;
    stored.paper = board.paper;
    QHash<QUuid, int> z;
    int index = 0;
    for (const QUuid& id : board.order) z.insert(id, index++);
    stored.places.reserve(board.items.size());
    for (const Item& it : board.items)
        stored.places.push_back(Place{it.id, it.on, it.x, it.y, it.rot, z.value(it.id, -1)});
    return stored;
}

QJsonDocument StoredTable::toJson() const {
    QJsonObject root;
    root.insert(QLatin1String("schemaVersion"), kSchemaVersion);
    root.insert(QLatin1String("portrait"), portrait);
    if (paper.has_value()) root.insert(QLatin1String("paper"), paperName(*paper));
    QJsonArray placesArray;
    for (const Place& p : places) {
        QJsonObject place;
        place.insert(QLatin1String("frame"), p.frame.toString(QUuid::WithoutBraces));
        place.insert(QLatin1String("on"), p.on);
        place.insert(QLatin1String("x"), p.x);
        place.insert(QLatin1String("y"), p.y);
        place.insert(QLatin1String("rot"), p.rot);
        place.insert(QLatin1String("z"), p.z);
        placesArray.append(place);
    }
    root.insert(QLatin1String("places"), placesArray);
    return QJsonDocument(root);
}

std::optional<StoredTable> StoredTable::parse(const QByteArray& bytes) {
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    const QJsonObject root = doc.object();
    // This build reads schemas 1 and 2; anything else may carry shapes it
    // would misread, so it is refused (and set aside by the caller) rather
    // than half-understood.
    const QJsonValue versionValue = root.value(QLatin1String("schemaVersion"));
    if (!versionValue.isDouble()) return std::nullopt;
    const int version = int(versionValue.toDouble());
    if (version != 1 && version != 2) return std::nullopt;
    const QJsonValue portraitValue = root.value(QLatin1String("portrait"));
    if (!portraitValue.isBool()) return std::nullopt;
    StoredTable stored;
    stored.portrait = portraitValue.toBool();
    const QJsonValue paperValue = root.value(QLatin1String("paper"));
    if (paperValue.isString()) {
        const std::optional<Paper> named = paperFromName(paperValue.toString());
        if (!named) return std::nullopt;
        stored.paper = *named;
    } else if (!paperValue.isUndefined()) {
        return std::nullopt;
    }
    const QJsonValue placesValue = root.value(QLatin1String("places"));
    if (!placesValue.isArray()) return std::nullopt;
    const QJsonArray placesArray = placesValue.toArray();
    for (const QJsonValue& v : placesArray) {
        if (!v.isObject()) return std::nullopt;
        const QJsonObject o = v.toObject();
        const QJsonValue frameValue = o.value(QLatin1String("frame"));
        if (!frameValue.isString()) return std::nullopt;
        const QUuid frame(frameValue.toString());
        if (frame.isNull()) return std::nullopt;
        const QJsonValue onValue = o.value(QLatin1String("on"));
        if (!onValue.isBool()) return std::nullopt;
        const QJsonValue xValue = o.value(QLatin1String("x"));
        const QJsonValue yValue = o.value(QLatin1String("y"));
        const QJsonValue rotValue = o.value(QLatin1String("rot"));
        const QJsonValue zValue = o.value(QLatin1String("z"));
        if (!xValue.isDouble() || !yValue.isDouble() || !rotValue.isDouble() || !zValue.isDouble())
            return std::nullopt;
        stored.places.push_back(Place{frame, onValue.toBool(), xValue.toDouble(),
                                      yValue.toDouble(), rotValue.toDouble(),
                                      int(zValue.toDouble())});
    }
    return stored;
}

namespace {
std::vector<StoredTable::Place> sortedPlaces(const std::vector<StoredTable::Place>& places) {
    std::vector<StoredTable::Place> sorted = places;
    // The pile in its order then the ones off it; stable, because the z keys
    // of the on-table places are distinct but the off-table ones share -1.
    std::stable_sort(sorted.begin(), sorted.end(), [](const StoredTable::Place& a,
                                                      const StoredTable::Place& b) {
        return (a.on ? a.z : std::numeric_limits<int>::max()) <
               (b.on ? b.z : std::numeric_limits<int>::max());
    });
    return sorted;
}
} // namespace

Board StoredTable::restore(Paper nowPaper, bool nowPortrait,
                           const std::function<std::optional<QSizeF>(QUuid)>& size) const {
    Board m;
    m.relayout(nowPaper, nowPortrait);
    for (const Place& p : sortedPlaces(places)) {
        const std::optional<QSizeF> s = size(p.frame);
        if (!s.has_value()) continue; // a frame the library let go of drops off the table
        if (p.on) {
            Board::Kept kept;
            kept.x = p.x;
            kept.y = p.y;
            kept.rot = p.rot;
            kept.portrait = portrait;
            kept.paper = keptPaper();
            m.place(p.frame, *s, std::nullopt, &kept);
        } else {
            m.place(p.frame, *s);
            m.takeOff(p.frame);
        }
    }
    return m;
}

Board StoredTable::boardUnknownSizes() const {
    // The Windows open path: the table comes back at its own kept size and
    // way up, each film where it lay, its pile in its order — sizes still
    // unknown (0 x 0) until the library's true millimetres arrive with the
    // next sync. (The Mac carries each place to the table the page opens
    // with; this page opens on the kept table, so there is nothing to carry.)
    Board b;
    b.paper = keptPaper();
    b.portrait = portrait;
    for (const Place& p : sortedPlaces(places)) {
        Item it(p.frame, QSizeF(0.0, 0.0), p.x, p.y);
        it.rot = p.rot;
        it.on = p.on;
        b.items.push_back(it);
        if (p.on) b.order.push_back(p.frame);
    }
    return b;
}

// --- the QML-facing table ------------------------------------------------------------------------

TableModel::TableModel(QObject* parent) : QAbstractListModel(parent) {
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(kDebounceMs);
    connect(&saveTimer_, &QTimer::timeout, this, &TableModel::writeStore);
}

int TableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return int(board_.items.size());
}

QHash<int, QByteArray> TableModel::roleNames() const {
    return {{FrameRole, "frame"},    {OnRole, "on"},
            {XRole, "x"},            {YRole, "y"},
            {RotRole, "rotDeg"},     {WRole, "w"},
            {HRole, "h"},            {ImageRole, "image"},
            {HasImageRole, "hasImage"}, {ZRole, "z"}};
}

QVariant TableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= int(board_.items.size()))
        return {};
    const Item& it = board_.items[size_t(index.row())];
    switch (role) {
    case FrameRole: return it.id.toString();
    case OnRole: return it.on;
    case XRole: return it.x;
    case YRole: return it.y;
    case RotRole: return it.rot * 180.0 / kPi;
    case WRole: return it.w();
    case HRole: return it.h();
    case ImageRole: return images_.value(it.id);
    case HasImageRole: return images_.contains(it.id) && !images_.value(it.id).isNull();
    case ZRole: {
        int z = 0;
        for (const QUuid& id : board_.order) {
            if (id == it.id) return z;
            ++z;
        }
        return -1;
    }
    default: return {};
    }
}

int TableModel::offCount() const {
    int n = 0;
    for (const Item& it : board_.items)
        if (!it.on) ++n;
    return n;
}

void TableModel::setPortrait(bool value) {
    if (board_.portrait == value) return;
    board_.orient(value);
    boardMutated();
}

double TableModel::itemX(const QString& uuid) const {
    const Item* it = board_.item(QUuid(uuid));
    return it ? it->x : 0.0;
}

double TableModel::itemY(const QString& uuid) const {
    const Item* it = board_.item(QUuid(uuid));
    return it ? it->y : 0.0;
}

double TableModel::itemW(const QString& uuid) const {
    const Item* it = board_.item(QUuid(uuid));
    return it ? it->w() : 0.0;
}

double TableModel::itemH(const QString& uuid) const {
    const Item* it = board_.item(QUuid(uuid));
    return it ? it->h() : 0.0;
}

double TableModel::itemRot(const QString& uuid) const {
    const Item* it = board_.item(QUuid(uuid));
    return it ? it->rot : 0.0;
}

bool TableModel::itemOn(const QString& uuid) const {
    const Item* it = board_.item(QUuid(uuid));
    return it ? it->on : false;
}

void TableModel::open(const QString& path) {
    storePath_ = path;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = file.readAll();
        const std::optional<StoredTable> stored = StoredTable::parse(bytes);
        if (stored.has_value()) {
            board_ = stored->boardUnknownSizes();
        } else {
            setAside(path, bytes);
            board_ = Board{};
        }
    } else {
        board_ = Board{};
    }
    beginResetModel();
    endResetModel();
    bumpRevision();
    emit boardChanged();
}

void TableModel::close() { saveNow(); }

void TableModel::saveNow() {
    saveTimer_.stop();
    writeStore();
}

int TableModel::carry(const QVariantList& frames) {
    // A place stored before its library size arrived (a 0 x 0 item from
    // open) meets its true millimetres on the carry or sync that offers it;
    // until then it is unsized and the page leaves it undrawn.
    int placed = 0;
    for (const QVariant& entry : frames) {
        const QVariantMap frame = entry.toMap();
        const QUuid id(frame.value(QLatin1String("uuid")).toString());
        if (id.isNull()) continue;
        const double mmw = frame.value(QLatin1String("mmw")).toDouble();
        const double mmh = frame.value(QLatin1String("mmh")).toDouble();
        if (!(mmw > 0.0) || !(mmh > 0.0)) continue; // no size: not qualified, never a guess
        if (board_.place(id, QSizeF(mmw, mmh))) {
            const QVariant image = frame.value(QLatin1String("image"));
            if (image.canConvert<QImage>()) {
                const QImage picture = image.value<QImage>();
                if (!picture.isNull()) images_.insert(id, picture);
            }
            ++placed;
        }
    }
    lastCarried_ = placed;
    boardMutated();
    return placed;
}

void TableModel::sync(const QVariantList& frames) {
    QSet<QUuid> offered;
    for (const QVariant& entry : frames) {
        const QVariantMap frame = entry.toMap();
        const QUuid id(frame.value(QLatin1String("uuid")).toString());
        if (id.isNull()) continue;
        const double mmw = frame.value(QLatin1String("mmw")).toDouble();
        const double mmh = frame.value(QLatin1String("mmh")).toDouble();
        if (!(mmw > 0.0) || !(mmh > 0.0)) continue; // no size record: does not qualify
        offered.insert(id);
        const QVariant image = frame.value(QLatin1String("image"));
        if (image.canConvert<QImage>()) {
            const QImage picture = image.value<QImage>();
            if (!picture.isNull()) images_.insert(id, picture);
        }
        board_.setSize(id, QSizeF(mmw, mmh));
    }
    // A frame ON the table the library no longer offers drops off for good
    // (LightTableController.sync). A film waiting off the table is not
    // dropped by a sync: the page's sync carries the page's current query,
    // and a waiting film out of that query keeps waiting.
    for (const Item& it : board_.items) {
        if (!it.on || offered.contains(it.id)) continue;
        board_.forget(it.id);
    }
    boardMutated();
}

void TableModel::takeOff(const QString& uuid) { board_.takeOff(QUuid(uuid)); boardMutated(); }

void TableModel::turnQuarter(const QString& uuid, int dir) {
    board_.turnQuarter(QUuid(uuid), dir);
    boardMutated();
}

void TableModel::rotateTo(const QString& uuid, double radians, bool steps, double snapDegrees) {
    const double settled =
        settle(radians, steps, snapDegrees * Lt::deg);
    board_.setRotation(QUuid(uuid), settled);
    board_.keepOn(QUuid(uuid));
    boardMutated();
}

void TableModel::moveTo(const QString& uuid, double x, double y) {
    board_.moveTo(QUuid(uuid), QPointF(x, y));
    boardMutated();
}

void TableModel::tidyUp() {
    board_.tidy(board_.order);
    boardMutated();
}

void TableModel::randomize(int seed) {
    board_.random(quint64(seed));
    boardMutated();
}

void TableModel::scatterNow(int seed) {
    board_.scatter(quint64(seed));
    boardMutated();
}

void TableModel::setPaper(const QString& name) {
    const QString lower = name.toLower();
    const std::optional<Paper> paper = paperFromName(lower);
    if (!paper || *paper == board_.paper) return;
    board_.relayout(*paper, board_.portrait);
    boardMutated();
}

void TableModel::setSelected(const QString& uuid) {
    if (selected_ == uuid) return;
    selected_ = uuid;
    bumpRevision();
}

QPointF TableModel::toWorld(QPointF p) const { return viewport_.toWorld(p); }

QPointF TableModel::toScreen(QPointF w) const { return viewport_.toScreen(w); }

void TableModel::measureView(double width, double height, double top, double left, double bottom,
                             double right) {
    Viewport::Insets insets;
    insets.top = top;
    insets.left = left;
    insets.bottom = bottom;
    insets.right = right;
    if (viewport_.measure(QSizeF(width, height), insets, board_.half())) viewMutated();
}

void TableModel::zoomAt(double z, QPointF world, QPointF screen) {
    viewport_.zoomAt(z, world, screen);
    if (!actualSizeActive_) {
        viewport_.zoomFloor = 1.0;
        viewport_.zoomCeiling = Lt::zoomMax;
    }
    viewMutated();
}

void TableModel::setZoom(double z) {
    viewport_.setZoom(z);
    if (!actualSizeActive_) {
        viewport_.zoomFloor = 1.0;
        viewport_.zoomCeiling = Lt::zoomMax;
    }
    viewMutated();
}

void TableModel::zoomIn() {
    setZoom(viewport_.zoom * 1.5);
}

void TableModel::zoomOut() {
    setZoom(viewport_.zoom / 1.5);
}

void TableModel::fitView() {
    actualSizeActive_ = false;
    viewport_.zoomFloor = 1.0;
    viewport_.zoomCeiling = Lt::zoomMax;
    viewport_.setZoom(1.0);
    viewMutated();
}

void TableModel::attachWindow(QObject* window) {
    const QQuickWindow* quick = qobject_cast<const QQuickWindow*>(window);
    if (!quick || !quick->screen()) return;
    const QScreen* screen = quick->screen();
    const std::optional<double> px =
        ActualSize::pxPerMM(screen->physicalDotsPerInchX(), screen->physicalDotsPerInchY(),
                            quick->effectiveDevicePixelRatio());
    const bool changed = px != pointsPerMM_;
    pointsPerMM_ = px;
    if (changed) emit screenPropertyChanged();
}

void TableModel::actualSize() {
    // 1:1 about the middle of the view; a second call goes back to fit.
    if (actualSizeActive_) {
        fitView();
        return;
    }
    if (!pointsPerMM_.has_value() || viewport_.sFit <= 0.0) return;
    const double z = pointsPerMM_.value() / viewport_.sFit;
    // The zoom's limits made to hold the real size, which may lie under fit
    // (a small table in a large window) or past 8x; back to 1-8x once the
    // view is at rest and not at Actual Size.
    viewport_.zoomFloor = std::min(1.0, z);
    viewport_.zoomCeiling = std::max(Lt::zoomMax, z);
    actualSizeActive_ = true;
    viewport_.zoomAt(z, viewport_.toWorld(viewport_.middle()), viewport_.middle());
    viewMutated();
}

void TableModel::panView(QPointF from, double dx, double dy) {
    viewport_.pan(from, QPointF(dx, dy));
    viewMutated();
}

QRectF TableModel::litScreenRect() const {
    const QSizeF h = board_.half();
    return QRectF(viewport_.toScreen(QPointF(-h.width(), h.height())),
                  viewport_.toScreen(QPointF(h.width(), -h.height())));
}

QRectF TableModel::frameScreenRect() const {
    const double r = Lt::rim * viewport_.s();
    return litScreenRect().adjusted(-r, -r, r, r);
}

void TableModel::beginGesture() { ++gestureDepth_; }

void TableModel::endGesture() {
    if (gestureDepth_ > 0) --gestureDepth_;
    if (gestureDepth_ == 0) boardMutated(); // catch the model up with the holds
}

void TableModel::boardMutated() {
    if (gestureDepth_ == 0) {
        beginResetModel();
        endResetModel();
    } else if (!board_.items.empty()) {
        // a reset would destroy the dragging delegate; the roles land
        // through dataChanged instead, so model.x follows the holds live
        emit dataChanged(index(0), index(int(board_.items.size()) - 1));
    }
    bumpRevision();
    emit boardChanged();
    scheduleSave();
}

void TableModel::viewMutated() {
    bumpRevision();
    emit viewChanged();
}

void TableModel::bumpRevision() {
    ++revision_;
    emit revisionChanged();
}

void TableModel::scheduleSave() {
    if (storePath_.isEmpty()) return;
    saveTimer_.start();
}

void TableModel::writeStore() {
    if (storePath_.isEmpty()) return;
    const QJsonDocument doc = StoredTable::from(board_).toJson();
    QDir().mkpath(QFileInfo(storePath_).absolutePath());
    QSaveFile file(storePath_);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson(QJsonDocument::Compact));
        if (file.commit()) emit storeSaved(storePath_);
    }
}

void TableModel::setAside(const QString& path, const QByteArray& bytes) {
    // A kept file this build cannot read is copied aside, never written over:
    // `<stem>.unreadable-<seconds>.<ext>`, once per content.
    const QFileInfo info(path);
    const QDir dir(info.absolutePath());
    const QString prefix = info.completeBaseName() + QStringLiteral(".unreadable-");
    const QStringList entries = dir.entryList({prefix + QStringLiteral("*")}, QDir::Files);
    for (const QString& entry : entries) {
        QFile copy(dir.filePath(entry));
        if (copy.open(QIODevice::ReadOnly) && copy.readAll() == bytes) return;
    }
    const QString name = prefix + QString::number(QDateTime::currentSecsSinceEpoch()) +
                         (info.suffix().isEmpty() ? QString() : QStringLiteral(".") + info.suffix());
    if (QFile::copy(path, dir.filePath(name))) emit storeAside(dir.filePath(name));
}

} // namespace spk::desktop::lighttable
