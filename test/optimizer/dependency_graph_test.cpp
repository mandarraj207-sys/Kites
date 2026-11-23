#include "optimizer/dependency_graph.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using Kites::AssembledProgram;
using Kites::ICUnit;
using Kites::optimizer::dependenciesOf;
using Kites::optimizer::findRegisterDependencies;
using Kites::optimizer::RegisterDependency;

namespace
{
ICUnit makeInstruction(const std::string &opcode, const std::string &rd, const std::string &rs1,
                       const std::string &rs2 = "")
{
    ICUnit unit;
    unit.setOpcode(opcode);
    unit.setRd(rd);
    unit.setRs1(rs1);
    unit.setRs2(rs2);
    return unit;
}

AssembledProgram programOf(const std::vector<ICUnit> &instructions)
{
    AssembledProgram program;
    for (const auto &instruction : instructions)
        program.intermediate_code.emplace_back(instruction, false);
    return program;
}

bool contains(const std::vector<RegisterDependency> &all, std::size_t producer,
              std::size_t consumer, const std::string &reg)
{
    for (const auto &d : all)
        if (d.producer == producer && d.consumer == consumer && d.reg == reg)
            return true;
    return false;
}
} // namespace

TEST(DependencyGraphTest, LinksConsumerToItsProducer)
{
    const auto deps = findRegisterDependencies(programOf({
        makeInstruction("addi", "x1", "x0"),
        makeInstruction("add", "x2", "x1", "x1"),
    }));

    ASSERT_EQ(deps.size(), 1u);
    EXPECT_TRUE(contains(deps, 0, 1, "x1"));
}

TEST(DependencyGraphTest, OnlyTheLatestWriterIsTheProducer)
{
    const auto deps = findRegisterDependencies(programOf({
        makeInstruction("addi", "x1", "x0"),
        makeInstruction("addi", "x1", "x0"),
        makeInstruction("add", "x2", "x1", "x0"),
    }));

    ASSERT_EQ(deps.size(), 1u);
    EXPECT_TRUE(contains(deps, 1, 2, "x1"));
}

TEST(DependencyGraphTest, InstructionCanBeConsumerThenProducerOfTheSameRegister)
{
    const auto deps = findRegisterDependencies(programOf({
        makeInstruction("addi", "x6", "x0"),
        makeInstruction("addi", "x6", "x6"),
        makeInstruction("add", "x7", "x6", "x0"),
    }));

    EXPECT_TRUE(contains(deps, 0, 1, "x6"));
    EXPECT_TRUE(contains(deps, 1, 2, "x6"));
    EXPECT_FALSE(contains(deps, 0, 2, "x6"));
}

TEST(DependencyGraphTest, StoresConsumeTheirOperands)
{
    const auto deps = findRegisterDependencies(programOf({
        makeInstruction("addi", "x5", "x0"),
        makeInstruction("sw", "", "x2", "x5"),
    }));

    EXPECT_TRUE(contains(deps, 0, 1, "x5"));
}

TEST(DependencyGraphTest, ControlFlowEndsTheSegment)
{
    const auto deps = findRegisterDependencies(programOf({
        makeInstruction("addi", "x5", "x0"),
        makeInstruction("beq", "", "x5", "x0"),
        makeInstruction("add", "x6", "x5", "x0"),
    }));

    EXPECT_TRUE(contains(deps, 0, 1, "x5"));
    EXPECT_FALSE(contains(deps, 0, 2, "x5"));
}

TEST(DependencyGraphTest, LabelEndsTheSegment)
{
    AssembledProgram program = programOf({
        makeInstruction("addi", "x5", "x0"),
        makeInstruction("add", "x6", "x5", "x0"),
    });
    Kites::SymbolData label{};
    label.isData = false;
    label.address = 4;
    program.symbol_table["loop"] = label;

    EXPECT_TRUE(findRegisterDependencies(program).empty());
}

TEST(DependencyGraphTest, UnmodelledInstructionsAreBarriers)
{
    const auto deps = findRegisterDependencies(programOf({
        makeInstruction("addi", "x5", "x0"),
        makeInstruction("custom.unknown", "x5", "f1"),
        makeInstruction("add", "x6", "x5", "x0"),
    }));

    EXPECT_TRUE(deps.empty());
}


TEST(DependencyGraphTest, FPComparisonsReadBothFloatingPointOperands)
{
    const std::vector<std::string> comparisons = {
        "feq.s", "flt.s", "fle.s", "feq.d", "flt.d", "fle.d"};

    for (const auto &opcode : comparisons)
    {
        const auto deps = findRegisterDependencies(programOf({
            makeInstruction("fadd.s", "f3", "f4", "f5"),
            makeInstruction(opcode, "x1", "f2", "f3"),
            makeInstruction("add", "x6", "x1", "x7"),
        }));

        EXPECT_TRUE(contains(deps, 0, 1, "f3")) << "opcode=" << opcode;
        EXPECT_TRUE(contains(deps, 1, 2, "x1")) << "opcode=" << opcode;
    }
}

TEST(DependencyGraphTest, DependenciesOfKeepsProducerAndConsumerRolesOnly)
{
    const std::vector<RegisterDependency> all = {{0, 1, "x1"}, {1, 2, "x2"}, {0, 2, "x1"}, {3, 4, "x9"}};

    const auto related = dependenciesOf(all, 1);

    ASSERT_EQ(related.size(), 2u);
    EXPECT_TRUE(contains(related, 0, 1, "x1"));
    EXPECT_TRUE(contains(related, 1, 2, "x2"));
}
