#include "optimizertab.h"

#include "optimizer/dependency_graph.h"
#include "optimizer/program_optimizer.h"
#include "optimizer/scheduler_core.h"
#include "ui/editor_tab/editortab.h"
#include "ui/optimizer_tab/benchmark_results_dialog.h"
#include "ui/optimizer_tab/instruction_listing.h"
#include "ui/optimizer_tab/instruction_listing_highlighter.h"

#include <QComboBox>
#include <QDebug>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include <exception>

namespace Kites
{
namespace
{
using DependencyRegisterMap = Kites::DependencyRegisterMap;

void markIntersection(const std::vector<std::string> &a,
                      const std::vector<std::string> &b,
                      std::map<std::string, DependencyRegisterStyle> &left,
                      std::map<std::string, DependencyRegisterStyle> &right,
                      bool leftIsWrite, bool rightIsWrite)
{
    for (const auto &reg : a)
    {
        if (std::find(b.begin(), b.end(), reg) == b.end())
            continue;
        auto &leftStyle = left[reg];
        auto &rightStyle = right[reg];
        (leftIsWrite ? leftStyle.written : leftStyle.read) = true;
        (rightIsWrite ? rightStyle.written : rightStyle.read) = true;
    }
}

DependencyRegisterMap dependencyRegistersForListing(
    const AssembledProgram &original, const std::vector<int> &instructionBlocks,
    const std::vector<int> *optimizedToOriginal = nullptr)
{
    std::vector<optimizer::detail::InstructionInfo> info;
    info.reserve(original.intermediate_code.size());
    for (const auto &entry : original.intermediate_code)
        info.push_back(optimizer::detail::describe(entry.first));

    std::vector<std::map<std::string, DependencyRegisterStyle>> perInstruction(info.size());
    std::set<std::size_t> labelBoundaries;
    for (const auto &[name, symbol] : original.symbol_table)
    {
        (void)name;
        if (!symbol.isData && (symbol.address % 4) == 0)
        {
            const auto index = static_cast<std::size_t>(symbol.address / 4);
            if (index < info.size())
                labelBoundaries.insert(index);
        }
    }
    // Follow the scheduler's region boundaries so unrelated blocks are not
    // presented as if their register dependencies affected scheduling.
    std::size_t regionStart = 0;
    while (regionStart < info.size())
    {
        if (!info[regionStart].supported || info[regionStart].is_store)
        {
            ++regionStart;
            continue;
        }

        std::size_t regionEnd = regionStart;
        while (regionEnd < info.size() &&
               (regionEnd == regionStart || !labelBoundaries.count(regionEnd)) &&
               info[regionEnd].supported && !info[regionEnd].is_store)
        {
            const bool branch = optimizer::detail::isConditionalBranch(
                original.intermediate_code[regionEnd].first);
            ++regionEnd;
            if (branch)
                break;
        }

        for (std::size_t i = regionStart; i < regionEnd; ++i)
        {
            for (std::size_t j = i + 1; j < regionEnd; ++j)
            {
                if (!optimizer::detail::hasDependency(info[i], info[j]))
                    continue;
                // Mark only the operands that actually form RAW, WAR or WAW
                // hazards. Read operands are outlined; write operands are filled.
                markIntersection(info[i].writes, info[j].reads,
                                 perInstruction[i], perInstruction[j], true, false);
                markIntersection(info[i].reads, info[j].writes,
                                 perInstruction[i], perInstruction[j], false, true);
                markIntersection(info[i].writes, info[j].writes,
                                 perInstruction[i], perInstruction[j], true, true);
            }
        }
        regionStart = regionEnd;
    }

    DependencyRegisterMap result;
    const std::size_t count = std::min(instructionBlocks.size(),
                                       optimizedToOriginal ? optimizedToOriginal->size()
                                                           : info.size());
    for (std::size_t outputIndex = 0; outputIndex < count; ++outputIndex)
    {
        const int originalIndex = optimizedToOriginal
                                      ? (*optimizedToOriginal)[outputIndex]
                                      : static_cast<int>(outputIndex);
        if (originalIndex < 0 || static_cast<std::size_t>(originalIndex) >= perInstruction.size())
            continue;
        auto &registers = result[instructionBlocks[outputIndex]];
        for (const auto &[reg, style] : perInstruction[static_cast<std::size_t>(originalIndex)])
        {
            auto &target = registers[QString::fromStdString(reg)];
            target.written = style.written;
            target.read = style.read;
        }
    }
    return result;
}
} // namespace

OptimizerTab::OptimizerTab(EditorTab *editor, QWidget *parent) : KitesTab(parent), m_editor(editor)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    addHeading(layout);
    addHazardModelSelector(layout);
    addActionButtons(layout);

