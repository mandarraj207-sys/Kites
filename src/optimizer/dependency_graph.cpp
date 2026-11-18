/**
 * @file dependency_graph.cpp
 * @brief Register read-after-write dependencies between instructions of a program.
 */
#include "optimizer/dependency_graph.h"

#include <map>
#include <set>

#include "optimizer/program_optimizer.h"
#include "optimizer/scheduler_core.h"

namespace Kites::optimizer
{
std::vector<RegisterDependency> findRegisterDependencies(const AssembledProgram &program)
{
    const std::size_t count = program.intermediate_code.size();
    const std::vector<bool> labelBoundary = findLabelBoundaries(program, count);

    std::vector<RegisterDependency> dependencies;
    std::map<std::string, std::size_t> lastWriter;

    for (std::size_t i = 0; i < count; ++i)
    {
        if (labelBoundary[i])
            lastWriter.clear();

        const ICUnit &unit = program.intermediate_code[i].first;
        const detail::InstructionInfo info = detail::describe(unit);

        const std::set<std::string> reads(info.reads.begin(), info.reads.end());
        for (const std::string &reg : reads)
        {
            const auto writer = lastWriter.find(reg);
            if (writer != lastWriter.end())
                dependencies.push_back({writer->second, i, reg});
        }

        for (const std::string &reg : info.writes)
            lastWriter[reg] = i;

        // Unmodelled instructions may write registers we cannot see.
        if (!info.supported || detail::isControlFlow(unit))
            lastWriter.clear();
    }
    return dependencies;
}

std::vector<RegisterDependency> dependenciesOf(const std::vector<RegisterDependency> &all,
                                               std::size_t instruction)
{
    std::vector<RegisterDependency> related;
    for (const auto &dependency : all)
    {
        if (dependency.producer == instruction || dependency.consumer == instruction)
            related.push_back(dependency);
    }
    return related;
}

} // namespace Kites::optimizer
