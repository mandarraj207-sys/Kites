/**
 * @file instruction_scheduler.cpp
 * @brief Conservative dependency-aware scheduling within basic blocks.
 */
#include "optimizer/instruction_scheduler.h"
#include <cstddef>
#include <limits>
#include <QDebug>
#include <QString>
#include <sstream>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Kites::optimizer
{
namespace
{
struct InstructionInfo
{
    bool supported = false;
    bool is_load = false;
    bool is_store = false;
    std::vector<std::string> reads;
    std::vector<std::string> writes;

    bool is_memory() const { return is_load || is_store; }
};

const std::unordered_set<std::string> kRTypeInteger = {
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
    "addw", "subw", "sllw", "srlw", "sraw", "mul", "mulh", "mulhsu", "mulhu",
    "div", "divu", "rem", "remu", "mulw", "divw", "divuw", "remw", "remuw"};
const std::unordered_set<std::string> kITypeInteger = {
    "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
    "addiw", "slliw", "srliw", "sraiw"};
const std::unordered_set<std::string> kLoads = {"lb", "lh", "lw", "ld", "lbu", "lhu", "lwu"};
const std::unordered_set<std::string> kStores = {"sb", "sh", "sw", "sd"};
const std::unordered_set<std::string> kControlFlow = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "jal", "jalr", "c.j", "c.jr",
    "ecall", "ebreak", "mret", "sret"};

bool contains(const std::unordered_set<std::string> &set, const std::string &value)
{
    return set.find(value) != set.end();
}
void addRead(std::vector<std::string> &reads, const std::string &reg)
{
    if (!reg.empty()) reads.push_back(reg);
}
void addWrite(std::vector<std::string> &writes, const std::string &reg)
{
    if (!reg.empty() && reg != "x0") writes.push_back(reg);
}

InstructionInfo describe(const ICUnit &unit)
{
    InstructionInfo info;
    const std::string op = unit.getOpcode();
    const std::string rd = unit.getRd();
    const std::string rs1 = unit.getRs1();
    const std::string rs2 = unit.getRs2();

    // Labels on ICUnits represent control-flow targets/references in the
    // assembler's intermediate form. Keep such instructions immovable.
    if (!unit.getLabel().empty() || contains(kControlFlow, op)) return info;

    if (contains(kRTypeInteger, op))
    {
        info.supported = true;
        addRead(info.reads, rs1); addRead(info.reads, rs2); addWrite(info.writes, rd);
    }
    else if (contains(kITypeInteger, op))
    {
        info.supported = true;
        addRead(info.reads, rs1); addWrite(info.writes, rd);
    }
    else if (contains(kLoads, op))
    {
        info.supported = true; info.is_load = true;
        addRead(info.reads, rs1); addWrite(info.writes, rd);
    }
    else if (contains(kStores, op))
    {
        info.supported = true; info.is_store = true;
        addRead(info.reads, rs1); addRead(info.reads, rs2);
    }
    else if (op == "lui" || op == "auipc")
    {
        info.supported = true; addWrite(info.writes, rd);
    }
    return info;
}

bool intersects(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    for (const auto &reg : a)
        if (std::find(b.begin(), b.end(), reg) != b.end()) return true;
    return false;
}

bool hasDependency(const InstructionInfo &a, const InstructionInfo &b)
{
    // RAW, WAR, and WAW dependencies must retain their relative order.
    return intersects(a.writes, b.reads) || intersects(a.reads, b.writes) ||
           intersects(a.writes, b.writes);
}

bool canCross(const InstructionInfo &moving, const InstructionInfo &crossed)
{
    // Memory alias analysis is intentionally out of scope: memory ops are barriers.
    if (!moving.supported || !crossed.supported || moving.is_memory() || crossed.is_memory())
        return false;
    return !hasDependency(moving, crossed);
}

bool hasRAWDependency(const InstructionInfo &producer, const InstructionInfo &consumer)
{
    // Only read-after-write dependencies impose a result-availability delay.
    return intersects(producer.writes, consumer.reads);
}

bool isControlFlow(const ICUnit &unit)
{
    return contains(kControlFlow, unit.getOpcode());
}

void updateIndicesIfMoved(ScheduleResult &result, const std::vector<ICUnit> &original)
{
    if (result.moved_instructions == 0 || original.empty()) return;
    const unsigned int first_index = original.front().getInstructionIndex();
    for (std::size_t i = 0; i < result.instructions.size(); ++i)
        result.instructions[i].setInstructionIndex(first_index + static_cast<unsigned int>(i));
}

} // namespace



