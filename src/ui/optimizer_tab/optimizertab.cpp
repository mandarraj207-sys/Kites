
#include "optimizertab.h"

#include "assembler/assembler.h"
#include "custom_pseudo_manager/custom_pseudo_manager.h"
#include "optimizer/forwarding_instruction_scheduler.h"
#include "optimizer/instruction_scheduler.h"
#include "optimizer/scheduler_config.h"
#include "ui/editor_tab/editortab.h"

#include "utils/utils.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTableWidget>
#include <QTextCharFormat>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

class InstructionListingHighlighter : public QSyntaxHighlighter
{
  public:
    explicit InstructionListingHighlighter(QTextDocument *document) : QSyntaxHighlighter(document)
    {
    }

  protected:
    void highlightBlock(const QString &text) override
    {
        // Subtle green background makes reordered instructions stand out.
        if (text.contains("| MOVED |"))
        {
            QTextCharFormat moved;
            moved.setBackground(QColor(40, 150, 80, 55));
            setFormat(0, text.size(), moved);
        }

        QTextCharFormat opcodeFormat;
        opcodeFormat.setForeground(QColor("#569CD6"));
        opcodeFormat.setFontWeight(QFont::DemiBold);

        QTextCharFormat registerFormat;
        registerFormat.setForeground(QColor("#4EC9B0"));

        QTextCharFormat immediateFormat;
        immediateFormat.setForeground(QColor("#CE9178"));

        static const QRegularExpression opcodeRegex(
            R"(\b(addi|add|sub|mul|and|or|xor|sll|srl|sra|slt|sltu|"
            R"(andi|ori|xori|slti|sltiu|lb|lh|lw|ld|lbu|lhu|lwu|"
            R"(sb|sh|sw|sd|lui|auipc|nop|mv|li|la|"
            R"(csrrw|csrrs|csrrc|csrrwi|csrrsi|csrrci)\b)",
            QRegularExpression::CaseInsensitiveOption);

        static const QRegularExpression registerRegex(
            R"(\b(?:x(?:[0-9]|[12][0-9]|3[01])|zero|ra|sp|gp|tp|"
            R"(t[0-6]|s(?:[0-9]|1[01])|a[0-7])\b)",
            QRegularExpression::CaseInsensitiveOption);

        static const QRegularExpression numberRegex(R"(\b-?(?:0x[0-9a-fA-F]+|[0-9]+)\b)");

        auto applyRegex = [&](const QRegularExpression &regex, const QTextCharFormat &format)
        {
            auto match = regex.globalMatch(text);
            while (match.hasNext())
            {
                const auto item = match.next();
                setFormat(item.capturedStart(), item.capturedLength(), format);
            }
        };

        applyRegex(opcodeRegex, opcodeFormat);
        applyRegex(registerRegex, registerFormat);
        applyRegex(numberRegex, immediateFormat);
    }
};

} // namespace

