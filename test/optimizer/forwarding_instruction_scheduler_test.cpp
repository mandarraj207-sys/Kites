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
    EXPECT_EQ(result.moved_instructions,2u);
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


TEST(ForwardingSchedulerTest, BranchReadsBothOperandsAndRemainsLast)
{
    std::vector<ICUnit> block = {
        makeFwdInstruction(0, "addi", "x5", "x0"),
        makeFwdInstruction(1, "addi", "x6", "x0"),
        makeFwdInstruction(2, "addi", "x7", "x0"),
        makeFwdInstruction(3, "beq", "", "x5", "x6")};

    // In the assembler IR, a branch's label field stores its TARGET.
    block.back().setLabel("target");

    const auto result = scheduleForwardingBasicBlock(block);
    ASSERT_EQ(result.instructions.size(), block.size());
    EXPECT_EQ(result.instructions.back().getOpcode(), "beq");
    // Both branch source registers must be produced before the branch.
    auto producerPosition = [&](const std::string &reg) {
        for (std::size_t i = 0; i < result.instructions.size(); ++i)
            if (result.instructions[i].getRd() == reg)
                return i;
        return result.instructions.size();
    };
    EXPECT_LT(producerPosition("x5"), result.instructions.size() - 1);
    EXPECT_LT(producerPosition("x6"), result.instructions.size() - 1);
}


TEST(ForwardingSchedulerTest, KeepsAuipcAtItsOriginalSlot)
{
    const std::vector<ICUnit> block = {
        makeFwdInstruction(0, "addi", "x6", "x1"),
        makeFwdInstruction(1, "auipc", "x5", ""),
        makeFwdInstruction(2, "addi", "x7", "x2"),
        makeFwdInstruction(3, "addi", "x8", "x3")};

    const auto result = scheduleForwardingBasicBlock(block);
    ASSERT_EQ(result.instructions.size(), block.size());
    EXPECT_EQ(result.instructions[1].getOpcode(), "auipc");
    EXPECT_EQ(result.instructions[1].getRd(), "x5");
}


TEST(ForwardingSchedulerTest, TracksFloatingPointRegisterDependencies)
{
    const std::vector<ICUnit> block = {
        makeFwdInstruction(0, "fadd.s", "f1", "f2", "f3"),
        makeFwdInstruction(1, "fsub.s", "f4", "f1", "f5"),
        makeFwdInstruction(2, "fadd.s", "f6", "f7", "f8")};

    const auto result = scheduleForwardingBasicBlock(block);
    ASSERT_EQ(result.instructions.size(), block.size());
    // Only require the true RAW ordering. An independent FP operation may
    // legally remain before or after the dependent pair.
    std::size_t producer = result.instructions.size();
    std::size_t consumer = result.instructions.size();
    for (std::size_t i = 0; i < result.instructions.size(); ++i)
    {
        if (result.instructions[i].getOpcode() == "fadd.s" &&
            result.instructions[i].getRd() == "f1") producer = i;
        if (result.instructions[i].getOpcode() == "fsub.s" &&
            result.instructions[i].getRd() == "f4") consumer = i;
    }
    EXPECT_LT(producer, consumer);
}

TEST(ForwardingSchedulerTest, KeepsIntegerAndFloatingPointRegisterFilesDistinct)
{
    const std::vector<ICUnit> block = {
        makeFwdInstruction(0, "addi", "x1", "x0"),
        makeFwdInstruction(1, "fadd.s", "f1", "f2", "f3"),
        makeFwdInstruction(2, "fsub.s", "f4", "f1", "f5")};

    const auto result = scheduleForwardingBasicBlock(block);
    ASSERT_EQ(result.instructions.size(), block.size());
    // x1 and f1 are different architectural registers. The integer
    // instruction may move freely, but the FP RAW edge must remain intact.
    std::size_t fpProducer = result.instructions.size();
    std::size_t fpConsumer = result.instructions.size();
    std::size_t intInstruction = result.instructions.size();
    for (std::size_t i = 0; i < result.instructions.size(); ++i)
    {
        const auto &u = result.instructions[i];
        if (u.getOpcode() == "fadd.s" && u.getRd() == "f1") fpProducer = i;
        if (u.getOpcode() == "fsub.s" && u.getRd() == "f4") fpConsumer = i;
        if (u.getOpcode() == "addi" && u.getRd() == "x1") intInstruction = i;
    }
    EXPECT_LT(fpProducer, fpConsumer);
    EXPECT_LT(intInstruction, result.instructions.size());
}


TEST(ForwardingSchedulerTest, PreservesSecondSourceDependencyOfFPCompare)
{
    const std::vector<ICUnit> block = {
        makeFwdInstruction(0, "fadd.s", "f3", "f4", "f5"),
        makeFwdInstruction(1, "feq.s", "x1", "f2", "f3"),
        makeFwdInstruction(2, "addi", "x6", "x7")};

    const auto result = scheduleForwardingBasicBlock(block);
    ASSERT_EQ(result.instructions.size(), block.size());
    std::size_t producer = result.instructions.size();
    std::size_t compare = result.instructions.size();
    for (std::size_t i = 0; i < result.instructions.size(); ++i)
    {
        if (result.instructions[i].getRd() == "f3") producer = i;
        if (result.instructions[i].getOpcode() == "feq.s") compare = i;
    }
    EXPECT_LT(producer, compare);
}

TEST(ForwardingSchedulerTest, TracksThirdSourceOfFusedFloatingPointOperation)
{
    ICUnit fused = makeFwdInstruction(1, "fmadd.s", "f4", "f1", "f2");
    fused.setRs3("f3");
    const std::vector<ICUnit> block = {
        makeFwdInstruction(0, "fadd.s", "f3", "f5", "f6"),
        fused,
        makeFwdInstruction(2, "fsub.s", "f7", "f8", "f9")};

    const auto result = scheduleForwardingBasicBlock(block);
    ASSERT_EQ(result.instructions.size(), block.size());
    // The producer of rs3 must remain before fmadd.s.
    std::size_t producer = result.instructions.size();
    std::size_t consumer = result.instructions.size();
    for (std::size_t i = 0; i < result.instructions.size(); ++i)
    {
        if (result.instructions[i].getRd() == "f3") producer = i;
        if (result.instructions[i].getOpcode() == "fmadd.s") consumer = i;
    }
    EXPECT_LT(producer, consumer);
}

TEST(ForwardingSchedulerTest, ModelsFloatingPointLoadsAndStores)
{
    const std::vector<ICUnit> block = {
        makeFwdInstruction(0, "flw", "f1", "x2"),
        makeFwdInstruction(1, "fadd.s", "f3", "f1", "f4"),
        makeFwdInstruction(2, "fsub.s", "f5", "f6", "f7")};

    const auto result = scheduleForwardingBasicBlock(block);
    ASSERT_EQ(result.instructions.size(), block.size());
    EXPECT_EQ(result.instructions[1].getOpcode(), "fsub.s");
    EXPECT_EQ(result.instructions[2].getOpcode(), "fadd.s");
}
