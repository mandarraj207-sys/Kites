/**
 * @file instruction_scheduler.h
 * @brief Conservative local scheduler for reducing load-use stalls.
 *
 * Phase 1 supports an explicitly recognized subset of integer RISC-V
 * instructions. Call it on one straight-line basic block at a time.
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
 * Reorders independent instructions to fill an immediate load-use gap.
 *
 * @param basic_block One straight-line basic block. It must not contain
 *                    branch/jump instructions or instructions from another
 *                    block. Instructions carrying labels are not moved.
 * @param window_size Maximum number of instructions considered starting at
 *                    the load, including the load itself. A value below 3
 *                    disables scheduling because no candidate can fit.
 *                    Defaults to the global g_scheduler_window_size setting.
 * @return A reordered copy. Unknown instructions and memory operations are
 *         conservative barriers. The input vector is never modified.
 *
 * This function does not mutate AssembledProgram, branch targets, symbol
 * tables, or GUI state; integration into the complete program is separate.
 */
ScheduleResult scheduleLoadUseHazards(
    const std::vector<ICUnit> &basic_block,
    std::size_t window_size = g_scheduler_window_size);

} // namespace Kites::optimizer

#endif // KITES_INSTRUCTION_SCHEDULER_H