    m_statusLabel =
        new QLabel("Ready. Generate suggestions without changing the original source.", this);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    addListings(layout);

    connect(m_optimizeButton, &QPushButton::clicked, this, &OptimizerTab::optimizeCurrentSource);
    connect(m_loadButton, &QPushButton::clicked, this, &OptimizerTab::loadOptimizedProgram);
    connect(m_benchmarkButton, &QPushButton::clicked, this,
            &OptimizerTab::benchmarkOptimizedProgram);
}

void OptimizerTab::addHeading(QVBoxLayout *layout)
{
    auto *heading = new QLabel("Instruction Scheduler", this);
    QFont font = heading->font();
    font.setPointSize(18);
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);
}

void OptimizerTab::addHazardModelSelector(QVBoxLayout *layout)
{
    auto *form = new QFormLayout();

    // Keep the existing scheduler as the default baseline. The forwarding
    // scheduler can be selected independently for side-by-side experiments.
    m_hazardModelComboBox = new QComboBox(this);
    m_hazardModelComboBox->addItem("No forwarding (baseline)", false);
    m_hazardModelComboBox->addItem("With forwarding (H_F)", true);
    m_hazardModelComboBox->setCurrentIndex(0);
    form->addRow("Hazard model:", m_hazardModelComboBox);

    layout->addLayout(form);
}

void OptimizerTab::addActionButtons(QVBoxLayout *layout)
{
    m_optimizeButton = new QPushButton("Generate Suggestions", this);
    m_loadButton = new QPushButton("Execute Optimized Program", this);
    m_benchmarkButton = new QPushButton("Benchmark Original vs Optimized", this);

    m_loadButton->setEnabled(false);
    m_benchmarkButton->setEnabled(false);

    layout->addWidget(m_optimizeButton);
    layout->addWidget(m_loadButton);
    layout->addWidget(m_benchmarkButton);
}

void OptimizerTab::addListings(QVBoxLayout *layout)
{
    auto *listingSplitter = new QSplitter(Qt::Horizontal, this);

    m_originalOutput = createListingPanel(listingSplitter, "Original instructions",
                                          "Original instruction listing appears here.",
                                          &m_originalHighlighter);
    m_optimizedOutput = createListingPanel(listingSplitter, "Optimized instructions",
                                           "Optimized instruction listing appears here.",
                                           &m_optimizedHighlighter);

    auto *legend = new QLabel(
        "Dependency registers are bold with a coloured underline; each register keeps its colour. "
        "Click an instruction to draw its register dependencies as arrows.",
        this);
    legend->setWordWrap(true);
    layout->addWidget(legend);

    listingSplitter->setStretchFactor(0, 1);
    listingSplitter->setStretchFactor(1, 1);
    layout->addWidget(listingSplitter, 1);
}

InstructionListingTextEdit *OptimizerTab::createListingPanel(QSplitter *splitter, const QString &title,
                                                 const QString &placeholder,
                                                 InstructionListingHighlighter **highlighter)
{
    auto *panel = new QWidget(splitter);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);

    panelLayout->addWidget(new QLabel(title, panel));

    auto *output = new InstructionListingTextEdit(panel);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setPlaceholderText(placeholder);
    output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output->setStyleSheet("QPlainTextEdit { padding: 8px; }");
    panelLayout->addWidget(output);

    // Parented to the document, so it lives as long as the listing does.
    if (highlighter)
    {
        *highlighter = new InstructionListingHighlighter(output->document());
    }

    splitter->addWidget(panel);
    return output;
}

void OptimizerTab::setLoadHandler(
    std::function<void(const AssembledProgram &, bool)> handler)
{
    m_loadHandler = std::move(handler);
}

void OptimizerTab::setBenchmarkHandler(
    std::function<void(const AssembledProgram &, const AssembledProgram &, bool)> handler)
{
    m_benchmarkHandler = std::move(handler);
}

// KITES Phase 1 contribution: keep the benchmark CPU in sync with the dropdown.
// If the "Hazard model" dropdown changed since the suggestions were generated,
// those suggestions (and the CPU they would run on) are stale, so regenerate
// them for the selected model instead of silently reusing the old one.
bool OptimizerTab::ensureSuggestionsMatchSelectedModel()
{
    if (!m_originalProgram || !m_optimizedProgram || !m_hazardModelComboBox)
        return m_originalProgram && m_optimizedProgram;

    const bool selectedForwarding = m_hazardModelComboBox->currentData().toBool();
    if (selectedForwarding == m_optimizedWithForwarding)
        return true;

    optimizeCurrentSource();
    return m_originalProgram && m_optimizedProgram &&
           m_optimizedWithForwarding == selectedForwarding;
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

    if (!ensureSuggestionsMatchSelectedModel())
        return; // regeneration failed; status label already explains why

    m_benchmarkHandler(*m_originalProgram, *m_optimizedProgram, m_optimizedWithForwarding);
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

    if (!ensureSuggestionsMatchSelectedModel())
        return;

    qDebug() << "[Optimizer] Executing optimized program";
    qDebug() << "[Optimizer] Instruction count:" << m_optimizedProgram->intermediate_code.size();

    m_loadHandler(*m_optimizedProgram, m_optimizedWithForwarding);
}

