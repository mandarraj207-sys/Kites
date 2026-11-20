#include "optimizertab.h"

#include "optimizer/dependency_graph.h"
#include "optimizer/program_optimizer.h"
#include "optimizer/scheduler_core.h"
#include "ui/editor_tab/editortab.h"
#include "ui/optimizer_tab/instruction_listing.h"
#include "ui/optimizer_tab/instruction_listing_highlighter.h"

#include <QComboBox>
#include <QApplication>
#include <QClipboard>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QDebug>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QWidget>
#include <QStringList>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <sstream>
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

// Everything the expanded details need to know about one instruction. `positions`
// maps each instruction to its index in the original program (null = identity).
std::vector<InstructionDetail> instructionDetailsFor(const AssembledProgram &program,
                                                     const std::vector<int> *positions,
                                                     const std::vector<int> &movement)
{
    std::vector<InstructionDetail> details;
    details.reserve(program.intermediate_code.size());
    for (std::size_t i = 0; i < program.intermediate_code.size(); ++i)
    {
        const ICUnit &unit = program.intermediate_code[i].first;
        const auto info = optimizer::detail::describe(unit);

        int original = static_cast<int>(i);
        if (positions && i < positions->size() && (*positions)[i] >= 0)
            original = (*positions)[i];

        InstructionDetail detail;
        detail.originalIndex = static_cast<qlonglong>(original) + 1;
        detail.sourceLine = static_cast<qlonglong>(unit.line_number);
        detail.movement = i < movement.size() ? movement[i] : 0;
        detail.assembly = QString::fromStdString(formatAssemblyInstruction(unit));
        detail.reads = info.reads;
        detail.writes = info.writes;
        details.push_back(std::move(detail));
    }
    return details;
}

constexpr int kDetailsMaxHeight = 230;

const QString kPrimaryButtonStyle =
    "QPushButton { background: palette(highlight); color: palette(highlighted-text);"
    "  border: none; border-radius: 6px; padding: 7px 16px; font-weight: 600; }"
    "QPushButton:disabled { background: palette(midlight); color: palette(mid); }"
    "QPushButton:pressed { padding-top: 8px; }";

const QString kSecondaryButtonStyle =
    "QPushButton { background: palette(button); color: palette(button-text);"
    "  border: 1px solid palette(mid); border-radius: 6px; padding: 7px 16px; }"
    "QPushButton:hover:enabled { background: palette(midlight); }"
    "QPushButton:disabled { color: palette(mid); border-color: palette(midlight); }";

const QString kBannerStyle =
    "QLabel { padding: 8px 12px; border: 1px solid palette(midlight);"
    "  border-radius: 6px; background: palette(alternate-base); color: palette(text); }";

struct DependencyCounts
{
    std::size_t raw = 0;
    std::size_t war = 0;
    std::size_t waw = 0;
};

DependencyCounts countDependencies(const AssembledProgram &program)
{
    std::vector<optimizer::detail::InstructionInfo> info;
    info.reserve(program.intermediate_code.size());
    for (const auto &entry : program.intermediate_code)
        info.push_back(optimizer::detail::describe(entry.first));

    std::set<std::size_t> labelBoundaries;
    for (const auto &[name, symbol] : program.symbol_table)
    {
        (void)name;
        if (!symbol.isData && symbol.address % 4 == 0)
        {
            const auto index = static_cast<std::size_t>(symbol.address / 4);
            if (index < info.size())
                labelBoundaries.insert(index);
        }
    }

    DependencyCounts counts;
    std::size_t begin = 0;
    while (begin < info.size())
    {
        if (!info[begin].supported || info[begin].is_store)
        {
            ++begin;
            continue;
        }

        std::size_t end = begin;
        while (end < info.size() &&
               (end == begin || !labelBoundaries.count(end)) &&
               info[end].supported && !info[end].is_store)
        {
            const bool branch =
                optimizer::detail::isConditionalBranch(program.intermediate_code[end].first);
            ++end;
            if (branch)
                break;
        }

        for (std::size_t i = begin; i < end; ++i)
        {
            for (std::size_t j = i + 1; j < end; ++j)
            {
                const auto &a = info[i];
                const auto &b = info[j];

                for (const auto &reg : a.writes)
                    if (std::find(b.reads.begin(), b.reads.end(), reg) != b.reads.end())
                        ++counts.raw;

                for (const auto &reg : a.reads)
                    if (std::find(b.writes.begin(), b.writes.end(), reg) != b.writes.end())
                        ++counts.war;

                for (const auto &reg : a.writes)
                    if (std::find(b.writes.begin(), b.writes.end(), reg) != b.writes.end())
                        ++counts.waw;
            }
        }
        begin = end;
    }
    return counts;
}

} // namespace

