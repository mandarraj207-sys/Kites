#pragma once

#include "ui/optimizer_tab/optimizer_benchmark_metrics.h"

#include <QDialog>

class QGridLayout;
class QVBoxLayout;

namespace Kites {

/** Modal comparison of original vs. optimized benchmark metrics. */
class BenchmarkResultsDialog : public QDialog
{
public:
    BenchmarkResultsDialog(const OptimizerBenchmarkMetrics &original,
                           const OptimizerBenchmarkMetrics &optimized,
                           QWidget *parent = nullptr);

private:
    void addHeading();
    void addSummaryCards();
    void addCard(QGridLayout *cards, int column, const QString &title, const QString &value,
                 const QString &detail, const QString &accent);
    void addMetricTable();
    void addBarComparison(const QString &label, unsigned int originalValue,
                          unsigned int optimizedValue);
    void addIntegrityStatus();
    void addCloseButton();

    OptimizerBenchmarkMetrics m_original;
    OptimizerBenchmarkMetrics m_optimized;
    QVBoxLayout *m_root = nullptr;
};

} // namespace Kites
