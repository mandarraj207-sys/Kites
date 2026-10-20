/**
 * @file scheduler_config.h
 * @brief Central configuration for the Phase 1 local instruction scheduler.
 */
#ifndef KITES_SCHEDULER_CONFIG_H
#define KITES_SCHEDULER_CONFIG_H

#include <cstddef>

namespace Kites::optimizer
{
/**
 * Maximum number of instructions considered from a load while searching for
 * an instruction that can fill its immediate load-use gap.
 *
 * A value of 3 considers the load, its immediate consumer, and one candidate.
 * This is a scheduling-window size, not a basic-block size.
 */
inline std::size_t g_scheduler_window_size = 4;

} // namespace Kites::optimizer

#endif // KITES_SCHEDULER_CONFIG_H