OptimizerTab::OptimizerTab(EditorTab *editor, QWidget *parent) : KitesTab(parent), m_editor(editor)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    addHeading(layout);
    addHazardModelSelector(layout);
    m_statusLabel =
        new QLabel("Ready. Generate suggestions without changing the original source.", this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(kBannerStyle);
    layout->addWidget(m_statusLabel);

    m_innerTabs = new QTabWidget(this);

    auto *reorderedPage = new QWidget(m_innerTabs);
    auto *reorderedLayout = new QVBoxLayout(reorderedPage);
    reorderedLayout->setContentsMargins(8, 8, 8, 8);
    reorderedLayout->setSpacing(10);
    addActionButtons(reorderedLayout);
    addListings(reorderedLayout);
    m_innerTabs->addTab(reorderedPage, "Reordered Code");

    auto *analyzerPage = new QWidget(m_innerTabs);
    addAnalyzer(analyzerPage);
    m_innerTabs->addTab(analyzerPage, "Analyzer");

    layout->addWidget(m_innerTabs, 1);

    connect(m_optimizeButton, &QPushButton::clicked, this, &OptimizerTab::optimizeCurrentSource);
    connect(m_loadButton, &QPushButton::clicked, this, &OptimizerTab::loadOptimizedProgram);
}

void OptimizerTab::addHeading(QVBoxLayout *layout)
{
    auto *heading = new QLabel("Instruction Scheduler", this);
    QFont font = heading->font();
    font.setPointSize(18);
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);

    auto *subtitle = new QLabel(
        "Reorder instructions to cut pipeline stalls. Your editor source is never modified.", this);
    subtitle->setStyleSheet("color: palette(text); font-size: 10pt;");
    layout->addWidget(subtitle);
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
    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);

    m_optimizeButton = new QPushButton("Generate Suggestions", this);
    m_loadButton = new QPushButton("Execute Optimized Program", this);
    m_copyButton = new QPushButton("Copy Optimized Assembly", this);

    m_loadButton->setEnabled(false);
    m_copyButton->setEnabled(false);

    m_optimizeButton->setStyleSheet(kPrimaryButtonStyle);
    m_loadButton->setStyleSheet(kSecondaryButtonStyle);
    m_copyButton->setStyleSheet(kSecondaryButtonStyle);
    for (QPushButton *button : {m_optimizeButton, m_loadButton, m_copyButton})
    {
        button->setMinimumHeight(32);
        button->setCursor(Qt::PointingHandCursor);
    }

    toolbar->addWidget(m_optimizeButton);
    toolbar->addWidget(m_loadButton);
    toolbar->addWidget(m_copyButton);
    toolbar->addStretch(1);
    layout->addLayout(toolbar);

    connect(m_copyButton, &QPushButton::clicked,
            this, &OptimizerTab::copyOptimizedAssembly);
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
        "<span style='color:#2ea078'>&#9632;</span> moved earlier &nbsp;&nbsp;"
        "<span style='color:#e09628'>&#9632;</span> moved later &nbsp;&nbsp;"
        "Rows without a colour kept their position.<br>"
        "Dependency registers are bold with a coloured underline; each register keeps its colour. "
        "Click an instruction to draw its arrows and expand its details; click it again to "
        "collapse.<br>"
        "<b>RAW</b> read after write &middot; <b>WAR</b> write after read &middot; "
        "<b>WAW</b> write after write.",
        this);
    legend->setTextFormat(Qt::RichText);
    legend->setStyleSheet(kBannerStyle);
    legend->setWordWrap(true);
    layout->addWidget(legend);

    listingSplitter->setStretchFactor(0, 1);
    listingSplitter->setStretchFactor(1, 1);
    layout->addWidget(listingSplitter, 1);
}

