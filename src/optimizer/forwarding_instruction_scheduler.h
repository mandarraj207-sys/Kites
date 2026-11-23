#ifndef KITES_FORWARDING_INSTRUCTION_SCHEDULER_H
#define KITES_FORWARDING_INSTRUCTION_SCHEDULER_H

#include "optimizer/instruction_scheduler.h"

namespace Kites::optimizer
{
// Public API: schedule one basic block using the H_F forwarding-aware policy.
ScheduleResult scheduleForwardingBasicBlock(
    const std::vector<ICUnit>& basic_block);
}

#endif