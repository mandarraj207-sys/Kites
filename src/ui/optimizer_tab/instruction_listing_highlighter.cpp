#include "ui/optimizer_tab/instruction_listing_highlighter.h"

#include <QColor>
#include <QFont>
#include <QRegularExpression>

#include <array>

namespace Kites {
namespace {

// Opacity (0-255) of the filled chip behind a consumer (source) register.
constexpr int kConsumerChipAlpha = 45;

const QRegularExpression &registerRegex()
{
    static const QRegularExpression regex(
        R"(\b(?:x(?:[0-9]|[12][0-9]|3[01])|zero|ra|sp|gp|tp|t[0-6]|s(?:[0-9]|1[01])|a[0-7])\b)",
        QRegularExpression::CaseInsensitiveOption);
    return regex;
}

} // namespace

// Each register keeps the same colour everywhere in the listing.
QColor dependencyColor(const QString &reg)
{
    static const std::array<QColor, 8> palette = {
        QColor(224, 82, 82), QColor(66, 133, 212), QColor(229, 138, 181),
        QColor(239, 203, 104), QColor(165, 204, 107), QColor(243, 167, 18),
        QColor(228, 87, 46), QColor(151, 112, 205)};

    unsigned int hash = 0;
    for (const QChar ch : reg)
        hash = hash * 31u + ch.unicode();
    return palette[hash % palette.size()];
}

InstructionListingHighlighter::InstructionListingHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
}

void InstructionListingHighlighter::setDependencyRegisters(
    const DependencyRegisterMap &registersByBlock)
{
    m_dependencyRegistersByBlock = registersByBlock;
    rehighlight();
}

void InstructionListingHighlighter::highlightBlock(const QString &text)
{

    QTextCharFormat opcodeFormat;
    opcodeFormat.setForeground(QColor("#569CD6"));
    opcodeFormat.setFontWeight(QFont::DemiBold);

    QTextCharFormat registerFormat;
    registerFormat.setForeground(QColor("#4EC9B0"));

    QTextCharFormat immediateFormat;
    immediateFormat.setForeground(QColor("#CE9178"));

    static const QRegularExpression opcodeRegex(
        R"(\b(addi|add|sub|mul|and|or|xor|sll|srl|sra|slt|sltu|andi|ori|xori|slti|sltiu|lb|lh|lw|ld|lbu|lhu|lwu|sb|sh|sw|sd|lui|auipc|nop|mv|li|la|csrrw|csrrs|csrrc|csrrwi|csrrsi|csrrci|beq|bne|blt|bge|bltu|bgeu|jal|jalr)\b)",
        QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression numberRegex(R"(\b-?(?:0x[0-9a-fA-F]+|[0-9]+)\b)");

    applyRegex(text, opcodeRegex, opcodeFormat);
    applyRegex(text, registerRegex(), registerFormat);
    applyRegex(text, numberRegex, immediateFormat);

    // Every register that takes part in a dependency (producer or consumer)
    // gets the same look: bold, filled chip + coloured underline.
    for (const DependencySpan &span : dependencySpans(text, currentBlock().blockNumber()))
    {
        QTextCharFormat dependencyFormat;
        dependencyFormat.setFontWeight(QFont::Bold);
        dependencyFormat.setUnderlineStyle(QTextCharFormat::SingleUnderline);
        dependencyFormat.setUnderlineColor(span.color);
        dependencyFormat.setBackground(QColor(span.color.red(), span.color.green(),
                                              span.color.blue(), kConsumerChipAlpha));
        setFormat(span.start, span.length, dependencyFormat);
    }
}

std::vector<DependencySpan> InstructionListingHighlighter::dependencySpans(const QString &text,
                                                                           int blockNumber) const
{
    std::vector<DependencySpan> spans;
    const auto block = m_dependencyRegistersByBlock.find(blockNumber);
    if (block == m_dependencyRegistersByBlock.end())
        return spans;

    // Locate the assembly column. The listing has fixed-width metadata
    // columns before the final " | " separator.
    const int assemblyStart = text.lastIndexOf(" | ") + 3;
    const QString assembly = text.mid(assemblyStart).trimmed();
    const int firstSpace = assembly.indexOf(QRegularExpression("\\s"));
    const QString opcode = (firstSpace < 0 ? assembly : assembly.left(firstSpace)).toLower();

    auto matches = registerRegex().globalMatch(text);
    while (matches.hasNext())
    {
        const auto match = matches.next();
        if (match.capturedStart() < assemblyStart)
            continue;

        const QString reg = match.captured();
        const auto dependency = block->second.find(reg);
        if (dependency == block->second.end())
            continue;

        // Determine the operand number so an instruction such as
        // "addi x6, x6, 1" can treat rd as the producer and rs1 as a
        // consumer even though both operands name the same register.
        int operandIndex = 0;
        for (int pos = assemblyStart; pos < match.capturedStart(); ++pos)
            if (text.at(pos) == ',')
                ++operandIndex;

        const bool isStore = opcode == "sb" || opcode == "sh" ||
                             opcode == "sw" || opcode == "sd";
        const bool isBranch = opcode == "beq" || opcode == "bne" ||
                              opcode == "blt" || opcode == "bge" ||
                              opcode == "bltu" || opcode == "bgeu";
        const bool writesOperand = !isStore && !isBranch && operandIndex == 0 &&
                                   opcode != "nop";
        const bool useWriteStyle = writesOperand && dependency->second.written;
        const bool useReadStyle = !writesOperand && dependency->second.read;
        if (!useWriteStyle && !useReadStyle)
            continue;

        spans.push_back({static_cast<int>(match.capturedStart()),
                         static_cast<int>(match.capturedLength()), dependencyColor(reg),
                         useWriteStyle});
    }
    return spans;
}

void InstructionListingHighlighter::applyRegex(const QString &text,
                                               const QRegularExpression &regex,
                                               const QTextCharFormat &format)
{
    auto match = regex.globalMatch(text);
    while (match.hasNext())
    {
        const auto item = match.next();
        setFormat(item.capturedStart(), item.capturedLength(), format);
    }
}

} // namespace Kites
