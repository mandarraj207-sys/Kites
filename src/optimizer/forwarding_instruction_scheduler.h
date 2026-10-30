#ifndef KITES_FORWARDING_INSTRUCTION_SCHEDULER_H
#define KITES_FORWARDING_INSTRUCTION_SCHEDULER_H

#include "optimizer/instruction_scheduler.h"

namespace Kites::optimizer
{
ScheduleResult scheduleForwardingBasicBlock(
    const std::vector<ICUnit>& basic_block);
}

#endif