namespace Kites
{

static void printInstructions(const std::vector<ICUnit> &instructions, const char *title)
{
    qDebug().noquote() << "\n====" << title << "====";

    for (std::size_t i = 0; i < instructions.size(); ++i)
    {
        std::ostringstream out;
        out << instructions[i];

        qDebug().noquote() << QString("[%1] %2")
                                  .arg(static_cast<qulonglong>(i))
                                  .arg(QString::fromStdString(out.str()));
    }
}

OptimizerTab::OptimizerTab(EditorTab *editor, QWidget *parent) : KitesTab(parent), m_editor(editor)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    auto *heading = new QLabel("Instruction Scheduler", this);
    QFont font = heading->font();
    font.setPointSize(18);
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);

    auto *form = new QFormLayout();

    m_windowSizeSpinBox = new QSpinBox(this);
    m_windowSizeSpinBox->setRange(3, 64);
    m_windowSizeSpinBox->setValue(static_cast<int>(optimizer::g_scheduler_window_size));

    form->addRow("Scheduling window:", m_windowSizeSpinBox);

    // Keep the existing scheduler as the default baseline. The forwarding
    // scheduler can be selected independently for side-by-side experiments.
    m_hazardModelComboBox = new QComboBox(this);
    m_hazardModelComboBox->addItem("No forwarding (baseline)", false);
    m_hazardModelComboBox->addItem("With forwarding (H_F)", true);
    m_hazardModelComboBox->setCurrentIndex(0);
    form->addRow("Hazard model:", m_hazardModelComboBox);

    layout->addLayout(form);

    m_optimizeButton = new QPushButton("Generate Suggestions", this);
    m_loadButton = new QPushButton("Execute Optimized Program", this);
    m_benchmarkButton = new QPushButton("Benchmark Original vs Optimized", this);

    m_loadButton->setEnabled(false);
    m_benchmarkButton->setEnabled(false);

    layout->addWidget(m_optimizeButton);
    layout->addWidget(m_loadButton);
    layout->addWidget(m_benchmarkButton);

    m_statusLabel =
        new QLabel("Ready. Generate suggestions without changing the original source.", this);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    auto *listingSplitter = new QSplitter(Qt::Horizontal, this);

    auto *originalPanel = new QWidget(listingSplitter);
    auto *originalLayout = new QVBoxLayout(originalPanel);
    originalLayout->setContentsMargins(0, 0, 0, 0);

    auto *originalHeading = new QLabel("Original instructions", originalPanel);
    originalLayout->addWidget(originalHeading);

    m_originalOutput = new QPlainTextEdit(originalPanel);
    m_originalOutput->setReadOnly(true);
    m_originalOutput->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_originalOutput->setPlaceholderText("Original instruction listing appears here.");
    originalLayout->addWidget(m_originalOutput);

    auto *optimizedPanel = new QWidget(listingSplitter);
    auto *optimizedLayout = new QVBoxLayout(optimizedPanel);
    optimizedLayout->setContentsMargins(0, 0, 0, 0);

    auto *optimizedHeading = new QLabel("Optimized instructions", optimizedPanel);
    optimizedLayout->addWidget(optimizedHeading);

    m_optimizedOutput = new QPlainTextEdit(optimizedPanel);
    m_optimizedOutput->setReadOnly(true);
    m_optimizedOutput->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_optimizedOutput->setPlaceholderText("Optimized instruction listing appears here.");
    optimizedLayout->addWidget(m_optimizedOutput);

    const QFont listingFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_originalOutput->setFont(listingFont);
    m_optimizedOutput->setFont(listingFont);

    m_originalOutput->setStyleSheet("QPlainTextEdit { padding: 8px; }");
    m_optimizedOutput->setStyleSheet("QPlainTextEdit { padding: 8px; }");

    // Keep the highlighters alive for the lifetime of their text documents.
    new InstructionListingHighlighter(m_originalOutput->document());
    new InstructionListingHighlighter(m_optimizedOutput->document());

    listingSplitter->addWidget(originalPanel);
    listingSplitter->addWidget(optimizedPanel);
    listingSplitter->setStretchFactor(0, 1);
    listingSplitter->setStretchFactor(1, 1);
    layout->addWidget(listingSplitter, 1);

    connect(m_windowSizeSpinBox, qOverload<int>(&QSpinBox::valueChanged), this, [](int value)
            { optimizer::g_scheduler_window_size = static_cast<std::size_t>(value); });

    connect(m_optimizeButton, &QPushButton::clicked, this, &OptimizerTab::optimizeCurrentSource);

    connect(m_loadButton, &QPushButton::clicked, this, &OptimizerTab::loadOptimizedProgram);

    connect(m_benchmarkButton, &QPushButton::clicked, this,
            &OptimizerTab::benchmarkOptimizedProgram);
}

void OptimizerTab::setLoadHandler(std::function<void(const AssembledProgram &)> handler)
{
    m_loadHandler = std::move(handler);
}

void OptimizerTab::setBenchmarkHandler(
    std::function<void(const AssembledProgram &, const AssembledProgram &, int)> handler)
{
    m_benchmarkHandler = std::move(handler);
}

