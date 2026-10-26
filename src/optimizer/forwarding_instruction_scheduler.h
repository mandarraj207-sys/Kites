/**
 * @file forwarding_instruction_scheduler.h
 * @brief Forwarding-aware instruction scheduling kept separate from Phase 1.
 */
#ifndef KITES_FORWARDING_INSTRUCTION_SCHEDULER_H
#define KITES_FORWARDING_INSTRUCTION_SCHEDULER_H

#include "optimizer/instruction_scheduler.h"

namespace Kites::optimizer
{
/**
 * Schedule a straight-line block for the RV5S H_F pipeline model.
 * ALU-to-ALU RAW dependencies are assumed to be handled by forwarding and are
 * therefore not filled/reordered. The scheduler only attempts to separate a
 * load from its immediate consumer by moving a safe independent instruction
 * into the load-use slot. Memory operations and unsupported instructions are
 * conservative barriers. Branches/labels are not moved across.
 */
ScheduleResult scheduleForwardingBasicBlock(
    const std::vector<ICUnit> &basic_block,
    std::size_t window_size = g_scheduler_window_size);
} // namespace Kites::optimizer

#endif
