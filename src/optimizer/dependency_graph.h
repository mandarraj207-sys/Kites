/**
 * @file dependency_graph.h
 * @brief Register read-after-write dependencies between instructions of a program.
 */
#ifndef KITES_DEPENDENCY_GRAPH_H
#define KITES_DEPENDENCY_GRAPH_H

#include <cstddef>
#include <string>
#include <vector>

#include "common/assembled_program.h"

namespace Kites::optimizer
{
/** `consumer` reads `reg` as last written by `producer` (both are instruction indices). */
struct RegisterDependency
{
    std::size_t producer = 0;
    std::size_t consumer = 0;
    std::string reg;
};

/**
 * Finds, for every register an instruction reads, the nearest earlier
 * instruction that wrote it. Labels and control flow end a straight-line
 * segment, and instructions the scheduler does not model are barriers.
 */
std::vector<RegisterDependency> findRegisterDependencies(const AssembledProgram &program);

/** Dependencies in which `instruction` is the producer or the consumer. */
std::vector<RegisterDependency> dependenciesOf(const std::vector<RegisterDependency> &all,
                                               std::size_t instruction);

} // namespace Kites::optimizer

#endif // KITES_DEPENDENCY_GRAPH_H
