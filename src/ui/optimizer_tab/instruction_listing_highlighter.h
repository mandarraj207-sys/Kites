#pragma once

#include <QColor>
#include <QSyntaxHighlighter>
#include <QString>
#include <QTextCharFormat>

#include <map>
#include <vector>

class QRegularExpression;

namespace Kites {

struct DependencyRegisterStyle
{
    bool written = false;
    bool read = false;
};

using DependencyRegisterMap = std::map<int, std::map<QString, DependencyRegisterStyle>>;

struct DependencySpan
{
    int start = 0;
    int length = 0;
    QColor color;
    bool write = false;
};

/** Text every stall (pipeline bubble) row starts with, right after the " | " column separator. */
inline const QString kStallRowMarker = QStringLiteral("-- stall");

/** True for the rows the listing inserts for pipeline bubbles. */
bool isStallRow(const QString &text);

/** The color a register keeps everywhere it appears in a dependency (underline, arrows). */
QColor dependencyColor(const QString &reg);

/** Colors opcodes, registers and immediates, and styles registers that take part in a dependency. */
class InstructionListingHighlighter : public QSyntaxHighlighter
{
public:
    explicit InstructionListingHighlighter(QTextDocument *document);

    void setDependencyRegisters(const DependencyRegisterMap &registersByBlock);

    std::vector<DependencySpan> dependencySpans(const QString &text, int blockNumber) const;

protected:
    void highlightBlock(const QString &text) override;

private:
    void applyRegex(const QString &text, const QRegularExpression &regex,
                    const QTextCharFormat &format);

    DependencyRegisterMap m_dependencyRegistersByBlock;
};

} // namespace Kites