void OptimizerTab::addAnalyzer(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(14);

    auto *profilerGroup = new QGroupBox("Profiler", page);
    auto *profilerGrid = new QGridLayout(profilerGroup);
    profilerGrid->setHorizontalSpacing(12);
    profilerGrid->setVerticalSpacing(10);

    auto makeMetric = [profilerGroup, profilerGrid](int row, int column,
                                                    const QString &caption,
                                                    QLabel **valueLabel) {
        auto *cell = new QWidget(profilerGroup);
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(10, 8, 10, 8);
        cellLayout->setSpacing(4);

        auto *name = new QLabel(caption, cell);
        name->setStyleSheet(
            "QLabel {"
            " color: palette(text);"
            " font-size: 12px;"
            " font-weight: 500;"
            " background: transparent;"
            " border: none;"
            "}"
        );
        *valueLabel = new QLabel("—", cell);
        QFont valueFont = (*valueLabel)->font();
        valueFont.setPointSize(valueFont.pointSize() + 3);
        valueFont.setBold(true);
        (*valueLabel)->setFont(valueFont);

        cellLayout->addWidget(name);
        cellLayout->addWidget(*valueLabel);
        cell->setObjectName("metricCard");
        cell->setStyleSheet(
            "QWidget#metricCard { border: 1px solid palette(mid); border-radius: 6px; "
            "background: palette(base); }");
        profilerGrid->addWidget(cell, row, column);
    };

    makeMetric(0, 0, "Instructions", &m_instructionCountLabel);
    makeMetric(0, 1, "Instructions moved", &m_movedCountLabel);
    makeMetric(1, 0, "RAW dependencies", &m_rawCountLabel);
    makeMetric(1, 1, "WAR dependencies", &m_warCountLabel);
    makeMetric(1, 2, "WAW dependencies", &m_wawCountLabel);
    profilerGrid->setColumnStretch(0, 1);
    profilerGrid->setColumnStretch(1, 1);
    profilerGrid->setColumnStretch(2, 1);
    layout->addWidget(profilerGroup);

    auto *benchmarkGroup = new QGroupBox("Benchmarker", page);
    auto *benchmarkLayout = new QVBoxLayout(benchmarkGroup);
    auto *benchmarkToolbar = new QHBoxLayout();
    benchmarkToolbar->addWidget(new QLabel(
        "Compare the original and optimized program using the selected hazard model.",
        benchmarkGroup));
    benchmarkToolbar->addStretch(1);

    m_benchmarkButton = new QPushButton("Benchmark Original vs Optimized", benchmarkGroup);
    m_benchmarkButton->setEnabled(false);
    benchmarkToolbar->addWidget(m_benchmarkButton);
    benchmarkLayout->addLayout(benchmarkToolbar);

    m_benchmarkSummaryLabel = new QLabel(
        "No benchmark has been run yet. Generate suggestions, then run the benchmark.",
        benchmarkGroup);
    m_benchmarkSummaryLabel->setWordWrap(true);
    m_benchmarkSummaryLabel->setTextFormat(Qt::RichText);
    m_benchmarkSummaryLabel->setStyleSheet(
        "QLabel { border: 1px solid palette(mid); border-radius: 6px; "
        "padding: 12px; background: palette(base); }");
    benchmarkLayout->addWidget(m_benchmarkSummaryLabel);
    layout->addWidget(benchmarkGroup);
    layout->addStretch(1);

    connect(m_benchmarkButton, &QPushButton::clicked,
            this, &OptimizerTab::benchmarkOptimizedProgram);
}

void OptimizerTab::updateDependencyAnalysis(const AssembledProgram &program)
{
    const DependencyCounts counts = countDependencies(program);
    if (m_instructionCountLabel)
        m_instructionCountLabel->setText(
            QString::number(static_cast<qulonglong>(program.intermediate_code.size())));
    if (m_rawCountLabel)
        m_rawCountLabel->setText(QString::number(static_cast<qulonglong>(counts.raw)));
    if (m_warCountLabel)
        m_warCountLabel->setText(QString::number(static_cast<qulonglong>(counts.war)));
    if (m_wawCountLabel)
        m_wawCountLabel->setText(QString::number(static_cast<qulonglong>(counts.waw)));
}

