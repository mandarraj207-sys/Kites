#include "optimizer/stall_model.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using Kites::AssembledProgram;
using Kites::ICUnit;
using Kites::optimizer::computeStalls;
using Kites::optimizer::totalStalls;

namespace
{
ICUnit makeInstruction(const std::string &opcode, const std::string &rd, const std::string &rs1,
                       const std::string &rs2 = "", const std::string &imm = "")
{
    ICUnit unit;
    unit.setOpcode(opcode);
    unit.setRd(rd);
    unit.setRs1(rs1);
    unit.setRs2(rs2);
    if (!imm.empty())
        unit.setImm(imm);
    return unit;
}

AssembledProgram programOf(const std::vector<ICUnit> &instructions)
{
    AssembledProgram program;
    for (const auto &instruction : instructions)
        program.intermediate_code.emplace_back(instruction, false);
    return program;
}
} // namespace

TEST(StallModelTest, DependentInstructionDirectlyBehindWaitsTwoCycles)
{
    const auto stalls = computeStalls(programOf({
        makeInstruction("add", "x1", "x2", "x3"),
        makeInstruction("add", "x4", "x1", "x5"),
    }), false);

    ASSERT_EQ(stalls.size(), 2u);
    EXPECT_EQ(stalls[0].cycles, 0u);
    EXPECT_EQ(stalls[1].cycles, 2u);
    EXPECT_EQ(stalls[1].producer, 0u);
    EXPECT_EQ(stalls[1].reg, "x1");
}

TEST(StallModelTest, OneInstructionBetweenCostsOneCycle)
{
    const auto stalls = computeStalls(programOf({
        makeInstruction("add", "x1", "x2", "x3"),
        makeInstruction("add", "x6", "x7", "x8"),
        makeInstruction("add", "x4", "x1", "x5"),
    }), false);

    EXPECT_EQ(totalStalls(stalls), 1u);
    EXPECT_EQ(stalls[2].cycles, 1u);
}

TEST(StallModelTest, TwoInstructionsBetweenCostNothing)
{
    const auto stalls = computeStalls(programOf({
        makeInstruction("add", "x1", "x2", "x3"),
        makeInstruction("add", "x6", "x7", "x8"),
        makeInstruction("add", "x9", "x7", "x8"),
        makeInstruction("add", "x4", "x1", "x5"),
    }), false);

    EXPECT_EQ(totalStalls(stalls), 0u);
}

TEST(StallModelTest, EarlierBubblesCountAsDistance)
{
    // The second add stalls 2 cycles behind the first; the third reads the first
    // add's result too, but by then it is already 4 slots behind, so it does not stall.
    const auto stalls = computeStalls(programOf({
        makeInstruction("add", "x1", "x2", "x3"),
        makeInstruction("add", "x4", "x1", "x5"),
        makeInstruction("add", "x6", "x1", "x7"),
    }), false);

    EXPECT_EQ(stalls[1].cycles, 2u);
    EXPECT_EQ(stalls[2].cycles, 0u);
}

TEST(StallModelTest, ForwardingOnlyStallsLoadUse)
{
    const AssembledProgram aluThenUse = programOf({
        makeInstruction("add", "x1", "x2", "x3"),
        makeInstruction("add", "x4", "x1", "x5"),
    });
    EXPECT_EQ(totalStalls(computeStalls(aluThenUse, true)), 0u);

    const AssembledProgram loadThenUse = programOf({
        makeInstruction("lw", "x1", "x2", "", "0"),
        makeInstruction("add", "x4", "x1", "x5"),
    });
    const auto stalls = computeStalls(loadThenUse, true);
    EXPECT_EQ(stalls[1].cycles, 1u);
    EXPECT_EQ(stalls[1].reg, "x1");
}

TEST(StallModelTest, WritesToX0NeverCauseStalls)
{
    const auto stalls = computeStalls(programOf({
        makeInstruction("add", "x0", "x2", "x3"),
        makeInstruction("add", "x4", "x0", "x5"),
    }), false);

    EXPECT_EQ(totalStalls(stalls), 0u);
}
