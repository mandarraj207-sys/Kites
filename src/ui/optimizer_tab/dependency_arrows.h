#pragma once

#include "optimizer/dependency_graph.h"

#include <QColor>
#include <QString>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

class QPainter;

namespace Kites {

struct DependencyArrow
{
    std::size_t producer = 0;
    std::size_t consumer = 0;
    QString reg;
    QColor color;
    int lane = 0; // 0 is the outermost (leftmost) lane
};

struct DependencyArrowLayout
{
    std::vector<DependencyArrow> arrows;
    int laneCount = 0;
};

/** Colors each arrow like its register's underline and gives it a lane so overlapping arrows don't collide. */
DependencyArrowLayout layoutDependencyArrows(
    const std::vector<optimizer::RegisterDependency> &dependencies);

struct RowMetrics
{
    qreal centerY = 0;
    qreal height = 0;
    qreal textEndX = 0;
};

/** Horizontal extent, in viewport pixels, of the blank gutter in front of the assembly column. */
struct GutterMetrics
{
    qreal left = 0;
    qreal right = 0;
};

using RowLookup = std::function<std::optional<RowMetrics>(std::size_t instruction)>;

/** Draws the arrows; `focus` is the clicked instruction, whose own row gets no labels. */
void paintDependencyArrows(QPainter &painter, const DependencyArrowLayout &layout,
                           const GutterMetrics &gutter, const RowLookup &rowOf,
                           std::size_t focus);

} // namespace Kites