void OptimizerTab::copyOptimizedAssembly()
{
    if (!m_optimizedProgram)
        return;

    // Reconstruct pasteable assembly from the scheduled IC stream. Emit labels
    // at their symbol-table target addresses, then emit one instruction per line.
    std::map<std::size_t, QStringList> labelsAtIndex;
    for (const auto &[name, symbol] : m_optimizedProgram->symbol_table)
    {
        if (symbol.isData || (symbol.address % 4) != 0)
            continue;
        const auto index = static_cast<std::size_t>(symbol.address / 4);
        if (index < m_optimizedProgram->intermediate_code.size())
            labelsAtIndex[index].append(QString::fromStdString(name) + ":");
    }

    QStringList lines;
    for (std::size_t i = 0; i < m_optimizedProgram->intermediate_code.size(); ++i)
    {
        for (const auto &label : labelsAtIndex[i])
            lines.append(label);

        const auto &instruction = m_optimizedProgram->intermediate_code[i].first;
        std::ostringstream stream;
        stream << instruction;
        QString instructionText = QString::fromStdString(stream.str()).trimmed();

        // ICUnit's debug formatter appends its internal label as "<name>".
        // Labels are emitted above in assembler syntax, so strip that suffix.
        if (!instruction.label.empty())
        {
            const QString suffix = " <" + QString::fromStdString(instruction.label) + ">";
            if (instructionText.endsWith(suffix))
                instructionText.chop(suffix.size());
        }

        if (!instructionText.isEmpty())
            lines.append("    " + instructionText);
    }

    QApplication::clipboard()->setText(lines.join('\n'));
    m_statusLabel->setText("Optimized assembly copied to clipboard.");
}

InstructionListingTextEdit *OptimizerTab::createListingPanel(QSplitter *splitter, const QString &title,
                                                 const QString &placeholder,
                                                 InstructionListingHighlighter **highlighter)
{
    auto *panel = new QWidget(splitter);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(6);

    auto *titleLabel = new QLabel(title, panel);
    titleLabel->setStyleSheet("font-weight: 600; font-size: 11pt;");
    panelLayout->addWidget(titleLabel);

    auto *output = new InstructionListingTextEdit(panel);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setPlaceholderText(placeholder);
    output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output->setStyleSheet("QPlainTextEdit { padding: 8px; border: 1px solid palette(midlight);"
                          " border-radius: 6px; }");
    panelLayout->addWidget(output, 1);

    // Expandable details for the clicked instruction. It stays collapsed (zero
    // height) until a row is selected, then slides open to fit its content.
    auto *details = new QScrollArea(panel);
    details->setObjectName("dependencyDetails");
    details->setWidgetResizable(true);
    details->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    details->setStyleSheet("QScrollArea#dependencyDetails { border: 1px solid palette(mid);"
                           " border-radius: 6px; background: palette(base); }"
                           "QScrollArea#dependencyDetails > QWidget > QWidget { background: palette(base); }");
    details->setFixedHeight(0);

    auto *detailsLabel = new QLabel(details);
    detailsLabel->setTextFormat(Qt::RichText);
    detailsLabel->setWordWrap(true);
    detailsLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    detailsLabel->setMargin(10);
    detailsLabel->setStyleSheet("QLabel { background: transparent; color: palette(text); }");
    details->setWidget(detailsLabel);
    panelLayout->addWidget(details, 0);

    auto *animation = new QVariantAnimation(details);
    animation->setDuration(160);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QVariantAnimation::valueChanged, details,
            [details](const QVariant &value) { details->setFixedHeight(value.toInt()); });

    output->setSelectionChangedCallback([details, detailsLabel, animation](const QString &html) {
        int target = 0;
        if (!html.isEmpty())
        {
            detailsLabel->setText(html);
            const int width = std::max(1, details->viewport()->width());
            const int content = std::max(detailsLabel->sizeHint().height(),
                                         detailsLabel->heightForWidth(width));
            target = std::min(kDetailsMaxHeight, content + 2 * details->frameWidth() + 4);
        }
        animation->stop();
        animation->setStartValue(details->height());
        animation->setEndValue(target);
        animation->start();
    });

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
        updateDependencyAnalysis(original);
        if (m_movedCountLabel)
            m_movedCountLabel->setText(
                QString::number(static_cast<qulonglong>(schedule.moved_instructions)));
        if (m_benchmarkSummaryLabel)
            m_benchmarkSummaryLabel->setText("No benchmark has been run yet.");

        m_originalProgram = std::move(original);
        m_optimizedProgram = std::move(optimized);
        m_optimizedWithForwarding = forwardingEnabled;

        m_loadButton->setEnabled(true);
        m_copyButton->setEnabled(true);
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
    m_copyButton->setEnabled(false);
    m_benchmarkButton->setEnabled(false);
    m_originalOutput->clear();
    m_optimizedOutput->clear();
    m_originalOutput->setDependencies({}, {});
    m_optimizedOutput->setDependencies({}, {});
    m_originalOutput->setInstructionDetails({});
    m_optimizedOutput->setInstructionDetails({});
    m_originalSourceLineBlocks.clear();
    m_optimizedSourceLineBlocks.clear();
    for (QLabel *label : {m_instructionCountLabel, m_movedCountLabel,
                          m_rawCountLabel, m_warCountLabel, m_wawCountLabel})
        if (label)
            label->setText("—");
    if (m_benchmarkSummaryLabel)
        m_benchmarkSummaryLabel->setText(
            "No benchmark has been run yet. Generate suggestions, then run the benchmark.");
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

    // -1 = the instruction now sits earlier than in the original, +1 = later.
    std::vector<int> originalMovement(original.intermediate_code.size(), 0);
    std::vector<int> optimizedMovement(optimized.intermediate_code.size(), 0);
    for (std::size_t i = 0; i < optimizedMovement.size() && i < positions.size(); ++i)
    {
        const int from = positions[i];
        if (from < 0 || from == static_cast<int>(i))
            continue;
        optimizedMovement[i] = from > static_cast<int>(i) ? -1 : 1;
        if (static_cast<std::size_t>(from) < originalMovement.size())
            originalMovement[static_cast<std::size_t>(from)] = optimizedMovement[i];
    }

    m_originalOutput->setInstructionDetails(
        instructionDetailsFor(original, nullptr, originalMovement));
    m_optimizedOutput->setInstructionDetails(
        instructionDetailsFor(optimized, &positions, optimizedMovement));
}

