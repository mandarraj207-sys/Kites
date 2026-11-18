#pragma once

#include "ui/common/kitestab.h"
#include "common/assembled_program.h"
#include "ui/optimizer_tab/optimizer_benchmark_metrics.h"

#include <functional>
#include <optional>
#include <map>
#include <vector>
#include <utility>

class QSpinBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSplitter;
class QTabWidget;
class QWidget;
class QVBoxLayout;

namespace Kites {

class EditorTab;
class InstructionListingHighlighter;
class InstructionListingTextEdit;

class OptimizerTab : public KitesTab
{
    Q_OBJECT

public:
    explicit OptimizerTab(EditorTab *editor, QWidget *parent = nullptr);

    void setLoadHandler(
        std::function<void(const AssembledProgram &, bool)> handler);

    void setBenchmarkHandler(
        std::function<void(const AssembledProgram &, const AssembledProgram &, bool)> handler);

    // Mirrors the editor tab pipeline-stage highlight in both instruction listings.
    void setExecutionHighlights(const std::vector<std::pair<int, std::string>> &editorLines);

    void showBenchmarkResults(
        const OptimizerBenchmarkMetrics &original,
        const OptimizerBenchmarkMetrics &optimized);

private:
    void addHeading(QVBoxLayout *layout);
    void addHazardModelSelector(QVBoxLayout *layout);
    void addActionButtons(QVBoxLayout *layout);
    void addListings(QVBoxLayout *layout);
    void addAnalyzer(QWidget *page);
    void updateDependencyAnalysis(const AssembledProgram &program);
    void copyOptimizedAssembly();
    InstructionListingTextEdit *createListingPanel(QSplitter *splitter, const QString &title,
                                       const QString &placeholder,
                                       InstructionListingHighlighter **highlighter);

    void optimizeCurrentSource();
    void loadOptimizedProgram();
    void benchmarkOptimizedProgram();
    bool ensureSuggestionsMatchSelectedModel();

    void resetResults();
    void showListings(const AssembledProgram &original, const AssembledProgram &optimized);
    void handleOptimizationFailure(const QString &reason);

    EditorTab *m_editor = nullptr;
    QComboBox *m_hazardModelComboBox = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTabWidget *m_innerTabs = nullptr;
    QLabel *m_instructionCountLabel = nullptr;
    QLabel *m_movedCountLabel = nullptr;
    QLabel *m_rawCountLabel = nullptr;
    QLabel *m_warCountLabel = nullptr;
    QLabel *m_wawCountLabel = nullptr;
    QLabel *m_benchmarkSummaryLabel = nullptr;
    InstructionListingTextEdit *m_originalOutput = nullptr;
    InstructionListingTextEdit *m_optimizedOutput = nullptr;
    InstructionListingHighlighter *m_originalHighlighter = nullptr;
    InstructionListingHighlighter *m_optimizedHighlighter = nullptr;
    std::map<int, std::vector<int>> m_originalSourceLineBlocks;
    std::map<int, std::vector<int>> m_optimizedSourceLineBlocks;

    QPushButton *m_optimizeButton = nullptr;
    QPushButton *m_loadButton = nullptr;
    QPushButton *m_benchmarkButton = nullptr;
    QPushButton *m_copyButton = nullptr;

    std::optional<AssembledProgram> m_originalProgram;
    std::optional<AssembledProgram> m_optimizedProgram;
    bool m_optimizedWithForwarding = false;

    std::function<void(const AssembledProgram &, bool)> m_loadHandler;

    std::function<void(
        const AssembledProgram &,
        const AssembledProgram &,
        bool)> m_benchmarkHandler;
};

} // namespace Kites
