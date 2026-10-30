#include "optimizer/forwarding_instruction_scheduler.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using Kites::ICUnit;
using Kites::optimizer::scheduleForwardingBasicBlock;

static ICUnit makeFwdInstruction(unsigned int index, const std::string& op,
                                 const std::string& rd, const std::string& rs1,
                                 const std::string& rs2 = "")
{
    ICUnit u; u.setInstructionIndex(index); u.setOpcode(op); u.setRd(rd);
    u.setRs1(rs1); u.setRs2(rs2); return u;
}

TEST(ForwardingSchedulerTest, DoesNotReorderOrdinaryAluRawHazards)
{
    const std::vector<ICUnit> block = {
        makeFwdInstruction(0,"addi","x1","x0"),
        makeFwdInstruction(1,"add","x2","x1","x4"),
        makeFwdInstruction(2,"addi","x5","x0")};
    const auto result=scheduleForwardingBasicBlock(block);
    EXPECT_EQ(result.moved_instructions,0u);
    EXPECT_EQ(result.instructions[1].getOpcode(),"add");
}

TEST(ForwardingSchedulerTest, FillsLoadUseGapWithIndependentInstruction)
{
    const std::vector<ICUnit> block = {
        makeFwdInstruction(10,"lw","x1","x2"),
        makeFwdInstruction(11,"add","x3","x1","x4"),
        makeFwdInstruction(12,"addi","x5","x6")};
    const auto result=scheduleForwardingBasicBlock(block);
    ASSERT_EQ(result.instructions.size(),3u);
    EXPECT_EQ(result.moved_instructions,1u);
    EXPECT_EQ(result.instructions[1].getOpcode(),"addi");
    EXPECT_EQ(result.instructions[2].getOpcode(),"add");
}

TEST(ForwardingSchedulerTest, DoesNotMoveCandidateThatClobbersConsumerInput)
{
    const std::vector<ICUnit> block = {
        makeFwdInstruction(0,"lw","x1","x2"),
        makeFwdInstruction(1,"add","x3","x1","x4"),
        makeFwdInstruction(2,"addi","x1","x6")};
    EXPECT_EQ(scheduleForwardingBasicBlock(block).moved_instructions,0u);
}

TEST(ForwardingSchedulerTest, BranchInstructionIsASchedulingBarrier)
{
    const std::vector<ICUnit> block = {
        makeFwdInstruction(0, "lw", "x1", "x2"),
        makeFwdInstruction(1, "add", "x3", "x1", "x4"),
        makeFwdInstruction(2, "beq", "", "x5", "x6"),
        makeFwdInstruction(3, "lw", "x7", "x8"),
        makeFwdInstruction(4, "add", "x9", "x7", "x10")};

    const auto result = scheduleForwardingBasicBlock(block);
    ASSERT_EQ(result.instructions.size(), block.size());
    EXPECT_EQ(result.instructions[2].getOpcode(), "beq");
    // The load-use pair on either side is independently scheduled; neither
    // instruction is allowed to cross the branch.
    EXPECT_EQ(result.instructions[0].getOpcode(), "lw");
    EXPECT_EQ(result.instructions[1].getOpcode(), "add");
    EXPECT_EQ(result.instructions[3].getOpcode(), "lw");
    EXPECT_EQ(result.instructions[4].getOpcode(), "add");
}

TEST(ForwardingSchedulerTest, CompressedBranchIsAlsoABarrier)
{
    const std::vector<ICUnit> block = {
        makeFwdInstruction(0, "lw", "x1", "x2"),
        makeFwdInstruction(1, "add", "x3", "x1", "x4"),
        makeFwdInstruction(2, "c.beqz", "", "x5"),
        makeFwdInstruction(3, "addi", "x6", "x7")};

    const auto result = scheduleForwardingBasicBlock(block);
    ASSERT_EQ(result.instructions.size(), block.size());
    EXPECT_EQ(result.instructions[2].getOpcode(), "c.beqz");
}
