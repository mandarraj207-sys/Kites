/**
 * @file instruction_scheduler.h
 * @brief Conservative dependency-aware scheduling within control-flow basic blocks.
 */
#ifndef KITES_INSTRUCTION_SCHEDULER_H
#define KITES_INSTRUCTION_SCHEDULER_H

#include <cstddef>
#include <vector>

#include "assembler/code_generator.h"
#include "optimizer/scheduler_config.h"

namespace Kites::optimizer
{
struct ScheduleResult
{
    std::vector<ICUnit> instructions;
    std::size_t moved_instructions = 0;
};

/**
 * Schedules a single basic block, never moving instructions across an
 * unsupported operation or a memory operation. The block must not contain
 * an interior branch/jump or instructions from another block.
 */
ScheduleResult scheduleBasicBlock(
    const std::vector<ICUnit> &basic_block,
    std::size_t window_size = g_scheduler_window_size);

/**
 * Splits a program at control-flow boundaries and at known branch-target
 * instruction indices, then schedules each block independently. Target
 * indices are zero-based instruction indices, not byte addresses.
 * Control-flow instructions remain at the end of their original block.
 */
ScheduleResult scheduleProgramBlocks(
    const std::vector<ICUnit> &program,
    const std::vector<std::size_t> &branch_target_indices,
    std::size_t window_size = g_scheduler_window_size);

/** Backwards-compatible name retained for callers from Phase 1. */
ScheduleResult scheduleLoadUseHazards(
    const std::vector<ICUnit> &basic_block,
    std::size_t window_size = g_scheduler_window_size);

} // namespace Kites::optimizer

#endif // KITES_INSTRUCTION_SCHEDULER_H
