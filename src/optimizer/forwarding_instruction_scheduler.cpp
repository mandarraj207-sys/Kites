/**
 * @file forwarding_instruction_scheduler.cpp
 * @brief DAG-based instruction scheduling for the RV5S H_F pipeline.
 */
#include "optimizer/forwarding_instruction_scheduler.h"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace Kites::optimizer
{
namespace
{
const std::unordered_set<std::string> kLoads = {
    "lb", "lh", "lw", "ld", "lbu", "lhu", "lwu"};

const std::unordered_set<std::string> kStores = {
    "sb", "sh", "sw", "sd"};

const std::unordered_set<std::string> kRType = {
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
    "addw", "subw", "sllw", "srlw", "sraw", "mul", "mulh", "mulhsu",
    "mulhu", "div", "divu", "rem", "remu", "mulw", "divw", "divuw",
    "remw", "remuw"};

const std::unordered_set<std::string> kIType = {
    "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli",
    "srai", "addiw", "slliw", "srliw", "sraiw"};

const std::unordered_set<std::string> kControl = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "jal", "jalr",
    "c.j", "c.jr", "ecall", "ebreak", "mret", "sret"};

struct Info
{
    bool ok = false;
    bool load = false;
    bool memory = false;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
};

bool has(const std::unordered_set<std::string>& set,
         const std::string& value)
{
    return set.count(value) != 0;
}

void readReg(Info& i, const std::string& r)
{
    if (!r.empty())
        i.reads.push_back(r);
}

void writeReg(Info& i, const std::string& r)
{
    if (!r.empty() && r != "x0")
        i.writes.push_back(r);
}

Info inspect(const ICUnit& u)
{
    Info i;
    const auto op = u.getOpcode();

    if (!u.getLabel().empty() || has(kControl, op))
        return i;

    if (has(kRType, op))
    {
        i.ok = true;
        readReg(i, u.getRs1());
        readReg(i, u.getRs2());
        writeReg(i, u.getRd());
    }
    else if (has(kIType, op))
    {
        i.ok = true;
        readReg(i, u.getRs1());
        writeReg(i, u.getRd());
    }
    else if (has(kLoads, op))
    {
        i.ok = true;
        i.load = true;
        i.memory = true;
        readReg(i, u.getRs1());
        writeReg(i, u.getRd());
    }
    else if (has(kStores, op))
    {
        i.ok = true;
        i.memory = true;
        readReg(i, u.getRs1());
        readReg(i, u.getRs2());
    }
    else if (op == "lui" || op == "auipc")
    {
        i.ok = true;
        writeReg(i, u.getRd());
    }

    return i;
}

bool intersects(const std::vector<std::string>& a,
                const std::vector<std::string>& b)
{
    for (const auto& x : a)
    {
        if (std::find(b.begin(), b.end(), x) != b.end())
            return true;
    }
    return false;
}

bool dependency(const Info& a, const Info& b)
{
    // RAW, WAR, and WAW dependencies.
    return intersects(a.writes, b.reads) ||
           intersects(a.reads, b.writes) ||
           intersects(a.writes, b.writes);
}

bool rawDependency(const Info& producer, const Info& consumer)
{
    return intersects(producer.writes, consumer.reads);
}

void refreshIndices(ScheduleResult& result,
                    const std::vector<ICUnit>& original)
{
    if (!result.moved_instructions || original.empty())
        return;

    const auto first = original.front().getInstructionIndex();

    for (std::size_t i = 0; i < result.instructions.size(); ++i)
    {
        result.instructions[i].setInstructionIndex(
            first + static_cast<unsigned int>(i));
    }
}
} // namespace

ScheduleResult scheduleForwardingBasicBlock(
    const std::vector<ICUnit> &block)
{
    ScheduleResult result;
    result.instructions = block;


    const std::size_t n = block.size();
    if (n < 2)
        return result;

    std::size_t regionStart = 0;

    while (regionStart < n)
    {
        const Info first = inspect(block[regionStart]);

        // Loads may move around independent arithmetic instructions.
        // Stores and unsupported instructions remain conservative barriers.
        if (!first.ok || (first.memory && !first.load))
        {
            ++regionStart;
            continue;
        }

        std::size_t regionEnd = regionStart;

        while (regionEnd < n)
        {
            const Info info = inspect(block[regionEnd]);

            if (!info.ok || (info.memory && !info.load))
                break;

            ++regionEnd;
        }

        const std::size_t count = regionEnd - regionStart;

        if (count < 2)
        {
            regionStart = regionEnd;
            continue;
        }

        std::vector<Info> nodes(count);

        for (std::size_t i = 0; i < count; ++i)
            nodes[i] = inspect(block[regionStart + i]);

        // Directed dependency graph. Edges point forward in the original
        // order and preserve RAW, WAR, and WAW dependencies.
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
                    nodes[i].load && nodes[j].load;
                edges[i][j] =
                    dependency(nodes[i], nodes[j]) || loadOrder;
            }
        }

        // Longest dependency-chain length from each node.
        std::vector<std::size_t> criticalPath(count, 1);

        for (std::size_t i = count; i-- > 0;)
        {
            for (std::size_t j = i + 1; j < count; ++j)
            {
                if (edges[i][j])
                {
                    criticalPath[i] = std::max(
                        criticalPath[i], criticalPath[j] + 1);
                }
            }
        }

        std::vector<bool> scheduled(count, false);
        std::vector<std::size_t> order;
        std::vector<Info> scheduledInfo;

        order.reserve(count);
        scheduledInfo.reserve(count);

        while (order.size() < count)
        {
            std::size_t best = count;
            std::size_t bestStallCost =
                std::numeric_limits<std::size_t>::max();
            std::size_t bestCriticalPath = 0;

            for (std::size_t candidate = 0;
                 candidate < count; ++candidate)
            {
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

                // Forwarding removes ordinary ALU-to-ALU RAW stalls.
                // A load followed immediately by its consumer still incurs
                // the load-use penalty.
                std::size_t stallCost = 0;

                if (!scheduledInfo.empty() &&
                    scheduledInfo.back().load &&
                    rawDependency(scheduledInfo.back(), nodes[candidate]))
                {
                    stallCost = 1;
                }

                if (best == count ||
                    stallCost < bestStallCost ||
                    (stallCost == bestStallCost &&
                     criticalPath[candidate] > bestCriticalPath) ||
                    (stallCost == bestStallCost &&
                     criticalPath[candidate] == bestCriticalPath &&
                     candidate < best))
                {
                    best = candidate;
                    bestStallCost = stallCost;
                    bestCriticalPath = criticalPath[candidate];
                }
            }

            if (best == count)
                break;

            scheduled[best] = true;
            order.push_back(best);
            scheduledInfo.push_back(nodes[best]);
        }

        if (order.size() == count)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                result.instructions[regionStart + i] =
                    block[regionStart + order[i]];

                if (order[i] != i)
                    ++result.moved_instructions;
            }
        }

        regionStart = regionEnd;
    }

    refreshIndices(result, block);
    return result;
}
} // namespace Kites::optimizer
