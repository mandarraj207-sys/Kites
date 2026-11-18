#include "ui/optimizer_tab/benchmark_results_dialog.h"

#include <QAbstractItemView>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace Kites {
namespace {

const QString kImprovedColor = "#21865b";
const QString kRegressedColor = "#c04b45";

const QString kTableStyle =
    "QTableWidget {"
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
    "}";

const QString kBarStyle =
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

QString changeText(long long before, long long after)
{
    const long long delta = before - after;
    const double percent = before > 0 ? 100.0 * static_cast<double>(delta) / before : 0.0;

    if (delta > 0)
        return QString("%1 fewer (%2% reduction)").arg(delta).arg(percent, 0, 'f', 2);

    if (delta < 0)
        return QString("%1 more (%2% increase)").arg(-delta).arg(-percent, 0, 'f', 2);

    return QString("No change (0%)");
}

const QString &accentFor(bool improved)
{
    return improved ? kImprovedColor : kRegressedColor;
}

} // namespace

BenchmarkResultsDialog::BenchmarkResultsDialog(const OptimizerBenchmarkMetrics &original,
                                               const OptimizerBenchmarkMetrics &optimized,
                                               QWidget *parent)
    : QDialog(parent), m_original(original), m_optimized(optimized)
{
    setWindowTitle("Benchmark Results");
    // Use the active Qt palette for readable text in light and dark themes.
    setStyleSheet(
        "QDialog { color: palette(text); }"
        "QLabel { color: palette(text); }"
    );
    setMinimumSize(680, 540);
    resize(760, 620);

    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(24, 22, 24, 22);
    m_root->setSpacing(14);

    addHeading();
    addSummaryCards();
    addMetricTable();
    addBarComparison("Cycles", m_original.cycles, m_optimized.cycles);
    addBarComparison("Stall cycles", m_original.stalls, m_optimized.stalls);
    addIntegrityStatus();
    addCloseButton();
}

void BenchmarkResultsDialog::addHeading()
{
    auto *heading = new QLabel("Performance comparison", this);
    QFont headingFont = heading->font();
    headingFont.setPointSize(18);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    m_root->addWidget(heading);

    auto *subtitle = new QLabel(QString("Original program vs. optimized program"), this);
    subtitle->setStyleSheet("color: palette(text); font-size: 10pt;");
    m_root->addWidget(subtitle);
}

void BenchmarkResultsDialog::addSummaryCards()
{
    auto *cards = new QGridLayout();
    cards->setSpacing(10);
    m_root->addLayout(cards);

    const long long cyclesSaved = static_cast<long long>(m_original.cycles) - m_optimized.cycles;
    const long long stallsSaved = static_cast<long long>(m_original.stalls) - m_optimized.stalls;
    const double cpiImprovement =
        m_original.cpi > 0.0 ? 100.0 * (m_original.cpi - m_optimized.cpi) / m_original.cpi : 0.0;

    addCard(cards, 0, "CYCLES SAVED", QString::number(cyclesSaved),
            changeText(m_original.cycles, m_optimized.cycles), accentFor(cyclesSaved >= 0));

    addCard(cards, 1, "STALL CYCLES SAVED", QString::number(stallsSaved),
            changeText(m_original.stalls, m_optimized.stalls), accentFor(stallsSaved >= 0));

    addCard(cards, 2, "CPI IMPROVEMENT", QString("%1%").arg(cpiImprovement, 0, 'f', 2),
            QString("%1 → %2").arg(m_original.cpi, 0, 'f', 3).arg(m_optimized.cpi, 0, 'f', 3),
            accentFor(cpiImprovement >= 0));
}

void BenchmarkResultsDialog::addCard(QGridLayout *cards, int column, const QString &title,
                                     const QString &value, const QString &detail,
                                     const QString &accent)
{
    auto *card = new QFrame(this);
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
}

void BenchmarkResultsDialog::addMetricTable()
{
    auto *comparisonTitle = new QLabel("Metric breakdown", this);
    QFont sectionFont = comparisonTitle->font();
    sectionFont.setBold(true);
    sectionFont.setPointSize(12);
    comparisonTitle->setFont(sectionFont);
    m_root->addWidget(comparisonTitle);

    auto *table = new QTableWidget(4, 3, this);
    table->setStyleSheet(kTableStyle);
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

    const QStringList names = {"Cycles", "Stall cycles", "Executed instructions", "CPI"};
    const QStringList originalValues = {
        QString::number(m_original.cycles), QString::number(m_original.stalls),
        QString::number(m_original.instructionsRetired), QString::number(m_original.cpi, 'f', 3)};
    const QStringList optimizedValues = {
        QString::number(m_optimized.cycles), QString::number(m_optimized.stalls),
        QString::number(m_optimized.instructionsRetired), QString::number(m_optimized.cpi, 'f', 3)};

    for (int row = 0; row < names.size(); ++row)
    {
        table->setItem(row, 0, new QTableWidgetItem(names[row]));
        table->setItem(row, 1, new QTableWidgetItem(originalValues[row]));
        table->setItem(row, 2, new QTableWidgetItem(optimizedValues[row]));
        table->item(row, 1)->setTextAlignment(Qt::AlignCenter);
        table->item(row, 2)->setTextAlignment(Qt::AlignCenter);
    }
    m_root->addWidget(table);
}

void BenchmarkResultsDialog::addBarComparison(const QString &label, unsigned int originalValue,
                                              unsigned int optimizedValue)
{
    auto *rowWidget = new QWidget(this);
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
    originalBar->setStyleSheet(kBarStyle);
    optimizedBar->setStyleSheet(kBarStyle);

    rowLayout->addWidget(name, 0, 0, 2, 1);
    rowLayout->addWidget(originalBar, 0, 1);
    rowLayout->addWidget(optimizedBar, 1, 1);
    m_root->addWidget(rowWidget);
}

void BenchmarkResultsDialog::addIntegrityStatus()
{
    const bool sameRetiredCount = m_original.instructionsRetired == m_optimized.instructionsRetired;

    auto *status = new QLabel(
        sameRetiredCount
            ? QString("Instruction count matches: %1 retired in each run. This is a useful check, "
                      "but not a complete proof of equivalent program state.")
                  .arg(m_original.instructionsRetired)
            : QString("Warning: retired instruction counts differ (%1 original, %2 optimized). "
                      "Investigate correctness before trusting the performance comparison.")
                  .arg(m_original.instructionsRetired)
                  .arg(m_optimized.instructionsRetired),
        this);
    status->setWordWrap(true);
    status->setStyleSheet(
        sameRetiredCount ? "padding: 9px; border-radius: 6px; background: palette(alternate-base);"
                         : "padding: 9px; border-radius: 6px; color: #c04b45; background: "
                           "palette(alternate-base);");
    m_root->addWidget(status);
}

void BenchmarkResultsDialog::addCloseButton()
{
    auto *closeButton = new QPushButton("Close", this);
    closeButton->setMinimumWidth(100);
    auto *buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    buttonRow->addWidget(closeButton);
    m_root->addLayout(buttonRow);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

} // namespace Kites