// KITES Phase 1 contribution: initiate the original-versus-optimized benchmark workflow.
void OptimizerTab::benchmarkOptimizedProgram()
{
    if (!m_originalProgram || !m_optimizedProgram)
    {
        QMessageBox::warning(this, "Benchmark unavailable",
                             "Generate suggestions successfully before benchmarking.");
        return;
    }

    if (!m_benchmarkHandler)
    {
        QMessageBox::warning(this, "Benchmark unavailable",
                             "The main window has not connected the benchmark handler.");
        return;
    }

    m_benchmarkHandler(*m_originalProgram, *m_optimizedProgram, m_optimizedWindowSize);
}

void OptimizerTab::loadOptimizedProgram()
{
    if (!m_optimizedProgram)
    {
        m_statusLabel->setText("No optimized program is available. Generate suggestions first.");
        return;
    }

    if (!m_loadHandler)
    {
        m_statusLabel->setText("Error: optimized-program execution is not connected.");
        QMessageBox::warning(this, "Execution unavailable",
                             "The main window has not connected the optimized-program "
                             "execution handler.");
        return;
    }

    qDebug() << "[Optimizer] Executing optimized program";
    qDebug() << "[Optimizer] Instruction count:" << m_optimizedProgram->intermediate_code.size();

    m_loadHandler(*m_optimizedProgram);
}

