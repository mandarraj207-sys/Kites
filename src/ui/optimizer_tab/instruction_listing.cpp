#include "ui/optimizer_tab/instruction_listing.h"

#include "ui/optimizer_tab/instruction_listing_highlighter.h"

#include <QBrush>
#include <QColor>
#include <QEvent>
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
#include <QPalette>
#include <QStringList>

#include <algorithm>
#include <functional>
#include <map>
#include <vector>

namespace Kites {
namespace {

constexpr int kIndexWidth = 4;
constexpr int kAssemblyWidth = 32;
constexpr int kHeaderLineCount = 2;
constexpr int kGutterWidth = 6; // blank characters in front of the assembly, used by the arrows

// Opacity (0-255) of the row bands behind the clicked instruction and its related rows.
constexpr int kSelectedRowAlpha = 45;
constexpr int kRelatedRowAlpha = 35;

// Producer box: border thickness in pixels and corner radius.
constexpr qreal kProducerBoxPenWidth = 1.5;
constexpr qreal kProducerBoxRadius = 3.0;

// Rows the scheduler moved: a faint band plus a solid bar on the left edge.
// Green = the instruction now sits earlier, amber = it now sits later.
constexpr int kMovedRowAlpha = 34;
constexpr qreal kMovedBarWidth = 3.0;
const QColor kMovedEarlierColor(46, 160, 120);
const QColor kMovedLaterColor(224, 150, 40);

// Stall rows (pipeline bubbles): a red hatched band plus a solid bar on the left edge.
constexpr int kStallRowAlpha = 26;
constexpr int kStallHatchAlpha = 70;
const QColor kStallColor(224, 82, 82);

// Colours of the dependency-type chips in the expanded details.
const QColor kRawColor(224, 82, 82);
const QColor kWarColor(66, 133, 212);
const QColor kWawColor(151, 112, 205);

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

void appendLabels(ListingView &view, const LabelsByInstruction &labels,
                  std::size_t instructionIndex, int &blockNumber)
{
    const auto found = labels.find(instructionIndex);
    if (found == labels.end())
        return;

    for (const QString &name : found->second)
    {
        view.text += numberColumn(static_cast<qlonglong>(instructionIndex + 1), kIndexWidth) +
                     " | " + assemblyCell(QString("%1:").arg(name)) + "\n";
        ++blockNumber;
    }
}

// One row per bubble cycle in front of an instruction. The first row says what the
// instruction waits for: the register and the number the producer has in the "#" column.
void appendStallRows(ListingView &view, const optimizer::StallInfo &stall, int &blockNumber)
{
    for (std::size_t k = 0; k < stall.cycles; ++k)
    {
        QString text = kStallRowMarker;
        if (stall.cycles > 1)
            text += QString(" %1/%2")
                        .arg(static_cast<qulonglong>(k + 1))
                        .arg(static_cast<qulonglong>(stall.cycles));
        if (k == 0)
            text += QString("  (%1 from #%2)")
                        .arg(QString::fromStdString(stall.reg))
                        .arg(static_cast<qulonglong>(stall.producer + 1));

        view.text += listingColumn(QString(), kIndexWidth) + " | " + assemblyCell(text) + "\n";
        view.stallBlocks.push_back(blockNumber);
        ++blockNumber;
    }
}

QString typeChip(const QString &type)
{
    const QColor &color = type == "RAW" ? kRawColor : (type == "WAR" ? kWarColor : kWawColor);
    return QString("<b><span style='color:%1'>%2</span></b>").arg(color.name(), type);
}

// A dependency between two instructions can be several hazards at once
// (e.g. "addi x6, x6, 1" after "add x6, ..." is both RAW and WAW).
QStringList hazardTypes(const InstructionDetail &producer, const InstructionDetail &consumer,
                        const std::string &reg)
{
    const auto has = [&reg](const std::vector<std::string> &list) {
        return std::find(list.begin(), list.end(), reg) != list.end();
    };

    QStringList types;
    if (has(producer.writes) && has(consumer.reads))
        types << "RAW";
    if (has(producer.reads) && has(consumer.writes))
        types << "WAR";
    if (has(producer.writes) && has(consumer.writes))
        types << "WAW";
    return types;
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

ListingView buildOriginalListing(const AssembledProgram &original,
                                 const std::vector<optimizer::StallInfo> *stalls)
{
    ListingView view;
    view.text += listingColumn("#", kIndexWidth, true) + " | " + assemblyCell("ASSEMBLY") + "\n";
    view.text += separatorLine({kIndexWidth, kGutterWidth + kAssemblyWidth});

    const auto labels = labelsByInstruction(original);
    int blockNumber = kHeaderLineCount;
    for (std::size_t i = 0; i < original.intermediate_code.size(); ++i)
    {
        appendLabels(view, labels, i, blockNumber);
        if (stalls && i < stalls->size())
            appendStallRows(view, (*stalls)[i], blockNumber);
        const ICUnit &instruction = original.intermediate_code[i].first;
        view.instructionBlocks.push_back(blockNumber);
        view.text += numberColumn(static_cast<qlonglong>(i + 1), kIndexWidth) + " | " +
                     assemblyCell(QString::fromStdString(formatAssemblyInstruction(instruction))) + "\n";
        view.sourceLineBlocks[static_cast<int>(instruction.line_number)].push_back(blockNumber);
        ++blockNumber;
    }
    appendLabels(view, labels, original.intermediate_code.size(), blockNumber);
    return view;
}

// The original position and moved/unchanged state are no longer printed as
// columns: they are shown as row colours and in the expanded details instead.
ListingView buildOptimizedListing(const AssembledProgram &optimized,
                                  const std::vector<int> &originalPositions,
                                  const std::vector<optimizer::StallInfo> *stalls)
{
    Q_UNUSED(originalPositions);

    ListingView view;
    view.text += listingColumn("#", kIndexWidth, true) + " | " + assemblyCell("ASSEMBLY") + "\n";
    view.text += separatorLine({kIndexWidth, kGutterWidth + kAssemblyWidth});

    const auto labels = labelsByInstruction(optimized);
    int blockNumber = kHeaderLineCount;
    for (std::size_t i = 0; i < optimized.intermediate_code.size(); ++i)
    {
        appendLabels(view, labels, i, blockNumber);
        if (stalls && i < stalls->size())
            appendStallRows(view, (*stalls)[i], blockNumber);
        const ICUnit &instruction = optimized.intermediate_code[i].first;
        view.instructionBlocks.push_back(blockNumber);
        view.text += numberColumn(static_cast<qlonglong>(i + 1), kIndexWidth) + " | " +
                     assemblyCell(QString::fromStdString(formatAssemblyInstruction(instruction))) + "\n";
        view.sourceLineBlocks[static_cast<int>(instruction.line_number)].push_back(blockNumber);
        ++blockNumber;
    }
    appendLabels(view, labels, optimized.intermediate_code.size(), blockNumber);
    return view;
}

InstructionListingTextEdit::InstructionListingTextEdit(QWidget *parent)
    : QPlainTextEdit(parent)
{
    // Mouse-move events are only delivered without a pressed button when the viewport tracks.
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
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

    m_hovered.reset();
    selectInstruction(std::nullopt);
}

void InstructionListingTextEdit::selectInstruction(std::optional<std::size_t> instruction)
{
    m_selected = instruction;
    m_arrowLayout = instruction
                        ? layoutDependencyArrows(optimizer::dependenciesOf(m_dependencies, *instruction))
                        : DependencyArrowLayout{};
    viewport()->update();

    if (m_selectionChanged)
        m_selectionChanged(instruction ? detailsHtml(*instruction) : QString());
}

void InstructionListingTextEdit::setInstructionDetails(std::vector<InstructionDetail> details)
{
    m_details = std::move(details);
    viewport()->update();
}

void InstructionListingTextEdit::setStallBlocks(std::vector<int> blocks)
{
    m_stallBlocks = std::move(blocks);
    viewport()->update();
}

void InstructionListingTextEdit::setSelectionChangedCallback(
    std::function<void(const QString &)> callback)
{
    m_selectionChanged = std::move(callback);
}

void InstructionListingTextEdit::setHoverChangedCallback(
    std::function<void(std::optional<std::size_t>)> callback)
{
    m_hoverChanged = std::move(callback);
}

void InstructionListingTextEdit::updateHover(std::optional<std::size_t> instruction)
{
    if (m_hovered == instruction)
        return;
    m_hovered = instruction;
    if (m_hoverChanged)
        m_hoverChanged(instruction);
}

void InstructionListingTextEdit::mouseMoveEvent(QMouseEvent *event)
{
    QPlainTextEdit::mouseMoveEvent(event);

    // cursorForPosition snaps to the nearest line, so check the mouse is really on the row.
    std::optional<std::size_t> hovered;
    const auto found =
        m_instructionForBlock.find(cursorForPosition(event->pos()).block().blockNumber());
    if (found != m_instructionForBlock.end() && found->second < m_instructionBlocks.size())
    {
        const QRectF row = rowRect(found->second);
        if (event->pos().y() >= row.top() && event->pos().y() <= row.bottom())
            hovered = found->second;
    }
    updateHover(hovered);
}

// The mouse leaving the viewport is reported to the viewport, not to the text edit itself.
bool InstructionListingTextEdit::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Leave)
        updateHover(std::nullopt);
    return QPlainTextEdit::viewportEvent(event);
}

std::optional<InstructionGeometry> InstructionListingTextEdit::instructionGeometry(
    std::size_t instruction) const
{
    const auto metrics = rowMetrics(instruction);
    if (!metrics)
        return std::nullopt;
    return InstructionGeometry{rowRect(instruction), metrics->textEndX};
}

// Rich text for the expandable panel under the listing: what the clicked
// instruction depends on (or feeds), the hazard type, and the register behind it.
QString InstructionListingTextEdit::detailsHtml(std::size_t instruction) const
{
    if (instruction >= m_details.size())
        return {};

    const QString muted = palette().color(QPalette::Disabled, QPalette::Text).name();
    const InstructionDetail &self = m_details[instruction];

    QString movement;
    if (self.movement < 0)
        movement = QString(" &middot; <span style='color:%1'>moved earlier</span>")
                       .arg(kMovedEarlierColor.name());
    else if (self.movement > 0)
        movement = QString(" &middot; <span style='color:%1'>moved later</span>")
                       .arg(kMovedLaterColor.name());

    QString html = QString("<b>#%1</b>&nbsp;&nbsp;%2<br>"
                           "<span style='color:%3'>Original #%4 &middot; Source line %5</span>%6")
                       .arg(instruction + 1)
                       .arg(self.assembly.toHtmlEscaped(), muted)
                       .arg(self.originalIndex)
                       .arg(self.sourceLine)
                       .arg(movement);

    const auto dependencies = optimizer::dependenciesOf(m_dependencies, instruction);
    if (dependencies.empty())
        return html + "<p>No register dependencies for this instruction.</p>";

    html += "<table cellspacing='0' cellpadding='4' width='100%'>"
            "<tr><th align='left'>Type</th><th align='left'>Register</th>"
            "<th align='left'>Link</th><th align='left'>Instruction</th>"
            "<th align='left'>Original</th></tr>";

    for (const auto &dependency : dependencies)
    {
        if (dependency.producer >= m_details.size() || dependency.consumer >= m_details.size())
            continue;

        const bool isProducer = dependency.producer == instruction;
        const std::size_t other = isProducer ? dependency.consumer : dependency.producer;
        const InstructionDetail &otherDetail = m_details[other];

        const QStringList types = hazardTypes(m_details[dependency.producer],
                                              m_details[dependency.consumer], dependency.reg);
        QStringList chips;
        for (const QString &type : types)
            chips << typeChip(type);

        const QString reg = QString::fromStdString(dependency.reg);
        html += QString("<tr><td>%1</td>"
                        "<td><b><span style='color:%2'>%3</span></b></td>"
                        "<td>%4 #%5</td><td>%6</td><td>#%7 &middot; line %8</td></tr>")
                    .arg(chips.isEmpty() ? QString("Register") : chips.join(" + "))
                    .arg(dependencyColor(reg).name(), reg.toHtmlEscaped())
                    .arg(isProducer ? QString("&rarr;") : QString("&larr;"))
                    .arg(other + 1)
                    .arg(otherDetail.assembly.toHtmlEscaped())
                    .arg(otherDetail.originalIndex)
                    .arg(otherDetail.sourceLine);
    }
    return html + "</table>";
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
    paintMovedRows(painter, event->rect());
    paintStallRows(painter, event->rect());
    paintProducerBoxes(painter, event->rect());
    paintDependencySelection(painter);
}

void InstructionListingTextEdit::paintMovedRows(QPainter &painter, const QRect &dirtyRect)
{
    const std::size_t count = std::min(m_details.size(), m_instructionBlocks.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        const int movement = m_details[i].movement;
        if (movement == 0)
            continue;
        if (!document()->findBlockByNumber(m_instructionBlocks[i]).isValid())
            continue;

        const QRectF row = rowRect(i);
        if (row.bottom() < dirtyRect.top() || row.top() > dirtyRect.bottom())
            continue;

        const QColor &color = movement < 0 ? kMovedEarlierColor : kMovedLaterColor;
        QColor tint = color;
        tint.setAlpha(kMovedRowAlpha);
        painter.fillRect(row, tint);
        painter.fillRect(QRectF(row.left(), row.top(), kMovedBarWidth, row.height()), color);
    }
}

void InstructionListingTextEdit::paintStallRows(QPainter &painter, const QRect &dirtyRect)
{
    for (const int blockNumber : m_stallBlocks)
    {
        const QTextBlock block = document()->findBlockByNumber(blockNumber);
        if (!block.isValid())
            continue;

        const QRectF geometry = blockBoundingGeometry(block).translated(contentOffset());
        if (geometry.bottom() < dirtyRect.top() || geometry.top() > dirtyRect.bottom())
            continue;

        const QRectF row(0, geometry.top(), viewport()->width(), geometry.height());
        QColor tint = kStallColor;
        tint.setAlpha(kStallRowAlpha);
        painter.fillRect(row, tint);

        QColor hatch = kStallColor;
        hatch.setAlpha(kStallHatchAlpha);
        painter.fillRect(row, QBrush(hatch, Qt::BDiagPattern));
        painter.fillRect(QRectF(row.left(), row.top(), kMovedBarWidth, row.height()), kStallColor);
    }
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
