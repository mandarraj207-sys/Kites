#include "ui/optimizer_tab/instruction_listing.h"

#include "ui/optimizer_tab/instruction_listing_highlighter.h"

#include <QColor>
#include <QList>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextFormat>
#include <QMouseEvent>

#include <map>
#include <vector>

namespace Kites {
namespace {

constexpr int kIndexWidth = 4;
constexpr int kOriginalIndexWidth = 5;
constexpr int kSourceLineWidth = 11;
constexpr int kStatusWidth = 10;
constexpr int kAssemblyWidth = 32;
constexpr int kHeaderLineCount = 2;
constexpr int kGutterWidth = 6; // blank characters in front of the assembly, used by the arrows

// Opacity (0-255) of the row bands behind the clicked instruction and its related rows.
constexpr int kSelectedRowAlpha = 45;
constexpr int kRelatedRowAlpha = 35;

// Producer box: border thickness in pixels and corner radius.
constexpr qreal kProducerBoxPenWidth = 1.5;
constexpr qreal kProducerBoxRadius = 3.0;

// Fixed-width columns for the monospace listing; widths are minimums, so
// large numbers widen their column instead of shifting the assembly column.
QString listingColumn(const QString &value, int width, bool rightAligned = false)
{
    return QString("%1").arg(value, rightAligned ? width : -width, QChar(' '));
}

QString numberColumn(qlonglong value, int width)
{
    return listingColumn(QString::number(value), width, true);
}

QString assemblyCell(const QString &text)
{
    return QString(kGutterWidth, ' ') + text;
}

QString separatorLine(const std::vector<int> &widths)
{
    QString line;
    for (std::size_t i = 0; i < widths.size(); ++i)
    {
        if (i > 0)
            line += "-+-";
        line += QString(widths[i], '-');
    }
    return line + "\n";
}

using LabelsByInstruction = std::map<std::size_t, std::vector<QString>>;

LabelsByInstruction labelsByInstruction(const AssembledProgram &program)
{
    LabelsByInstruction labels;
    for (const auto &[name, symbol] : program.symbol_table)
    {
        if (symbol.isData || (symbol.address % 4) != 0)
            continue;
        const auto instructionIndex = static_cast<std::size_t>(symbol.address / 4);
        if (instructionIndex <= program.intermediate_code.size())
            labels[instructionIndex].push_back(QString::fromStdString(name));
    }
    return labels;
}

void appendOriginalLabels(ListingView &view, const LabelsByInstruction &labels,
                          std::size_t instructionIndex, int &blockNumber,
                          const AssembledProgram &original)
{
    const auto found = labels.find(instructionIndex);
    if (found == labels.end())
        return;

    const bool hasTarget = instructionIndex < original.intermediate_code.size();
    const qlonglong sourceLine =
        hasTarget ? original.intermediate_code[instructionIndex].first.line_number : 0;

    for (const QString &name : found->second)
    {
        view.text += numberColumn(static_cast<qlonglong>(instructionIndex + 1),
                                  kIndexWidth) + " | " +
                     numberColumn(sourceLine, kSourceLineWidth) + " | " +
                     assemblyCell(QString("%1:").arg(name)) + "\n";
        ++blockNumber;
    }
}

void appendOptimizedLabels(ListingView &view, const LabelsByInstruction &labels,
                           std::size_t instructionIndex, int &blockNumber,
                           const AssembledProgram &optimized,
                           const std::vector<int> &originalPositions)
{
    const auto found = labels.find(instructionIndex);
    if (found == labels.end())
        return;

    const bool hasTarget = instructionIndex < optimized.intermediate_code.size();
    const qlonglong sourceLine =
        hasTarget ? optimized.intermediate_code[instructionIndex].first.line_number : 0;
    const int originalPosition =
        hasTarget && instructionIndex < originalPositions.size()
            ? originalPositions[instructionIndex]
            : -1;
    const QString status =
        !hasTarget ? "LABEL"
                   : (originalPosition >= 0 &&
                              originalPosition != static_cast<int>(instructionIndex)
                          ? "MOVED"
                          : "UNCHANGED");

    for (const QString &name : found->second)
    {
        const qlonglong optimizedLine = static_cast<qlonglong>(instructionIndex + 1);
        const qlonglong originalLine =
            originalPosition >= 0
                ? static_cast<qlonglong>(originalPosition + 1)
                : static_cast<qlonglong>(instructionIndex + 1);

        view.text += numberColumn(optimizedLine, kIndexWidth) + " | " +
                     numberColumn(originalLine, kOriginalIndexWidth) + " | " +
                     numberColumn(sourceLine, kSourceLineWidth) + " | " +
                     listingColumn(status, kStatusWidth) + " | " +
                     assemblyCell(QString("%1:").arg(name)) + "\n";
        ++blockNumber;
    }
}

} // namespace

std::string formatAssemblyInstruction(const ICUnit &instruction)
{
    const std::string op = instruction.opcode.data();
    const std::string rd = instruction.rd.data();
    const std::string rs1 = instruction.rs1.data();
    const std::string rs2 = instruction.rs2.data();
    const std::string rs3 = instruction.rs3.data();
    const std::string imm = instruction.imm.data();
    const auto present = [](const std::string &value) { return !value.empty(); };
    const auto join = [](const std::vector<std::string> &parts) {
        std::string result;
        for (const auto &part : parts) {
            if (!result.empty()) result += ", ";
            result += part;
        }
        return result;
    };

    if (op == "sw" || op == "sb" || op == "sh" || op == "sd" ||
        op == "fsw" || op == "fsd")
        return op + " " + rs2 + ", " + (present(imm) ? imm : "0") + "(" + rs1 + ")";

    if (op == "lb" || op == "lh" || op == "lw" || op == "ld" ||
        op == "lbu" || op == "lhu" || op == "lwu" || op == "flw" || op == "fld")
        return op + " " + rd + ", " + (present(imm) ? imm : "0") + "(" + rs1 + ")";

    if (op == "beq" || op == "bne" || op == "blt" || op == "bge" ||
        op == "bltu" || op == "bgeu")
        return op + " " + join({rs1, rs2, present(instruction.label) ? instruction.label : imm});

    if (op == "jal")
        return op + " " + join({present(rd) ? rd : "x1", present(instruction.label) ? instruction.label : imm});
    if (op == "jalr")
        return op + " " + rd + ", " + (present(imm) ? imm : "0") + "(" + rs1 + ")";

    if (op == "lui" || op == "auipc")
        return op + " " + join({rd, imm});

    if (present(rs3))
        return op + " " + join({rd, rs1, rs2, rs3});
    if (present(rs2))
        return op + " " + join({rd, rs1, rs2});
    if (present(rs1) && present(imm))
        return op + " " + join({rd, rs1, imm});
    if (present(rd) && present(imm))
        return op + " " + join({rd, imm});
    if (present(rd) && present(rs1))
        return op + " " + join({rd, rs1});
    if (present(rd))
        return op + " " + rd;
    if (present(rs1))
        return op + " " + rs1;
    return op;
}

ListingView buildOriginalListing(const AssembledProgram &original)
{
    ListingView view;
    view.text += listingColumn("#", kIndexWidth, true) + " | " +
                 listingColumn("SOURCE LINE", kSourceLineWidth) + " | " + assemblyCell("ASSEMBLY") + "\n";
    view.text += separatorLine({kIndexWidth, kSourceLineWidth, kGutterWidth + kAssemblyWidth});

    const auto labels = labelsByInstruction(original);
    int blockNumber = kHeaderLineCount;
    for (std::size_t i = 0; i < original.intermediate_code.size(); ++i)
    {
        appendOriginalLabels(view, labels, i, blockNumber, original);
        const ICUnit &instruction = original.intermediate_code[i].first;
        view.instructionBlocks.push_back(blockNumber);
        view.text += numberColumn(static_cast<qlonglong>(i + 1), kIndexWidth) + " | " +
                     numberColumn(instruction.line_number, kSourceLineWidth) + " | " +
                     assemblyCell(QString::fromStdString(formatAssemblyInstruction(instruction))) + "\n";
        view.sourceLineBlocks[static_cast<int>(instruction.line_number)].push_back(blockNumber);
        ++blockNumber;
    }
    appendOriginalLabels(view, labels, original.intermediate_code.size(), blockNumber, original);
    return view;
}

ListingView buildOptimizedListing(const AssembledProgram &optimized,
                                  const std::vector<int> &originalPositions)
{
    ListingView view;
    view.text += listingColumn("#", kIndexWidth, true) + " | " +
                 listingColumn("ORG #", kOriginalIndexWidth, true) + " | " +
                 listingColumn("SOURCE LINE", kSourceLineWidth) + " | " +
                 listingColumn("STATUS", kStatusWidth) + " | " + assemblyCell("ASSEMBLY") + "\n";
    view.text += separatorLine({kIndexWidth, kOriginalIndexWidth, kSourceLineWidth, kStatusWidth,
                                kGutterWidth + kAssemblyWidth});

    const auto labels = labelsByInstruction(optimized);
    int blockNumber = kHeaderLineCount;
    for (std::size_t i = 0; i < optimized.intermediate_code.size(); ++i)
    {
        appendOptimizedLabels(view, labels, i, blockNumber, optimized, originalPositions);
        const ICUnit &instruction = optimized.intermediate_code[i].first;
        view.instructionBlocks.push_back(blockNumber);

        const int originalPosition = i < originalPositions.size() ? originalPositions[i] : -1;
        const bool moved = originalPosition >= 0 && originalPosition != static_cast<int>(i);
        const QString status = moved ? "MOVED" : "UNCHANGED";

        view.text += numberColumn(static_cast<qlonglong>(i + 1), kIndexWidth) + " | " +
                     numberColumn(originalPosition >= 0 ? originalPosition + 1 : 0,
                                  kOriginalIndexWidth) + " | " +
                     numberColumn(instruction.line_number, kSourceLineWidth) + " | " +
                     listingColumn(status, kStatusWidth) + " | " +
                     assemblyCell(QString::fromStdString(formatAssemblyInstruction(instruction))) + "\n";
        view.sourceLineBlocks[static_cast<int>(instruction.line_number)].push_back(blockNumber);
        ++blockNumber;
    }
    appendOptimizedLabels(view, labels, optimized.intermediate_code.size(), blockNumber, optimized, originalPositions);
    return view;
}

InstructionListingTextEdit::InstructionListingTextEdit(QWidget *parent)
    : QPlainTextEdit(parent)
{
}

void InstructionListingTextEdit::setDependencyHighlighter(
    const InstructionListingHighlighter *highlighter)
{
    m_dependencyHighlighter = highlighter;
    viewport()->update();
}

void InstructionListingTextEdit::setDependencies(
    std::vector<optimizer::RegisterDependency> dependencies, std::vector<int> instructionBlocks)
{
    m_dependencies = std::move(dependencies);
    m_instructionBlocks = std::move(instructionBlocks);

    m_instructionForBlock.clear();
    for (std::size_t i = 0; i < m_instructionBlocks.size(); ++i)
        m_instructionForBlock[m_instructionBlocks[i]] = i;

    selectInstruction(std::nullopt);
}

void InstructionListingTextEdit::selectInstruction(std::optional<std::size_t> instruction)
{
    m_selected = instruction;
    m_arrowLayout = instruction
                        ? layoutDependencyArrows(optimizer::dependenciesOf(m_dependencies, *instruction))
                        : DependencyArrowLayout{};
    viewport()->update();
}

void InstructionListingTextEdit::mousePressEvent(QMouseEvent *event)
{
    QPlainTextEdit::mousePressEvent(event);
    if (event->button() != Qt::LeftButton)
        return;

    const int blockNumber = cursorForPosition(event->pos()).block().blockNumber();
    const auto clicked = m_instructionForBlock.find(blockNumber);
    if (clicked == m_instructionForBlock.end() || m_selected == clicked->second)
        selectInstruction(std::nullopt);
    else
        selectInstruction(clicked->second);
}

QRectF InstructionListingTextEdit::rowRect(std::size_t instruction) const
{
    const QTextBlock block = document()->findBlockByNumber(m_instructionBlocks[instruction]);
    const QRectF geometry = blockBoundingGeometry(block).translated(contentOffset());
    return QRectF(0, geometry.top(), viewport()->width(), geometry.height());
}

std::optional<RowMetrics> InstructionListingTextEdit::rowMetrics(std::size_t instruction) const
{
    if (instruction >= m_instructionBlocks.size())
        return std::nullopt;

    const QTextBlock block = document()->findBlockByNumber(m_instructionBlocks[instruction]);
    if (!block.isValid())
        return std::nullopt;

    QTextCursor end(block);
    end.movePosition(QTextCursor::EndOfBlock);
    const QRectF row = rowRect(instruction);
    return RowMetrics{row.center().y(), row.height(), static_cast<qreal>(cursorRect(end).left())};
}

void InstructionListingTextEdit::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);

    QPainter painter(viewport());
    paintProducerBoxes(painter, event->rect());
    paintDependencySelection(painter);
}

