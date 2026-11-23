
/**
 * @file instruction_scheduler.h
 * @brief Conservative dependency-aware scheduling within control-flow basic blocks.
 */
#ifndef KITES_INSTRUCTION_SCHEDULER_H
#define KITES_INSTRUCTION_SCHEDULER_H

#include <cstddef>
#include <vector>

#include "assembler/code_generator.h"

namespace Kites::optimizer
{
// Result model: reordered instructions and the count moved from their original positions.
struct ScheduleResult
{
    std::vector<ICUnit> instructions;
    std::size_t moved_instructions = 0;
};

/**
 * Schedules a single basic block using dependency-aware instruction scheduling.
 * Instructions are never moved across unsupported operations or memory barriers.
 */
// API: schedule one basic block using the H_NF policy.
ScheduleResult scheduleBasicBlock(
    const std::vector<ICUnit> &basic_block);

/**
 * Splits a program at control-flow boundaries and branch-target indices,
 * then schedules each block independently.
 */
// API: partition a program at control-flow boundaries and schedule each block.
ScheduleResult scheduleProgramBlocks(
    const std::vector<ICUnit> &program,
    const std::vector<std::size_t> &branch_target_indices);

/** Backwards-compatible name retained for Phase 1 callers. */
// Compatibility API: legacy entry point that delegates to H_NF scheduling.
ScheduleResult scheduleLoadUseHazards(
    const std::vector<ICUnit> &basic_block);

} // namespace Kites::optimizer

#endif // KITES_INSTRUCTION_SCHEDULER_H
