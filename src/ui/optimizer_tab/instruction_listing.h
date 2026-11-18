#pragma once

#include "common/assembled_program.h"
#include "optimizer/dependency_graph.h"
#include "ui/optimizer_tab/dependency_arrows.h"

#include <QPlainTextEdit>
#include <QString>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Kites {

class InstructionListingHighlighter;

/** Listing text plus the text-block numbers of its rows. */
struct ListingView
{
    QString text;
    std::map<int, std::vector<int>> sourceLineBlocks;
    std::vector<int> instructionBlocks;
};

/**
 * Read-only listing. Clicking an instruction row draws arrows to the
 * instructions that consume its results and from the ones it consumes.
 */
class InstructionListingTextEdit : public QPlainTextEdit
{
public:
    explicit InstructionListingTextEdit(QWidget *parent = nullptr);

    void setDependencyHighlighter(const InstructionListingHighlighter *highlighter);

    /** `instructionBlocks[i]` is the text block showing instruction `i`; clears any selection. */
    void setDependencies(std::vector<optimizer::RegisterDependency> dependencies,
                         std::vector<int> instructionBlocks);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void paintProducerBoxes(QPainter &painter, const QRect &dirtyRect);
    void paintDependencySelection(QPainter &painter);
    void selectInstruction(std::optional<std::size_t> instruction);
    std::optional<RowMetrics> rowMetrics(std::size_t instruction) const;
    QRectF rowRect(std::size_t instruction) const;

    const InstructionListingHighlighter *m_dependencyHighlighter = nullptr;
    std::vector<optimizer::RegisterDependency> m_dependencies;
    std::vector<int> m_instructionBlocks;
    std::map<int, std::size_t> m_instructionForBlock;
    std::optional<std::size_t> m_selected;
    DependencyArrowLayout m_arrowLayout;
};

std::string formatAssemblyInstruction(const ICUnit &instruction);

ListingView buildOriginalListing(const AssembledProgram &original);

ListingView buildOptimizedListing(const AssembledProgram &optimized,
                                  const std::vector<int> &originalPositions);

void highlightExecutionLines(QPlainTextEdit *view,
                             const std::map<int, std::vector<int>> &sourceLineBlocks,
                             const std::vector<std::pair<int, std::string>> &editorLines);

} // namespace Kites