void InstructionListingTextEdit::paintProducerBoxes(QPainter &painter, const QRect &dirtyRect)
{
    if (!m_dependencyHighlighter)
        return;

    // QTextCharFormat cannot draw a border around a run of text, so the
    // unfilled producer boxes are painted on top of the finished text.
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);

    const int viewportBottom = dirtyRect.bottom();
    const int viewportTop = dirtyRect.top();
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next())
    {
        const QRectF geometry = blockBoundingGeometry(block).translated(contentOffset());
        if (geometry.top() > viewportBottom)
            break;
        if (!block.isVisible() || geometry.bottom() < viewportTop)
            continue;

        for (const DependencySpan &span :
             m_dependencyHighlighter->dependencySpans(block.text(), block.blockNumber()))
        {
            if (!span.write)
                continue;

            QTextCursor cursor(document());
            cursor.setPosition(block.position() + span.start);
            const QRect first = cursorRect(cursor);
            cursor.setPosition(block.position() + span.start + span.length);
            const QRect last = cursorRect(cursor);

            const QRectF box(first.left(), first.top(), last.left() - first.left(),
                             first.height());
            painter.setPen(QPen(span.color, kProducerBoxPenWidth));
            painter.drawRoundedRect(box.adjusted(-1.0, 1.0, 0.0, -1.0), kProducerBoxRadius,
                                    kProducerBoxRadius);
        }
    }
}

