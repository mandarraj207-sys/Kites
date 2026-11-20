/**
 * @file program_optimizer.cpp
 * @brief Turns editor source into an assembled program and rebuilds it in scheduled order.
 */
#include "optimizer/program_optimizer.h"

#include <QDebug>
#include <QString>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "assembler/assembler.h"
#include "custom_pseudo_manager/custom_pseudo_manager.h"
#include "optimizer/jalr_handler.h"
#include "optimizer/forwarding_instruction_scheduler.h"
#include "optimizer/scheduler_core.h"

namespace Kites::optimizer
{
namespace
{
void writeTemporarySource(const std::filesystem::path &path, const std::string &source)
{
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Could not create the temporary assembly file.");

    out << source;
    if (!out)
        throw std::runtime_error("Could not write the temporary assembly file.");
}

/**
 * Maps expanded pseudo-instruction lines back to the user's source line, so a
 * pseudo that expands to several instructions highlights one editor line.
 */
void remapToOriginalLines(AssembledProgram &program,
                          const std::vector<int> &expandedLineToOriginalLine)
{
    program.line_number_instruction_number_mapping.clear();
    program.instruction_number_line_number_mapping.clear();

    for (std::size_t i = 0; i < program.intermediate_code.size(); ++i)
    {
        auto &instruction = program.intermediate_code[i].first;
        const std::size_t expandedIndex =
            instruction.line_number > 0 ? static_cast<std::size_t>(instruction.line_number - 1)
                                        : expandedLineToOriginalLine.size();
        if (expandedIndex < expandedLineToOriginalLine.size())
            instruction.line_number =
                static_cast<unsigned int>(expandedLineToOriginalLine[expandedIndex]);

        program.line_number_instruction_number_mapping[instruction.line_number] =
            static_cast<unsigned int>(i);
        program.instruction_number_line_number_mapping[static_cast<unsigned int>(i)] =
            instruction.line_number;
    }
}

std::vector<ICUnit> instructionsOf(const AssembledProgram &program)
{
    std::vector<ICUnit> instructions;
    instructions.reserve(program.intermediate_code.size());
    for (const auto &entry : program.intermediate_code)
        instructions.push_back(entry.first);
    return instructions;
}

void appendScheduledRegion(ScheduleResult &result, const std::vector<ICUnit> &instructions,
                           std::size_t begin, std::size_t end, bool forwardingEnabled)
{
    if (begin >= end)
        return;

    const std::vector<ICUnit> region(instructions.begin() + static_cast<std::ptrdiff_t>(begin),
                                     instructions.begin() + static_cast<std::ptrdiff_t>(end));

    qDebug() << "[Optimizer] Forwarding enabled:" << forwardingEnabled;
    qDebug() << "[Optimizer] Scheduler selected:" << (forwardingEnabled ? "H_F" : "H_NF");

    const auto regionResult = forwardingEnabled ? scheduleForwardingBasicBlock(region)
                                                : scheduleBasicBlock(region);

    result.moved_instructions += regionResult.moved_instructions;
    result.instructions.insert(result.instructions.end(), regionResult.instructions.begin(),
                               regionResult.instructions.end());
}

void printInstructions(const std::vector<ICUnit> &instructions, const char *title)
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

void logSchedule(std::size_t originalCount, const ScheduleResult &result)
{
    printInstructions(result.instructions, "AFTER SCHEDULING");

    qDebug() << "Original instruction count:" << originalCount;
    qDebug() << "Scheduled instruction count:" << result.instructions.size();
    qDebug() << "Instructions moved:" << result.moved_instructions;

    for (std::size_t i = 0; i < result.instructions.size(); ++i)
    {
        std::ostringstream line;
        line << result.instructions[i];

        qDebug() << "Scheduled" << i << ":" << QString::fromStdString(line.str());
    }
}

bool sameInstruction(const ICUnit &a, const ICUnit &b)
{
    return a.line_number == b.line_number && a.opcode == b.opcode && a.rd == b.rd &&
           a.rs1 == b.rs1 && a.rs2 == b.rs2 && a.rs3 == b.rs3 && a.imm == b.imm &&
           a.csr == b.csr && a.rm == b.rm && a.label == b.label;
}

/** Finds and claims the first unused entry equal to `scheduled`, so repeats match one-to-one. */
std::optional<std::size_t> claimMatchingEntry(const AssembledProgram &program,
                                              std::vector<bool> &used, const ICUnit &scheduled)
{
    for (std::size_t i = 0; i < program.intermediate_code.size(); ++i)
    {
        if (used[i])
            continue;

        if (sameInstruction(program.intermediate_code[i].first, scheduled))
        {
            used[i] = true;
            return i;
        }
    }
    return std::nullopt;
}

void rebuildIndexMetadata(AssembledProgram &program)
{
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
}

} // namespace

/**
 * Marks instructions that code labels point at. A symbol address is in bytes
 * and KITES stores one 32-bit instruction per entry, so address / 4 is the index.
 */
std::vector<bool> findLabelBoundaries(const AssembledProgram &program, std::size_t instructionCount)
{
    std::vector<bool> labelBoundary(instructionCount, false);
    for (const auto &symbolEntry : program.symbol_table)
    {
        const auto &symbol = symbolEntry.second;
        if (symbol.isData || (symbol.address % 4) != 0)
            continue;

        const std::size_t targetIndex = static_cast<std::size_t>(symbol.address / 4);
        if (targetIndex < labelBoundary.size())
            labelBoundary[targetIndex] = true;
    }
    return labelBoundary;
}

AssembledProgram assembleEditorSource(const std::string &rawSource)
{
    std::vector<int> expandedLineToOriginalLine;
    const std::string source =
        customPseudoManager::expandPseudoInstruction(rawSource, expandedLineToOriginalLine);

    const auto tempPath = std::filesystem::temp_directory_path() / "kites_optimizer_input.s";
    writeTemporarySource(tempPath, source);

    AssembledProgram program = Kites::assemble(tempPath.string());
    remapToOriginalLines(program, expandedLineToOriginalLine);
    return program;
}

ScheduleResult scheduleProgram(const AssembledProgram &program, bool forwardingEnabled)
{
    const std::vector<ICUnit> instructions = instructionsOf(program);
    std::vector<bool> labelBoundary = findLabelBoundaries(program, instructions.size());

    const JalrAnalysis jalr = analyzeJalrTargets(instructions);
    if (jalr.disableScheduling)
    {
        qDebug() << "[JALR] Unresolved indirect target(s):"
                 << jalr.unresolvedIndirectJumps
                 << "- preserving original instruction order.";
        ScheduleResult unchanged;
        unchanged.instructions = instructions;
        for (std::size_t i = 0; i < unchanged.instructions.size(); ++i)
            unchanged.instructions[i].setInstructionIndex(static_cast<unsigned int>(i));
        return unchanged;
    }

    for (std::size_t i = 0; i < labelBoundary.size(); ++i)
        labelBoundary[i] = labelBoundary[i] || jalr.protectedEntry[i];

    ScheduleResult result;
    result.instructions.reserve(instructions.size());

    std::size_t regionStart = 0;

    for (std::size_t i = 0; i < instructions.size(); ++i)
    {
        // Keep a JALR-sensitive entry at its exact original index.
        if (jalr.protectedEntry[i])
        {
            appendScheduledRegion(result, instructions, regionStart, i, forwardingEnabled);
            result.instructions.push_back(instructions[i]);
            regionStart = i + 1;
            continue;
        }

        // A label starts a new basic block that includes its target instruction.
        if (labelBoundary[i])
        {
            appendScheduledRegion(result, instructions, regionStart, i, forwardingEnabled);
            regionStart = i;
        }

        if (!detail::isControlFlow(instructions[i]))
            continue;

        // A conditional branch is the fixed last node of its region so the
        // scheduler can model its operands; other control flow stays unmoved.
        if (detail::isConditionalBranch(instructions[i]))
        {
            appendScheduledRegion(result, instructions, regionStart, i + 1, forwardingEnabled);
        }
        else
        {
            appendScheduledRegion(result, instructions, regionStart, i, forwardingEnabled);
            result.instructions.push_back(instructions[i]);
        }
        regionStart = i + 1;
    }
    appendScheduledRegion(result, instructions, regionStart, instructions.size(),
                          forwardingEnabled);

    for (std::size_t i = 0; i < result.instructions.size(); ++i)
        result.instructions[i].setInstructionIndex(static_cast<unsigned int>(i));

    logSchedule(instructions.size(), result);
    return result;
}

AssembledProgram buildOptimizedProgram(const AssembledProgram &original,
                                       const ScheduleResult &schedule)
{
    AssembledProgram program = original;

    // The scheduler rewrites instruction_index, so entries are matched on
    // source line and instruction fields instead.
    std::vector<std::pair<ICUnit, bool>> reordered;
    reordered.reserve(schedule.instructions.size());
    std::vector<bool> used(original.intermediate_code.size(), false);

    for (const auto &scheduled : schedule.instructions)
    {
        const auto match = claimMatchingEntry(original, used, scheduled);
        if (!match)
        {
            throw std::runtime_error(
                "Could not match a scheduled instruction to its original assembled entry.");
        }
        reordered.emplace_back(scheduled, original.intermediate_code[*match].second);
    }

    program.intermediate_code = std::move(reordered);
    rebuildIndexMetadata(program);

    program.text_buffer = Kites::generateMachineCode(program.intermediate_code);
    return program;
}

std::vector<int> originalPositions(const AssembledProgram &original,
                                   const AssembledProgram &optimized)
{
    std::vector<bool> used(original.intermediate_code.size(), false);
    std::vector<int> positions;
    positions.reserve(optimized.intermediate_code.size());

    for (const auto &entry : optimized.intermediate_code)
    {
        const auto match = claimMatchingEntry(original, used, entry.first);
        positions.push_back(match ? static_cast<int>(*match) : -1);
    }
    return positions;
}

} // namespace Kites::optimizer
