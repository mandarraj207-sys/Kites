#include "ui/optimizer_tab/dependency_arrows.h"

#include "ui/optimizer_tab/instruction_listing_highlighter.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <map>

namespace Kites {
namespace {

constexpr qreal kLineWidth = 1.6;
constexpr qreal kCornerRadius = 3.0;
constexpr qreal kArrowHeadSize = 4.0;
constexpr qreal kGutterPadding = 5.0;
constexpr qreal kMaxLaneSpacing = 6.0;
constexpr qreal kMaxAttachSpacing = 4.0;
constexpr qreal kRowAttachSpan = 0.6; // fraction of a row's height arrows may spread over
constexpr qreal kLabelGap = 14.0;
constexpr qreal kLabelFontScale = 0.9;

struct Label
{
    QString text;
    QColor color;
};

bool overlaps(const DependencyArrow &a, const DependencyArrow &b)
{
    return a.producer <= b.consumer && b.producer <= a.consumer;
}

// Lanes are packed against the text: the innermost lane sits right in front of
// the arrowheads and longer arrows nest further out, keeping the horizontal runs short.
qreal laneX(const GutterMetrics &gutter, const DependencyArrowLayout &layout, int lane)
{
    const qreal innermost = gutter.right - kArrowHeadSize - kGutterPadding;
    const qreal usable = innermost - (gutter.left + kGutterPadding);
    const qreal spacing = layout.laneCount > 1
                              ? std::min(kMaxLaneSpacing, usable / (layout.laneCount - 1))
                              : 0.0;
    return innermost - (layout.laneCount - 1 - lane) * spacing;
}

/** Spreads the arrows meeting at one row over several heights so their ends don't stack. */
std::vector<qreal> attachOffsets(std::size_t count, qreal rowHeight)
{
    std::vector<qreal> offsets(count, 0.0);
    if (count < 2)
        return offsets;

    const qreal spacing = std::min(kMaxAttachSpacing, rowHeight * kRowAttachSpan / (count - 1));
    for (std::size_t i = 0; i < count; ++i)
        offsets[i] = (static_cast<qreal>(i) - (count - 1) / 2.0) * spacing;
    return offsets;
}

struct Attachment
{
    qreal producerY = 0;
    qreal consumerY = 0;
};

/** Vertical attach point of every arrow at both of its rows. */
std::vector<std::optional<Attachment>> attachPoints(const DependencyArrowLayout &layout,
                                                    const RowLookup &rowOf)
{
    // For each row: the arrows touching it, ordered by where their other end is.
    std::map<std::size_t, std::vector<std::pair<std::size_t, std::size_t>>> touching;
    for (std::size_t i = 0; i < layout.arrows.size(); ++i)
    {
        const DependencyArrow &arrow = layout.arrows[i];
        touching[arrow.producer].push_back({arrow.consumer, i});
        touching[arrow.consumer].push_back({arrow.producer, i});
    }

    std::vector<std::optional<Attachment>> points(layout.arrows.size(), Attachment{});
    for (auto &[instruction, arrows] : touching)
    {
        const auto row = rowOf(instruction);
        std::sort(arrows.begin(), arrows.end());
        const auto offsets = attachOffsets(arrows.size(), row ? row->height : 0.0);

        for (std::size_t k = 0; k < arrows.size(); ++k)
        {
            auto &point = points[arrows[k].second];
            if (!row)
            {
                point.reset();
                continue;
            }
            if (!point)
                continue;
            const DependencyArrow &arrow = layout.arrows[arrows[k].second];
            (arrow.producer == instruction ? point->producerY : point->consumerY) =
                row->centerY + offsets[k];
        }
    }
    return points;
}

void drawArrow(QPainter &painter, const DependencyArrow &arrow, qreal x, qreal edgeX,
               const Attachment &y)
{
    const qreal r = std::min(kCornerRadius, (y.consumerY - y.producerY) / 2);

    QPainterPath path(QPointF(edgeX, y.producerY));
    path.lineTo(x + r, y.producerY);
    path.quadTo(x, y.producerY, x, y.producerY + r);
    path.lineTo(x, y.consumerY - r);
    path.quadTo(x, y.consumerY, x + r, y.consumerY);
    path.lineTo(edgeX, y.consumerY);

    painter.setPen(QPen(arrow.color, kLineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);

    QPainterPath head(QPointF(edgeX + kArrowHeadSize, y.consumerY));
    head.lineTo(edgeX, y.consumerY - kArrowHeadSize * 0.7);
    head.lineTo(edgeX, y.consumerY + kArrowHeadSize * 0.7);
    head.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(arrow.color);
    painter.drawPath(head);
}

void drawLabels(QPainter &painter, const std::map<std::size_t, std::vector<Label>> &labels,
                const RowLookup &rowOf)
{
    QFont font = painter.font();
    font.setPointSizeF(font.pointSizeF() * kLabelFontScale);
    painter.setFont(font);
    const QFontMetricsF metrics(font);

    for (const auto &[instruction, rowLabels] : labels)
    {
        const auto row = rowOf(instruction);
        if (!row)
            continue;

        qreal x = row->textEndX + kLabelGap;
        for (const Label &label : rowLabels)
        {
            painter.setPen(label.color);
            painter.drawText(QPointF(x, row->centerY + (metrics.ascent() - metrics.descent()) / 2),
                             label.text);
            x += metrics.horizontalAdvance(label.text) + kLabelGap;
        }
    }
}

} // namespace

DependencyArrowLayout layoutDependencyArrows(
    const std::vector<optimizer::RegisterDependency> &dependencies)
{
    DependencyArrowLayout layout;
    for (const auto &dependency : dependencies)
    {
        DependencyArrow arrow;
        arrow.producer = dependency.producer;
        arrow.consumer = dependency.consumer;
        arrow.reg = QString::fromStdString(dependency.reg);
        arrow.color = dependencyColor(arrow.reg);
        layout.arrows.push_back(arrow);
    }

    // Longest spans first, so they take the outer lanes and shorter ones nest inside.
    std::sort(layout.arrows.begin(), layout.arrows.end(),
              [](const DependencyArrow &a, const DependencyArrow &b) {
                  const std::size_t spanA = a.consumer - a.producer;
                  const std::size_t spanB = b.consumer - b.producer;
                  if (spanA != spanB)
                      return spanA > spanB;
                  if (a.producer != b.producer)
                      return a.producer < b.producer;
                  return a.consumer < b.consumer;
              });

    for (std::size_t i = 0; i < layout.arrows.size(); ++i)
    {
        DependencyArrow &arrow = layout.arrows[i];

        int lane = 0;
        while (std::any_of(layout.arrows.begin(), layout.arrows.begin() + i,
                           [&](const DependencyArrow &placed) {
                               return placed.lane == lane && overlaps(placed, arrow);
                           }))
            ++lane;
        arrow.lane = lane;
        layout.laneCount = std::max(layout.laneCount, lane + 1);
    }
    return layout;
}

void paintDependencyArrows(QPainter &painter, const DependencyArrowLayout &layout,
                           const GutterMetrics &gutter, const RowLookup &rowOf,
                           std::size_t focus)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    const auto attachments = attachPoints(layout, rowOf);

    std::map<std::size_t, std::vector<Label>> labels;
    for (std::size_t i = 0; i < layout.arrows.size(); ++i)
    {
        const DependencyArrow &arrow = layout.arrows[i];
        if (!attachments[i])
            continue;

        drawArrow(painter, arrow, laneX(gutter, layout, arrow.lane), gutter.right - kArrowHeadSize,
                  *attachments[i]);

        // The clicked row needs no label: its arrows already point at the other rows.
        if (arrow.producer != focus)
            labels[arrow.producer].push_back(
                {QString("%1 → #%2").arg(arrow.reg).arg(arrow.consumer + 1), arrow.color});
        if (arrow.consumer != focus)
            labels[arrow.consumer].push_back(
                {QString("← %1 from #%2").arg(arrow.reg).arg(arrow.producer + 1), arrow.color});
    }

    drawLabels(painter, labels, rowOf);
    painter.restore();
}

} // namespace Kites