void InstructionListingTextEdit::paintDependencySelection(QPainter &painter)
{
    if (!m_selected || *m_selected >= m_instructionBlocks.size())
        return;

    QColor selectedTint = palette().color(QPalette::Highlight);
    selectedTint.setAlpha(kSelectedRowAlpha);
    painter.fillRect(rowRect(*m_selected), selectedTint);

    for (const DependencyArrow &arrow : m_arrowLayout.arrows)
    {
        QColor tint = arrow.color;
        tint.setAlpha(kRelatedRowAlpha);
        const std::size_t other = arrow.producer == *m_selected ? arrow.consumer : arrow.producer;
        if (other < m_instructionBlocks.size())
            painter.fillRect(rowRect(other), tint);
    }

    const QTextBlock block = document()->findBlockByNumber(m_instructionBlocks[*m_selected]);
    const int assemblyStart = block.text().lastIndexOf(" | ") + 3;
    QTextCursor cursor(block);
    cursor.setPosition(block.position() + assemblyStart);
    const qreal left = cursorRect(cursor).left();
    cursor.setPosition(block.position() + assemblyStart + kGutterWidth);
    const qreal right = cursorRect(cursor).left();

    paintDependencyArrows(painter, m_arrowLayout, {left, right},
                          [this](std::size_t instruction) { return rowMetrics(instruction); },
                          *m_selected);
}

void highlightExecutionLines(QPlainTextEdit *view,
                             const std::map<int, std::vector<int>> &sourceLineBlocks,
                             const std::vector<std::pair<int, std::string>> &editorLines)
{
    QList<QTextEdit::ExtraSelection> selections;
    for (const auto &[sourceLine, stage] : editorLines)
    {
        const auto found = sourceLineBlocks.find(sourceLine);
        if (found == sourceLineBlocks.end())
            continue;

        for (const int blockNumber : found->second)
        {
            QTextBlock block = view->document()->findBlockByNumber(blockNumber);
            if (!block.isValid())
                continue;

            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(block);
            selection.cursor.select(QTextCursor::LineUnderCursor);
            selection.format.setBackground(QColor(235, 190, 65, 75));
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.format.setToolTip(
                QString("Pipeline stage: %1").arg(QString::fromStdString(stage)));
            selections.push_back(selection);
        }
    }
    view->setExtraSelections(selections);
}

} // namespace Kites