void OptimizerTab::optimizeCurrentSource()
{
    resetResults();

    if (!m_editor)
    {
        m_statusLabel->setText("Error: editor connection is missing.");
        return;
    }

    try
    {
        AssembledProgram original = optimizer::assembleEditorSource(m_editor->getRawText());

        const bool forwardingEnabled =
            m_hazardModelComboBox && m_hazardModelComboBox->currentData().toBool();

        const optimizer::ScheduleResult schedule =
            optimizer::scheduleProgram(original, forwardingEnabled);
        AssembledProgram optimized = optimizer::buildOptimizedProgram(original, schedule);

        showListings(original, optimized);

        m_originalProgram = std::move(original);
        m_optimizedProgram = std::move(optimized);
        m_optimizedWithForwarding = forwardingEnabled;

        m_loadButton->setEnabled(true);
        m_benchmarkButton->setEnabled(true);

        m_statusLabel->setText(QString("Suggestions generated. Instructions moved: %1. "
                                       "The original editor source is unchanged.")
                                   .arg(static_cast<qulonglong>(schedule.moved_instructions)));
    }
    catch (const std::exception &e)
    {
        handleOptimizationFailure(QString::fromUtf8(e.what()));
    }
}

// A failed optimization must never leave an older program available for execution.
void OptimizerTab::resetResults()
{
    m_originalProgram.reset();
    m_optimizedProgram.reset();
    m_optimizedWithForwarding = false;

    m_loadButton->setEnabled(false);
    m_benchmarkButton->setEnabled(false);
    m_originalOutput->clear();
    m_optimizedOutput->clear();
    m_originalOutput->setDependencies({}, {});
    m_optimizedOutput->setDependencies({}, {});
    m_originalSourceLineBlocks.clear();
    m_optimizedSourceLineBlocks.clear();
}

void OptimizerTab::showListings(const AssembledProgram &original,
                                const AssembledProgram &optimized)
{
    const ListingView originalListing = buildOriginalListing(original);
    const std::vector<int> positions = optimizer::originalPositions(original, optimized);
    const ListingView optimizedListing = buildOptimizedListing(optimized, positions);

    m_originalSourceLineBlocks = originalListing.sourceLineBlocks;
    m_optimizedSourceLineBlocks = optimizedListing.sourceLineBlocks;

    m_originalOutput->setPlainText(originalListing.text);
    m_optimizedOutput->setPlainText(optimizedListing.text);

    if (m_originalHighlighter)
        m_originalHighlighter->setDependencyRegisters(
            dependencyRegistersForListing(original, originalListing.instructionBlocks));
    if (m_optimizedHighlighter)
    {
        m_optimizedHighlighter->setDependencyRegisters(
            dependencyRegistersForListing(original, optimizedListing.instructionBlocks, &positions));
    }

    m_originalOutput->setDependencies(optimizer::findRegisterDependencies(original),
                                      originalListing.instructionBlocks);
    m_optimizedOutput->setDependencies(optimizer::findRegisterDependencies(optimized),
                                       optimizedListing.instructionBlocks);
}

void OptimizerTab::handleOptimizationFailure(const QString &reason)
{
    m_originalProgram.reset();
    m_benchmarkButton->setEnabled(false);

    m_optimizedProgram.reset();
    m_loadButton->setEnabled(false);
    m_originalOutput->clear();
    m_optimizedOutput->clear();

    m_originalOutput->setDependencies({}, {});
    m_optimizedOutput->setDependencies({}, {});

    m_statusLabel->setText("Optimization failed.");

    QMessageBox::warning(this, "Optimization failed", reason);
}

void OptimizerTab::setExecutionHighlights(
    const std::vector<std::pair<int, std::string>> &editorLines)
{
    highlightExecutionLines(m_originalOutput, m_originalSourceLineBlocks, editorLines);
    highlightExecutionLines(m_optimizedOutput, m_optimizedSourceLineBlocks, editorLines);
}

void OptimizerTab::showBenchmarkResults(const OptimizerBenchmarkMetrics &original,
                                        const OptimizerBenchmarkMetrics &optimized)
{
    BenchmarkResultsDialog dialog(original, optimized, this);

    m_statusLabel->setText(
        "Benchmark complete. Graphical results are shown in the comparison window.");
    dialog.exec();
}

} // namespace Kites