void OptimizerTab::optimizeCurrentSource()
{
    // Invalidate the previous result first. A failed optimization must
    // never leave an older program available for accidental execution.
    m_originalProgram.reset();
    m_optimizedProgram.reset();

    m_loadButton->setEnabled(false);
    m_benchmarkButton->setEnabled(false);
    m_originalOutput->clear();
    m_optimizedOutput->clear();

    if (!m_editor)
    {
        m_statusLabel->setText("Error: editor connection is missing.");
        return;
    }

    try
    {
        // Expand custom pseudo-instructions before assembling the source.
        const std::string source =
            customPseudoManager::expandPseudoInstruction(m_editor->getRawText());

        const auto tempPath = std::filesystem::temp_directory_path() / "kites_optimizer_input.s";

        {
            std::ofstream out(tempPath);
            if (!out)
            {
                throw std::runtime_error("Could not create the temporary assembly file.");
            }

            out << source;
            if (!out)
            {
                throw std::runtime_error("Could not write the temporary assembly file.");
            }
        }

        // Keep a mutable copy: we will modify its instruction sequence
        // and rebuild the metadata needed by the processor.
        AssembledProgram originalProgram = Kites::assemble(tempPath.string());

        AssembledProgram program = originalProgram;

        std::vector<ICUnit> instructions;
        instructions.reserve(program.intermediate_code.size());

        for (const auto &entry : program.intermediate_code)
        {
            instructions.push_back(entry.first);
        }

        // This initial phase only supports straight-line code.
        // Reordering across control-flow instructions can change behavior.
        const std::vector<std::string> controlFlowOpcodes = {"beq",  "bne",  "blt",   "bge",
                                                             "bltu", "bgeu", "jal",   "jalr",
                                                             "c.j",  "c.jr", "ecall", "ebreak"};

        for (const auto &instruction : instructions)
        {
            const std::string opcode = instruction.getOpcode();

            if (std::find(controlFlowOpcodes.begin(), controlFlowOpcodes.end(), opcode) !=
                controlFlowOpcodes.end())
            {
                throw std::runtime_error("This initial optimizer phase does not support "
                                         "branches, jumps, or traps. Try a straight-line "
                                         "instruction sequence.");
            }
        }

        // Non-data labels at nonzero addresses may point into the code.
        // Until label-aware reordering is implemented, reject such programs.
        for (const auto &symbolEntry : program.symbol_table)
        {
            const auto &symbol = symbolEntry.second;

            if (!symbol.isData && symbol.address != 0)
            {
                throw std::runtime_error("This initial optimizer phase supports only code "
                                         "labels at address zero. A nonzero code label was "
                                         "found: " +
                                         symbolEntry.first);
            }
        }

        const std::size_t windowSizeUsed = static_cast<std::size_t>(m_windowSizeSpinBox->value());

        m_optimizedWindowSize = static_cast<int>(windowSizeUsed);

        printInstructions(instructions, "BEFORE SCHEDULING");

        const bool forwardingEnabled =
            m_hazardModelComboBox && m_hazardModelComboBox->currentData().toBool();

        const auto result =
            forwardingEnabled
                ? optimizer::scheduleForwardingBasicBlock(instructions, windowSizeUsed)
                : optimizer::scheduleLoadUseHazards(instructions, windowSizeUsed);

        printInstructions(result.instructions, "AFTER SCHEDULING");

        qDebug() << "Original instruction count:" << instructions.size();
        qDebug() << "Scheduled instruction count:" << result.instructions.size();
        qDebug() << "Instructions moved:" << result.moved_instructions;

        for (std::size_t i = 0; i < result.instructions.size(); ++i)
        {
            std::ostringstream line;
            line << result.instructions[i];

            qDebug() << "Scheduled" << i << ":" << QString::fromStdString(line.str());
        }

        // Reconstruct the intermediate-code entries in scheduled order.
        // The scheduler changes instruction_index, so match using the original
        // source line and the actual instruction fields instead.

        std::vector<std::pair<ICUnit, bool>> optimizedIntermediateCode;
        optimizedIntermediateCode.reserve(result.instructions.size());

        std::vector<bool> originalEntryUsed(program.intermediate_code.size(), false);

        for (const auto &scheduled : result.instructions)
        {
            bool found = false;

            for (std::size_t i = 0; i < program.intermediate_code.size(); ++i)
            {
                if (originalEntryUsed[i])
                    continue;

                const ICUnit &original = program.intermediate_code[i].first;

                const bool sameInstruction =
                    original.line_number == scheduled.line_number &&
                    original.opcode == scheduled.opcode && original.rd == scheduled.rd &&
                    original.rs1 == scheduled.rs1 && original.rs2 == scheduled.rs2 &&
                    original.rs3 == scheduled.rs3 && original.imm == scheduled.imm &&
                    original.csr == scheduled.csr && original.rm == scheduled.rm &&
                    original.label == scheduled.label;

                if (sameInstruction)
                {
                    optimizedIntermediateCode.emplace_back(scheduled,
                                                           program.intermediate_code[i].second);

                    originalEntryUsed[i] = true;
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                throw std::runtime_error("Could not match a scheduled instruction to its original "
                                         "assembled entry.");
            }
        }

        // Replace the original sequence with the scheduled sequence.
        program.intermediate_code = std::move(optimizedIntermediateCode);

        // Rebuild instruction-index-related metadata because the order
        // of instructions has changed.
        program.line_number_instruction_number_mapping.clear();
        program.instruction_number_line_number_mapping.clear();
        program.instruction_number_instruction_text_mapping.clear();
        program.instruction_number_disassembly_mapping.clear();

        for (std::size_t i = 0; i < program.intermediate_code.size(); ++i)
        {
            auto &instruction = program.intermediate_code[i].first;

            instruction.setInstructionIndex(static_cast<int>(i));

            const auto instructionNumber = i;
            const auto lineNumber = instruction.getLineNumber();

            program.line_number_instruction_number_mapping[lineNumber] = instructionNumber;

            program.instruction_number_line_number_mapping[instructionNumber] = lineNumber;

            std::ostringstream instructionText;
            instructionText << instruction;

            program.instruction_number_instruction_text_mapping[instructionNumber] =
                instructionText.str();
        }

        // Machine code must correspond to the newly scheduled sequence,
        // not the original sequence assembled from the editor.
        program.text_buffer = Kites::generateMachineCode(program.intermediate_code);

        // Display both listings. Match whole instruction records so repeated
        // instructions are handled as separate occurrences.
        std::vector<bool> originalUsed(program.intermediate_code.size(), false);
        std::vector<int> originalPositionForOptimized;
        originalPositionForOptimized.reserve(program.intermediate_code.size());

        for (const auto &scheduledEntry : program.intermediate_code)
        {
            const ICUnit &scheduled = scheduledEntry.first;
            int matchedPosition = -1;

            for (std::size_t i = 0; i < originalProgram.intermediate_code.size(); ++i)
            {
                if (originalUsed[i])
                    continue;

                const ICUnit &original = originalProgram.intermediate_code[i].first;
                const bool sameInstruction =
                    original.line_number == scheduled.line_number &&
                    original.opcode == scheduled.opcode && original.rd == scheduled.rd &&
                    original.rs1 == scheduled.rs1 && original.rs2 == scheduled.rs2 &&
                    original.rs3 == scheduled.rs3 && original.imm == scheduled.imm &&
                    original.csr == scheduled.csr && original.rm == scheduled.rm &&
                    original.label == scheduled.label;

                if (sameInstruction)
                {
                    originalUsed[i] = true;
                    matchedPosition = static_cast<int>(i);
                    break;
                }
            }

            originalPositionForOptimized.push_back(matchedPosition);
        }

        // Render aligned columns using a fixed-width font.
        // Original # and optimized # are listing positions (1-based).
        // Source line preserves the source location carried by each instruction.
        QString originalPreview = "ORG # | SRC LINE | INSTRUCTION\n"
                                  "------+----------+----------------------------------------\n";

        for (std::size_t i = 0; i < originalProgram.intermediate_code.size(); ++i)
        {
            const ICUnit &instruction = originalProgram.intermediate_code[i].first;

            std::ostringstream instructionText;
            instructionText << instruction;

            originalPreview += QString("%1 | %2 | %3\n")
                                   .arg(static_cast<qulonglong>(i + 1), 5)
                                   .arg(instruction.line_number, 8)
                                   .arg(QString::fromStdString(instructionText.str()));
        }

        QString optimizedPreview =
            "OPT # | ORG # | SRC LINE | STATUS    | INSTRUCTION\n"
            "------+-------+----------+-----------+----------------------------------------\n";

        for (std::size_t i = 0; i < program.intermediate_code.size(); ++i)
        {
            const ICUnit &instruction = program.intermediate_code[i].first;

            std::ostringstream instructionText;
            instructionText << instruction;

            const int originalPosition = originalPositionForOptimized[i];
            const bool moved = originalPosition >= 0 && originalPosition != static_cast<int>(i);

            optimizedPreview += QString("%1 | %2 | %3 | %4 | %5\n")
                                    .arg(static_cast<qulonglong>(i + 1), 5)
                                    .arg(originalPosition >= 0 ? originalPosition + 1 : 0, 5)
                                    .arg(instruction.line_number, 8)
                                    .arg(moved ? "MOVED" : "UNCHANGED", -9)
                                    .arg(QString::fromStdString(instructionText.str()));
        }

        m_originalOutput->setPlainText(originalPreview);
        m_optimizedOutput->setPlainText(optimizedPreview);

        // Retain the complete program for the separate execution action.
        m_originalProgram = std::move(originalProgram);
        m_optimizedProgram = std::move(program);

        m_loadButton->setEnabled(true);
        m_benchmarkButton->setEnabled(true);

        m_statusLabel->setText(QString("Suggestions generated. Instructions moved: %1. "
                                       "The original editor source is unchanged.")
                                   .arg(static_cast<qulonglong>(result.moved_instructions)));
    }
    catch (const std::exception &e)
    {

        m_originalProgram.reset();
        m_benchmarkButton->setEnabled(false);

        m_optimizedProgram.reset();
        m_loadButton->setEnabled(false);
        m_originalOutput->clear();
        m_optimizedOutput->clear();

        m_statusLabel->setText("Optimization failed.");

        QMessageBox::warning(this, "Optimization failed", QString::fromUtf8(e.what()));
    }
}

void OptimizerTab::showBenchmarkResults(int windowSize, const OptimizerBenchmarkMetrics &original,
                                        const OptimizerBenchmarkMetrics &optimized)
{
    const auto reductionPercent = [](double before, double after)
    {
        if (before <= 0.0)
            return 0.0;
        return (before - after) * 100.0 / before;
    };

    const double cycleChange = reductionPercent(original.cycles, optimized.cycles);
    const double stallChange = reductionPercent(original.stalls, optimized.stalls);
    const double cpiChange = reductionPercent(original.cpi, optimized.cpi);
    const bool sameRetiredCount = original.instructionsRetired == optimized.instructionsRetired;

    QDialog dialog(this);
    dialog.setWindowTitle("Benchmark Results");
    // Use the active Qt palette for readable text in light and dark themes.
    dialog.setStyleSheet(
        "QDialog { color: palette(text); }"
        "QLabel { color: palette(text); }"
    );
    dialog.setMinimumSize(680, 540);
    dialog.resize(760, 620);

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(24, 22, 24, 22);
    root->setSpacing(14);

    auto *heading = new QLabel("Performance comparison", &dialog);
    QFont headingFont = heading->font();
    headingFont.setPointSize(18);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto *subtitle = new QLabel(
        QString("Scheduling window: %1  ·  Original program vs. optimized program").arg(windowSize),
        &dialog);
    subtitle->setStyleSheet("color: palette(text); font-size: 10pt;");
    root->addWidget(subtitle);

    auto *cards = new QGridLayout();
    cards->setSpacing(10);
    root->addLayout(cards);

    auto addCard = [&](int column, const QString &title, const QString &value,
                       const QString &detail, const QString &accent)
    {
        auto *card = new QFrame(&dialog);
        card->setFrameShape(QFrame::StyledPanel);
        card->setStyleSheet(
            QString("QFrame { border: 1px solid palette(midlight); border-radius: 8px; }"));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(13, 11, 13, 11);
        cardLayout->setSpacing(4);
        auto *titleLabel = new QLabel(title, card);
        titleLabel->setStyleSheet("color: palette(text); border: none; font-size: 9pt;");
        auto *valueLabel = new QLabel(value, card);
        QFont valueFont = valueLabel->font();
        valueFont.setPointSize(17);
        valueFont.setBold(true);
        valueLabel->setFont(valueFont);
        valueLabel->setStyleSheet(QString("color: %1; border: none;").arg(accent));
        auto *detailLabel = new QLabel(detail, card);
        detailLabel->setWordWrap(true);
        detailLabel->setStyleSheet("color: palette(text); border: none; font-size: 9pt;");
        cardLayout->addWidget(titleLabel);
        cardLayout->addWidget(valueLabel);
        cardLayout->addWidget(detailLabel);
        cards->addWidget(card, 0, column);
    };

    const auto changeText = [](double value)
    {
        if (value > 0.005)
            return QString("%1% lower").arg(value, 0, 'f', 2);
        if (value < -0.005)
            return QString("%1% higher").arg(-value, 0, 'f', 2);
        return QString("No change");
    };

    addCard(0, "CYCLES SAVED",
            QString::number(static_cast<long long>(original.cycles) - optimized.cycles),
            changeText(cycleChange), cycleChange >= 0 ? "#21865b" : "#c04b45");
    addCard(1, "STALL REDUCTION",
            QString::number(static_cast<long long>(original.stalls) - optimized.stalls),
            changeText(stallChange), stallChange >= 0 ? "#21865b" : "#c04b45");
    addCard(2, "CPI IMPROVEMENT", changeText(cpiChange),
            QString("%1 → %2").arg(original.cpi, 0, 'f', 3).arg(optimized.cpi, 0, 'f', 3),
            cpiChange >= 0 ? "#21865b" : "#c04b45");

    auto *comparisonTitle = new QLabel("Metric breakdown", &dialog);
    QFont sectionFont = comparisonTitle->font();
    sectionFont.setBold(true);
    sectionFont.setPointSize(12);
    comparisonTitle->setFont(sectionFont);
    root->addWidget(comparisonTitle);

    auto *table = new QTableWidget(4, 3, &dialog);
    table->setStyleSheet("QTableWidget {"
                         "  color: palette(text);"
                         "  background-color: palette(base);"
                         "  gridline-color: palette(midlight);"
                         "  selection-background-color: palette(highlight);"
                         "  selection-color: palette(highlighted-text);"
                         "}"
                         "QHeaderView::section {"
                         "  color: palette(text);"
                         "  background-color: palette(alternate-base);"
                         "  padding: 6px;"
                         "  border: none;"
                         "  font-weight: bold;"
                         "}"
                         "QTableWidget::item {"
                         "  padding: 5px;"
                         "}");
    table->setHorizontalHeaderLabels({"Metric", "Original", "Optimized"});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setShowGrid(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->setMinimumHeight(175);

    const QStringList names = {"Cycles", "Stall cycles", "Instructions retired", "CPI"};
    const QStringList originalValues = {
        QString::number(original.cycles), QString::number(original.stalls),
        QString::number(original.instructionsRetired), QString::number(original.cpi, 'f', 3)};
    const QStringList optimizedValues = {
        QString::number(optimized.cycles), QString::number(optimized.stalls),
        QString::number(optimized.instructionsRetired), QString::number(optimized.cpi, 'f', 3)};

    for (int row = 0; row < names.size(); ++row)
    {
        table->setItem(row, 0, new QTableWidgetItem(names[row]));
        table->setItem(row, 1, new QTableWidgetItem(originalValues[row]));
        table->setItem(row, 2, new QTableWidgetItem(optimizedValues[row]));
        table->item(row, 1)->setTextAlignment(Qt::AlignCenter);
        table->item(row, 2)->setTextAlignment(Qt::AlignCenter);
    }
    root->addWidget(table);

    auto addBarComparison =
        [&](const QString &label, unsigned int originalValue, unsigned int optimizedValue)
    {
        auto *rowWidget = new QWidget(&dialog);
        auto *rowLayout = new QGridLayout(rowWidget);
        rowLayout->setContentsMargins(0, 2, 0, 2);
        rowLayout->setHorizontalSpacing(10);
        auto *name = new QLabel(label, rowWidget);
        name->setMinimumWidth(95);
        auto *originalBar = new QProgressBar(rowWidget);
        auto *optimizedBar = new QProgressBar(rowWidget);
        const int maximum = static_cast<int>(std::max(1u, std::max(originalValue, optimizedValue)));
        originalBar->setRange(0, maximum);
        optimizedBar->setRange(0, maximum);
        originalBar->setValue(static_cast<int>(originalValue));
        optimizedBar->setValue(static_cast<int>(optimizedValue));
        originalBar->setFormat(QString("Original · %1").arg(originalValue));
        optimizedBar->setFormat(QString("Optimized · %1").arg(optimizedValue));
        originalBar->setTextVisible(true);
        optimizedBar->setTextVisible(true);
        const QString barStyle =
            "QProgressBar {"
            "  min-height: 22px;"
            "  text-align: center;"
            "  color: palette(text);"
            "  background-color: palette(alternate-base);"
            "  border: 1px solid palette(midlight);"
            "  border-radius: 4px;"
            "}"
            "QProgressBar::chunk {"
            "  background-color: #607D9B;"
            "  border-radius: 3px;"
            "}";
        originalBar->setStyleSheet(barStyle);
        optimizedBar->setStyleSheet(barStyle);
        rowLayout->addWidget(name, 0, 0, 2, 1);
        rowLayout->addWidget(originalBar, 0, 1);
        rowLayout->addWidget(optimizedBar, 1, 1);
        root->addWidget(rowWidget);
    };

    addBarComparison("Cycles", original.cycles, optimized.cycles);
    addBarComparison("Stall cycles", original.stalls, optimized.stalls);

    auto *status = new QLabel(
        sameRetiredCount
            ? QString("Instruction count matches: %1 retired in each run. This is a useful check, "
                      "but not a complete proof of equivalent program state.")
                  .arg(original.instructionsRetired)
            : QString("Warning: retired instruction counts differ (%1 original, %2 optimized). "
                      "Investigate correctness before trusting the performance comparison.")
                  .arg(original.instructionsRetired)
                  .arg(optimized.instructionsRetired),
        &dialog);
    status->setWordWrap(true);
    status->setStyleSheet(
        sameRetiredCount ? "padding: 9px; border-radius: 6px; background: palette(alternate-base);"
                         : "padding: 9px; border-radius: 6px; color: #c04b45; background: "
                           "palette(alternate-base);");
    root->addWidget(status);

    auto *closeButton = new QPushButton("Close", &dialog);
    closeButton->setMinimumWidth(100);
    auto *buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    buttonRow->addWidget(closeButton);
    root->addLayout(buttonRow);
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    m_statusLabel->setText(
        "Benchmark complete. Graphical results are shown in the comparison window.");
    dialog.exec();
}

} // namespace Kites