void OptimizerTab::handleOptimizationFailure(const QString &reason)
{
    m_originalProgram.reset();
    m_benchmarkButton->setEnabled(false);

    m_optimizedProgram.reset();
    m_loadButton->setEnabled(false);
    m_copyButton->setEnabled(false);
    m_originalOutput->clear();
    m_optimizedOutput->clear();

    m_originalOutput->setDependencies({}, {});
    m_optimizedOutput->setDependencies({}, {});
    m_originalOutput->setInstructionDetails({});
    m_optimizedOutput->setInstructionDetails({});
    for (QLabel *label : {m_instructionCountLabel, m_movedCountLabel,
                          m_rawCountLabel, m_warCountLabel, m_wawCountLabel})
        if (label)
            label->setText("—");
    if (m_benchmarkSummaryLabel)
        m_benchmarkSummaryLabel->setText("Optimization failed; no analysis available.");

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
    const auto percentChange = [](unsigned int before, unsigned int after) {
        if (before == 0)
            return QString("n/a");
        const double change =
            (static_cast<double>(after) - static_cast<double>(before)) * 100.0 /
            static_cast<double>(before);
        return QString("%1%2%")
            .arg(change > 0.0 ? "+" : "")
            .arg(change, 0, 'f', 1);
    };

    if (m_benchmarkSummaryLabel)
    {
        const QString table =
            "<table width='100%' cellspacing='0' cellpadding='8'>"
            "<tr><th align='left'></th><th align='left'>Original</th>"
            "<th align='left'>Optimized</th><th align='left'>Change</th></tr>"
            "<tr><td><b>Cycles</b></td><td>%1</td><td>%2</td><td>%3</td></tr>"
            "<tr><td><b>Stalls</b></td><td>%4</td><td>%5</td><td>%6</td></tr>"
            "<tr><td><b>Instructions retired</b></td><td>%7</td><td>%8</td><td>—</td></tr>"
            "<tr><td><b>CPI</b></td><td>%9</td><td>%10</td><td>—</td></tr>"
            "</table>"
            "<p><b>Integrity check:</b> %11</p>"
            "<style>td,th { border-bottom: 1px solid palette(mid); }</style>";

        const QString integrity =
            original.instructionsRetired == optimized.instructionsRetired
                ? "PASS — retired instruction counts match"
                : "WARNING — retired instruction counts differ";

        m_benchmarkSummaryLabel->setText(
            table.arg(original.cycles)
                .arg(optimized.cycles)
                .arg(percentChange(original.cycles, optimized.cycles))
                .arg(original.stalls)
                .arg(optimized.stalls)
                .arg(percentChange(original.stalls, optimized.stalls))
                .arg(original.instructionsRetired)
                .arg(optimized.instructionsRetired)
                .arg(QString::number(original.cpi, 'f', 3))
                .arg(QString::number(optimized.cpi, 'f', 3))
                .arg(integrity));
    }

    m_statusLabel->setText("Benchmark complete. Results are available in the Analyzer tab.");
    if (m_innerTabs)
        m_innerTabs->setCurrentIndex(1);
}

} // namespace Kites
