/**
 * @file program_optimizer.h
 * @brief Turns editor source into an assembled program and rebuilds it in scheduled order.
 */
#ifndef KITES_PROGRAM_OPTIMIZER_H
#define KITES_PROGRAM_OPTIMIZER_H

#include <cstddef>
#include <string>
#include <vector>

#include "common/assembled_program.h"
#include "optimizer/instruction_scheduler.h"

namespace Kites::optimizer
{
/**
 * Expands custom pseudo-instructions, assembles the source, and maps every
 * instruction back to the line the user wrote.
 * @throws std::runtime_error if the temporary assembly file cannot be written.
 */
AssembledProgram assembleEditorSource(const std::string &rawSource);

/**
 * Schedules each straight-line region of the program independently. Regions
 * keep their length, so labels, branches and their immediates stay valid.
 */
ScheduleResult scheduleProgram(const AssembledProgram &program, bool forwardingEnabled);

/**
 * Copy of `original` with its instructions in scheduled order and all
 * index-dependent metadata and machine code regenerated.
 * @throws std::runtime_error if a scheduled instruction has no original entry.
 */
AssembledProgram buildOptimizedProgram(const AssembledProgram &original,
                                       const ScheduleResult &schedule);

/** Marks instructions that a code label points at (addresses are bytes, one 32-bit instruction each). */
std::vector<bool> findLabelBoundaries(const AssembledProgram &program, std::size_t instructionCount);

/** For each optimized instruction, its position in the original program (-1 if unmatched). */
std::vector<int> originalPositions(const AssembledProgram &original,
                                   const AssembledProgram &optimized);

} // namespace Kites::optimizer

#endif // KITES_PROGRAM_OPTIMIZER_H
