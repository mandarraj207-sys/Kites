
#pragma once

#include "ui/common/kitestab.h"
#include "common/assembled_program.h"

#include <functional>
#include <optional>

class QSpinBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace Kites {

class EditorTab;

struct OptimizerBenchmarkMetrics
{
    unsigned int cycles = 0;
    unsigned int stalls = 0;
    unsigned int instructionsRetired = 0;
    float cpi = 0.0f;
};

class OptimizerTab : public KitesTab
{
    Q_OBJECT

public:
    explicit OptimizerTab(EditorTab *editor, QWidget *parent = nullptr);

    void setLoadHandler(
        std::function<void(const AssembledProgram &)> handler);

    void setBenchmarkHandler(
        std::function<void(const AssembledProgram &, const AssembledProgram &, bool)> handler);
    
    void showBenchmarkResults(
        const OptimizerBenchmarkMetrics &original,
        const OptimizerBenchmarkMetrics &optimized);

private:
    void optimizeCurrentSource();
    void loadOptimizedProgram();
    void benchmarkOptimizedProgram();

    EditorTab *m_editor = nullptr;
    QComboBox *m_hazardModelComboBox = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPlainTextEdit *m_originalOutput = nullptr;
    QPlainTextEdit *m_optimizedOutput = nullptr;

    QPushButton *m_optimizeButton = nullptr;
    QPushButton *m_loadButton = nullptr;
    QPushButton *m_benchmarkButton = nullptr;

    std::optional<AssembledProgram> m_originalProgram;
    std::optional<AssembledProgram> m_optimizedProgram;
    bool m_optimizedWithForwarding = false;

    std::function<void(const AssembledProgram &)> m_loadHandler;

    std::function<void(
        const AssembledProgram &,
        const AssembledProgram &,
        bool)> m_benchmarkHandler;
};

} // namespace Kites
