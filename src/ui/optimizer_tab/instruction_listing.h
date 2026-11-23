#pragma once

// RECONSTRUCTED from instruction_listing.cpp (the original header was not uploaded).
// Diff it against your real header and keep your own include paths / any extra members.

#include "ui/optimizer_tab/dependency_arrows.h" // DependencyArrowLayout, RowMetrics, RowLookup
#include "optimizer/stall_model.h"                // optimizer::StallInfo

// TODO: use your project's real includes for these types:
//   AssembledProgram, ICUnit, optimizer::RegisterDependency / dependenciesOf
// #include "optimizer/dependency_graph.h"
// #include "optimizer/program_optimizer.h"

#include <QPlainTextEdit>
#include <QRect>
#include <QRectF>
#include <QString>

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;

namespace Kites {

class InstructionListingHighlighter;

struct ListingView
{
    QString text;
    std::vector<int> instructionBlocks;                 // text block number of each instruction
    std::map<int, std::vector<int>> sourceLineBlocks;   // editor line -> text blocks
    std::vector<int> stallBlocks;                       // text block number of every stall row
};

// NEW: everything the expandable details panel needs about one instruction.
struct InstructionDetail
{
    qlonglong originalIndex = 0;      // 1-based position in the original program
    qlonglong sourceLine = 0;         // editor line the instruction came from
    int movement = 0;                 // -1 moved earlier, 0 unchanged, +1 moved later
    QString assembly;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
};

std::string formatAssemblyInstruction(const ICUnit &instruction);

// `stalls` (one entry per instruction, see optimizer::computeStalls) is optional: when
// given, a "-- stall" row is inserted for every bubble cycle in front of an instruction.
ListingView buildOriginalListing(const AssembledProgram &original,
                                 const std::vector<optimizer::StallInfo> *stalls = nullptr);
ListingView buildOptimizedListing(const AssembledProgram &optimized,
                                  const std::vector<int> &originalPositions,
                                  const std::vector<optimizer::StallInfo> *stalls = nullptr);

void highlightExecutionLines(QPlainTextEdit *view,
                             const std::map<int, std::vector<int>> &sourceLineBlocks,
                             const std::vector<std::pair<int, std::string>> &editorLines);

// Where an instruction's row sits, in viewport coordinates.
struct InstructionGeometry
{
    QRectF row;          // full-width band of the row
    qreal textEndX = 0;  // x just after the last character of the row
};

class InstructionListingTextEdit : public QPlainTextEdit
{
public:
    explicit InstructionListingTextEdit(QWidget *parent = nullptr);

    void setDependencyHighlighter(const InstructionListingHighlighter *highlighter);
    void setDependencies(std::vector<optimizer::RegisterDependency> dependencies,
                         std::vector<int> instructionBlocks);

    // NEW
    void setInstructionDetails(std::vector<InstructionDetail> details);
    void setSelectionChangedCallback(std::function<void(const QString &)> callback);
    void setStallBlocks(std::vector<int> blocks); // NEW: rows to paint as pipeline bubbles

    // NEW: hover support for the movement link. The callback gets the instruction under the
    // mouse, or nullopt when the mouse is not over an instruction row.
    void setHoverChangedCallback(std::function<void(std::optional<std::size_t>)> callback);
    std::optional<InstructionGeometry> instructionGeometry(std::size_t instruction) const;

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void selectInstruction(std::optional<std::size_t> instruction);
    void updateHover(std::optional<std::size_t> instruction);
    QRectF rowRect(std::size_t instruction) const;
    std::optional<RowMetrics> rowMetrics(std::size_t instruction) const;

    void paintProducerBoxes(QPainter &painter, const QRect &dirtyRect);
    void paintDependencySelection(QPainter &painter);

    // NEW
    void paintMovedRows(QPainter &painter, const QRect &dirtyRect);
    void paintStallRows(QPainter &painter, const QRect &dirtyRect);
    QString detailsHtml(std::size_t instruction) const;

    const InstructionListingHighlighter *m_dependencyHighlighter = nullptr;
    std::vector<optimizer::RegisterDependency> m_dependencies;
    std::vector<int> m_instructionBlocks;
    std::map<int, std::size_t> m_instructionForBlock;
    std::optional<std::size_t> m_selected;
    DependencyArrowLayout m_arrowLayout;

    // NEW
    std::vector<InstructionDetail> m_details;
    std::vector<int> m_stallBlocks;
    std::function<void(const QString &)> m_selectionChanged;
    std::function<void(std::optional<std::size_t>)> m_hoverChanged;
    std::optional<std::size_t> m_hovered;
};

} // namespace Kites