ScheduleResult scheduleBasicBlock(
    const std::vector<ICUnit> &basic_block
    )
{
    ScheduleResult result;
    result.instructions = basic_block;


    const std::size_t n = basic_block.size();
    qDebug() << "[H_NF] Received block size:" << n;
    if (n < 2)
        return result;

    std::size_t region_start = 0;

    while (region_start < n)
    {
        const InstructionInfo first =
            describe(basic_block[region_start]);

        // Loads may move around independent arithmetic instructions.
        // Stores remain barriers because memory alias analysis is not implemented.
        if (!first.supported || first.is_store)
        {
            ++region_start;
            continue;
        }

        std::size_t region_end = region_start;
        while (region_end < n)
        {
            const InstructionInfo info =
                describe(basic_block[region_end]);

            if (!info.supported || info.is_store)
                break;

            ++region_end;
        }

        const std::size_t count = region_end - region_start;
        if (count < 2)
        {
            region_start = region_end;
            continue;
        }

        std::vector<InstructionInfo> nodes(count);
        for (std::size_t i = 0; i < count; ++i)
            nodes[i] = describe(basic_block[region_start + i]);

        // DAG edges preserve every original RAW, WAR, and WAW dependency.
        std::vector<std::vector<bool>> edges(
            count, std::vector<bool>(count, false));

        for (std::size_t i = 0; i < count; ++i)
        {
            for (std::size_t j = i + 1; j < count; ++j)
            {
                // Preserve register dependencies and the original order of
                // loads. Loads may move relative to independent ALU ops, but
                // we do not perform memory-dependence/alias analysis.
                const bool loadOrder =
                    nodes[i].is_load && nodes[j].is_load;
                edges[i][j] =
                    hasDependency(nodes[i], nodes[j]) || loadOrder;
            }
        }

        // Longest dependency-chain length from each node.
        std::vector<std::size_t> critical_path(count, 1);

        for (std::size_t i = count; i-- > 0;)
        {
            for (std::size_t j = i + 1; j < count; ++j)
            {
                if (edges[i][j])
                {
                    critical_path[i] = std::max(
                        critical_path[i], critical_path[j] + 1);
                }
            }
        }

        std::vector<bool> scheduled(count, false);
        std::vector<std::size_t> order;
        std::vector<InstructionInfo> scheduled_info;
        order.reserve(count);
        scheduled_info.reserve(count);

        while (order.size() < count)
        {
            std::size_t best = count;
            std::size_t best_stalls =
                std::numeric_limits<std::size_t>::max();
            std::size_t best_path = 0;

            // Consider ALL ready nodes, not just a fixed-size window.

            for (std::size_t candidate = 0;
                 candidate < count; ++candidate)
            {
                if (!scheduled[candidate])
                    {
                        std::ostringstream instructionText;
                        instructionText << basic_block[region_start + candidate];

                        qDebug() << "[H_NF] Candidate:"
                                << QString::fromStdString(instructionText.str());
                    }

                if (scheduled[candidate])
                    continue;

                bool ready = true;
                for (std::size_t pred = 0; pred < candidate; ++pred)
                {
                    if (edges[pred][candidate] && !scheduled[pred])
                    {
                        ready = false;
                        break;
                    }
                }

                if (!ready)
                    continue;

                // Estimate the immediate no-forwarding RAW stall penalty.
                std::size_t stalls = 0;

                if (!scheduled_info.empty() &&
                    hasRAWDependency(scheduled_info.back(),
                                     nodes[candidate]))
                {
                    stalls = 2;
                }
                else if (scheduled_info.size() >= 2 &&
                         hasRAWDependency(
                             scheduled_info[scheduled_info.size() - 2],
                             nodes[candidate]))
                {
                    stalls = 1;
                }

                if (!scheduled[candidate])
                    {
                        std::ostringstream instructionText;
                        instructionText << basic_block[region_start + candidate];

                        qDebug() << "[H_NF] Candidate:"
                                << QString::fromStdString(instructionText.str())
                                << "Stalls:" << stalls
                                << "Critical path:" << critical_path[candidate];
                    }

                if (best == count ||
                    stalls < best_stalls ||
                    (stalls == best_stalls &&
                     critical_path[candidate] > best_path) ||
                    (stalls == best_stalls &&
                     critical_path[candidate] == best_path &&
                     candidate < best))
                {
                    best = candidate;
                    best_stalls = stalls;
                    best_path = critical_path[candidate];
                }
            }

            // The graph is acyclic because all edges point forward in the
            // original instruction order. This guard protects against bugs.
            if (best == count)
                break;

            scheduled[best] = true;
            order.push_back(best);
            scheduled_info.push_back(nodes[best]);
        }

        // Reorder only if the scheduler produced a complete valid ordering.
        if (order.size() == count)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                result.instructions[region_start + i] =
                    basic_block[region_start + order[i]];

                if (order[i] != i)
                    ++result.moved_instructions;
            }
        }

        region_start = region_end;
    }

    updateIndicesIfMoved(result, basic_block);
    return result;
}

ScheduleResult scheduleProgramBlocks(const std::vector<ICUnit> &program,
                                     const std::vector<std::size_t> &branch_target_indices)
{
    ScheduleResult result;
    result.instructions = program;
    if (program.empty()) return result;

    std::vector<bool> leader(program.size(), false);
    leader[0] = true;
    for (std::size_t target : branch_target_indices)
        if (target < program.size()) leader[target] = true;
    for (std::size_t i = 0; i < program.size(); ++i)
        if (isControlFlow(program[i]) && i + 1 < program.size()) leader[i + 1] = true;

    std::vector<ICUnit> scheduled;
    scheduled.reserve(program.size());
    std::size_t block_start = 0;
    for (std::size_t i = 1; i <= program.size(); ++i)
    {
        if (i == program.size() || leader[i])
        {
            std::vector<ICUnit> block(program.begin() + static_cast<std::ptrdiff_t>(block_start),
                                      program.begin() + static_cast<std::ptrdiff_t>(i));
            auto block_result = scheduleBasicBlock(block);
            result.moved_instructions += block_result.moved_instructions;
            scheduled.insert(scheduled.end(), block_result.instructions.begin(),
                             block_result.instructions.end());
            block_start = i;
        }
    }
    result.instructions = std::move(scheduled);
    updateIndicesIfMoved(result, program);
    return result;
}

ScheduleResult scheduleLoadUseHazards(
    const std::vector<ICUnit> &basic_block)
{
    return scheduleBasicBlock(basic_block);
}


} // namespace Kites::optimizer
