// 500 additional DISTINCT optimizer regression scenarios for KITES.
// Unlike the earlier stress file, this suite does not repeat a small set of
// gap patterns. Each parameter changes the instruction mix, register mapping,
// operand position, dependency topology, or control-flow/barrier placement.

#include "optimizer/dependency_graph.h"
#include "optimizer/forwarding_instruction_scheduler.h"
#include "optimizer/instruction_scheduler.h"
#include "optimizer/scheduler_core.h"
#include "optimizer/stall_model.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
using Kites::AssembledProgram;
using Kites::ICUnit;
using Kites::optimizer::RegisterDependency;
using Kites::optimizer::ScheduleResult;

ICUnit ins(unsigned id, const std::string &op, const std::string &rd,
           const std::string &rs1, const std::string &rs2 = "",
           const std::string &rs3 = "")
{
    ICUnit u;
    u.setInstructionIndex(id);
    u.setOpcode(op);
    u.setRd(rd);
    u.setRs1(rs1);
    u.setRs2(rs2);
    u.setRs3(rs3);
    return u;
}

AssembledProgram program(const std::vector<ICUnit> &v)
{
    AssembledProgram p;
    for (const auto &u : v) p.intermediate_code.emplace_back(u, false);
    return p;
}

bool hasDep(const std::vector<RegisterDependency> &deps, std::size_t p,
            std::size_t c, const std::string &r)
{
    return std::any_of(deps.begin(), deps.end(), [&](const auto &d) {
        return d.producer == p && d.consumer == c && d.reg == r;
    });
}

void assertScheduleInvariants(const std::vector<ICUnit> &original,
                              const ScheduleResult &scheduled)
{
    ASSERT_EQ(scheduled.instructions.size(), original.size());
    std::map<unsigned, std::size_t> outPos;
    for (std::size_t i = 0; i < scheduled.instructions.size(); ++i)
    {
        const unsigned id = scheduled.instructions[i].getInstructionIndex();
        EXPECT_EQ(outPos.count(id), 0u) << "duplicate instruction id=" << id;
        outPos[id] = i;
    }
    ASSERT_EQ(outPos.size(), original.size());

    for (std::size_t i = 0; i < original.size(); ++i)
        for (std::size_t j = i + 1; j < original.size(); ++j)
        {
            const auto a = Kites::optimizer::detail::describe(original[i]);
            const auto b = Kites::optimizer::detail::describe(original[j]);
            if (Kites::optimizer::detail::hasDependency(a, b))
                EXPECT_LT(outPos[original[i].getInstructionIndex()],
                          outPos[original[j].getInstructionIndex()])
                    << "dependency reversed: original " << i << " -> " << j;
        }
}

// -------------------------------------------------------------------------
// 150 unique GPR dependency-graph cases: opcode pair, register, operand slot,
// and number/type of independent instructions all vary.
// -------------------------------------------------------------------------
struct GprDepCase
{
    unsigned id;
    std::string producer;
    std::string consumer;
    unsigned reg;
    unsigned gap;
    bool consumerUsesRs2;
};

std::vector<GprDepCase> makeGprDepCases()
{
    const std::vector<std::string> producers = {"addi", "add", "sub", "xor", "or", "and", "lw", "lb", "slli", "mul"};
    const std::vector<std::string> consumers = {"add", "sub", "xor", "or", "and", "sll", "slt", "addi", "lw", "sw"};
    std::vector<GprDepCase> v;
    for (unsigned i = 0; i < 150; ++i)
        v.push_back({i, producers[i % producers.size()], consumers[(i * 7 + i / 3) % consumers.size()],
                     1 + (i % 25), (i / 10) % 4, (i % 2) != 0});
    return v;
}

class GprDependencyDiversityTest : public ::testing::TestWithParam<GprDepCase> {};
TEST_P(GprDependencyDiversityTest, FindsTheIntendedLatestWriter)
{
    const auto c = GetParam();
    const std::string r = "x" + std::to_string(c.reg);
    std::vector<ICUnit> v;
    if (c.producer == "addi" || c.producer == "slli")
        v.push_back(ins(0, c.producer, r, "x0"));
    else if (c.producer == "lw" || c.producer == "lb")
        v.push_back(ins(0, c.producer, r, "x28"));
    else
        v.push_back(ins(0, c.producer, r, "x29", "x30"));

    for (unsigned g = 0; g < c.gap; ++g)
    {
        const unsigned d = 26 + ((c.id + g) % 5); // disjoint from producer x1..x25
        v.push_back(ins(static_cast<unsigned>(v.size()), "addi", "x" + std::to_string(d), "x0"));
    }
    const std::string other = "x" + std::to_string(26 + (c.id % 5));
    const std::string rd = "x" + std::to_string(1 + ((c.id + 11) % 25));
    if (c.consumer == "addi" || c.consumer == "lw")
        v.push_back(ins(static_cast<unsigned>(v.size()), c.consumer, rd, r));
    else if (c.consumer == "sw")
        v.push_back(ins(static_cast<unsigned>(v.size()), "sw", "",
                        c.consumerUsesRs2 ? other : r,
                        c.consumerUsesRs2 ? r : other));
    else
        v.push_back(ins(static_cast<unsigned>(v.size()), c.consumer, rd,
                        c.consumerUsesRs2 ? other : r,
                        c.consumerUsesRs2 ? r : other));

    const auto deps = Kites::optimizer::findRegisterDependencies(program(v));
    ASSERT_TRUE(hasDep(deps, 0, v.size() - 1, r)) << "case=" << c.id;
}
INSTANTIATE_TEST_SUITE_P(Unique150GprDependencyScenarios, GprDependencyDiversityTest,
                         ::testing::ValuesIn(makeGprDepCases()),
                         [](const auto &i) { return "Scenario" + std::to_string(i.index); });

// -------------------------------------------------------------------------
// 80 unique FP / conversion scenarios. These cover unary, binary, fused,
// compare, class, loads/stores, and both directions of register conversion.
// -------------------------------------------------------------------------
struct FpCase { unsigned id; std::string producer; std::string consumer; unsigned reg; };
std::vector<FpCase> makeFpCases()
{
    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"fadd.s", "fsub.s"}, {"fmul.s", "fdiv.s"}, {"fsqrt.s", "fadd.s"},
        {"flw", "fmul.s"}, {"fcvt.s.w", "fadd.s"}, {"fadd.d", "fmax.d"},
        {"fmadd.s", "fsub.s"}, {"fclass.s", "add"}, {"fmv.w.x", "fadd.s"},
        {"fcvt.w.s", "add"}, {"feq.s", "add"}, {"fld", "fdiv.d"},
        {"fmin.s", "fsqrt.s"}, {"fsgnj.d", "fsub.d"}, {"fcvt.d.s", "fadd.d"},
        {"fmv.x.w", "sub"}};
    std::vector<FpCase> v;
    for (unsigned i = 0; i < 80; ++i)
        v.push_back({i, pairs[i % pairs.size()].first, pairs[i % pairs.size()].second,
                     1 + (i % 28)});
    return v;
}
class FpDependencyDiversityTest : public ::testing::TestWithParam<FpCase> {};
TEST_P(FpDependencyDiversityTest, TracksCorrectRegisterFileAcrossConversions)
{
    const auto c = GetParam();
    const bool producerWritesGpr = c.producer == "fclass.s" || c.producer == "fcvt.w.s" || c.producer == "fmv.x.w" || c.producer == "feq.s";
    const std::string dst = std::string(producerWritesGpr ? "x" : "f") + std::to_string(c.reg);
    const bool consumerReadsFpr = c.consumer.rfind("f", 0) == 0;
    const std::string src = std::string(consumerReadsFpr ? "f" : "x") + std::to_string(c.reg);
    std::vector<ICUnit> v;
    if (c.producer == "flw" || c.producer == "fld")
        v.push_back(ins(0, c.producer, dst, "x29"));
    else if (c.producer == "fsqrt.s" || c.producer == "fcvt.s.w" || c.producer == "fcvt.d.s" || c.producer == "fmv.w.x" || c.producer == "fcvt.w.s" || c.producer == "fmv.x.w" || c.producer == "fclass.s")
        v.push_back(ins(0, c.producer, dst, producerWritesGpr ? "f29" : "x29"));
    else if (c.producer == "fmadd.s")
        v.push_back(ins(0, c.producer, dst, "f27", "f28", "f29"));
    else
        v.push_back(ins(0, c.producer, dst, producerWritesGpr ? "f29" : "f27", producerWritesGpr ? "f28" : "f28"));

    // Make the consumer read the produced register in the correct register file.
    if (c.consumer == "add" || c.consumer == "sub")
        v.push_back(ins(1, c.consumer, "x30", src, "x31"));
    else
        v.push_back(ins(1, c.consumer, "f30", src, "f31"));
    const auto deps = Kites::optimizer::findRegisterDependencies(program(v));
    EXPECT_TRUE(hasDep(deps, 0, 1, dst)) << "case=" << c.id << " dst=" << dst << " src=" << src;
}
INSTANTIATE_TEST_SUITE_P(Unique80FloatingPointScenarios, FpDependencyDiversityTest,
                         ::testing::ValuesIn(makeFpCases()),
                         [](const auto &i) { return "Scenario" + std::to_string(i.index); });

// -------------------------------------------------------------------------
// 100 distinct stall scenarios. Each has a unique producer/consumer opcode,
// destination register, gap length, and independent-register arrangement.
// -------------------------------------------------------------------------
struct StallCase { unsigned id; std::string producer; std::string consumer; unsigned gap; unsigned reg; };
std::vector<StallCase> makeHnfCases()
{
    const std::vector<std::string> prod = {"add", "sub", "xor", "or", "and", "mul", "addi", "slli", "lw", "lb"};
    const std::vector<std::string> cons = {"add", "sub", "xor", "slt", "and"};
    std::vector<StallCase> v;
    for (unsigned i = 0; i < 50; ++i)
        v.push_back({i, prod[i % prod.size()], cons[(i * 3 + i / 5) % cons.size()], i % 5, 1 + (i % 25)});
    return v;
}
std::vector<StallCase> makeHfCases()
{
    const std::vector<std::string> gprLoads = {"lw", "lb", "lh", "ld", "lbu", "lhu", "lwu"};
    const std::vector<std::string> gprConsumers = {"add", "sub", "xor"};
    const std::vector<std::string> fpLoads = {"flw", "fld"};
    const std::vector<std::string> fpConsumers = {"fadd.s", "fsub.s"};
    std::vector<StallCase> v;
    for (unsigned i = 0; i < 50; ++i)
    {
        if (i % 2 == 0)
            v.push_back({i, gprLoads[(i / 2) % gprLoads.size()],
                         gprConsumers[(i / 2) % gprConsumers.size()], (i / 2) % 5,
                         1 + ((i * 3) % 25)});
        else
            v.push_back({i, fpLoads[(i / 2) % fpLoads.size()],
                         fpConsumers[(i / 2) % fpConsumers.size()], (i / 2) % 5,
                         1 + ((i * 7) % 25)});
    }
    return v;
}
std::vector<ICUnit> makeStallSequence(const StallCase &c, bool forwarding)
{
    const bool fpLoad = c.producer == "flw" || c.producer == "fld";
    const std::string r = std::string(fpLoad ? "f" : "x") + std::to_string(c.reg);
    std::vector<ICUnit> v;
    if (c.producer == "addi") v.push_back(ins(0, c.producer, r, "x0"));
    else if (c.producer == "slli") v.push_back(ins(0, c.producer, r, "x0"));
    else if (c.producer == "lw" || c.producer == "lb" || c.producer == "lh" || c.producer == "ld" || c.producer == "lbu" || c.producer == "lhu" || c.producer == "lwu" || fpLoad)
        v.push_back(ins(0, c.producer, r, "x29"));
    else v.push_back(ins(0, c.producer, r, "x27", "x28"));
    for (unsigned g = 0; g < c.gap; ++g)
        v.push_back(ins(static_cast<unsigned>(v.size()), "addi", "x" + std::to_string(26 + ((c.id + g) % 5)), "x0"));
    if (c.consumer == "fadd.s" || c.consumer == "fsub.s")
        v.push_back(ins(static_cast<unsigned>(v.size()), c.consumer, "f31", r, "f30"));
    else
        v.push_back(ins(static_cast<unsigned>(v.size()), c.consumer, "x31", r, "x30"));
    (void)forwarding;
    return v;
}
class HnfDiverseStallTest : public ::testing::TestWithParam<StallCase> {};
TEST_P(HnfDiverseStallTest, ComputesHnfBubblesForDistinctInstructionSequences)
{
    const auto c = GetParam();
    const auto v = makeStallSequence(c, false);
    const auto s = Kites::optimizer::computeStalls(program(v), false);
    const std::size_t expected = c.gap >= 2 ? 0u : 2u - c.gap;
    ASSERT_EQ(s.size(), v.size());
    EXPECT_EQ(s.back().cycles, expected) << "scenario=" << c.id;
    EXPECT_EQ(Kites::optimizer::totalStalls(s), expected);
}
INSTANTIATE_TEST_SUITE_P(Unique50HnfStallScenarios, HnfDiverseStallTest,
                         ::testing::ValuesIn(makeHnfCases()),
                         [](const auto &i) { return "Scenario" + std::to_string(i.index); });
class HfDiverseStallTest : public ::testing::TestWithParam<StallCase> {};
TEST_P(HfDiverseStallTest, ComputesLoadUseBubblesForDistinctInstructionSequences)
{
    const auto c = GetParam();
    const auto v = makeStallSequence(c, true);
    const auto s = Kites::optimizer::computeStalls(program(v), true);
    const std::size_t expected = c.gap == 0 ? 1u : 0u;
    ASSERT_EQ(s.size(), v.size());
    EXPECT_EQ(s.back().cycles, expected) << "scenario=" << c.id;
    EXPECT_EQ(Kites::optimizer::totalStalls(s), expected);
}
INSTANTIATE_TEST_SUITE_P(Unique50HfStallScenarios, HfDiverseStallTest,
                         ::testing::ValuesIn(makeHfCases()),
                         [](const auto &i) { return "Scenario" + std::to_string(i.index); });

// -------------------------------------------------------------------------
// 120 scheduler cases with six different dependency graph shapes. Every case
// has a distinct register assignment and a varying mix of ALU/load/store/FP.
// -------------------------------------------------------------------------
struct ScheduleCase { unsigned id; unsigned shape; unsigned base; };
std::vector<ScheduleCase> makeScheduleCases()
{
    std::vector<ScheduleCase> v;
    for (unsigned i = 0; i < 120; ++i) v.push_back({i, i % 6, 1 + (i / 6)});
    return v;
}
std::vector<ICUnit> makeScheduleBlock(const ScheduleCase &c)
{
    const std::string a = "x" + std::to_string(c.base);
    const std::string b = "x" + std::to_string(20 + (c.base % 10));
    const std::string d = "x" + std::to_string(10 + (c.base % 10));
    const std::string f = "f" + std::to_string(c.base);
    std::vector<ICUnit> v;
    switch (c.shape)
    {
    case 0: // linear RAW chain
        v = {ins(0,"add",a,"x27","x28"), ins(1,"sub",d,a,"x29"), ins(2,"xor",b,d,"x30")}; break;
    case 1: // two independent chains interleaved
        v = {ins(0,"add",a,"x27","x28"), ins(1,"sub",b,"x29","x30"), ins(2,"xor",d,a,"x31"), ins(3,"or","x"+std::to_string(11+(c.base%8)),b,"x26")}; break;
    case 2: // WAR then RAW
        v = {ins(0,"add",d,a,"x29"), ins(1,"addi",a,"x0"), ins(2,"sub",b,a,"x30")}; break;
    case 3: // load-use with independent ALU candidate
        v = {ins(0,"lw",a,"x29"), ins(1,"addi",d,"x0"), ins(2,"add",b,a,"x30")}; break;
    case 4: // FP chain plus independent GPR work
        v = {ins(0,"fadd.s",f,"f27","f28"), ins(1,"addi",d,"x0"), ins(2,"fsub.s","f"+std::to_string(20+(c.base%10)),f,"f29")}; break;
    default: // store is a scheduling boundary and consumes both operands
        v = {ins(0,"add",a,"x27","x28"), ins(1,"sw","", "x29",a), ins(2,"addi",d,"x0")}; break;
    }
    for (unsigned i = 0; i < v.size(); ++i) v[i].setInstructionIndex(i);
    return v;
}
class DiverseSchedulerInvariantTest : public ::testing::TestWithParam<ScheduleCase> {};
TEST_P(DiverseSchedulerInvariantTest, BothSchedulersPreserveInstructionIdentityAndDependencies)
{
    const auto b = makeScheduleBlock(GetParam());
    assertScheduleInvariants(b, Kites::optimizer::scheduleBasicBlock(b));
    assertScheduleInvariants(b, Kites::optimizer::scheduleForwardingBasicBlock(b));
}
INSTANTIATE_TEST_SUITE_P(Unique120SchedulerTopologies, DiverseSchedulerInvariantTest,
                         ::testing::ValuesIn(makeScheduleCases()),
                         [](const auto &i) { return "Scenario" + std::to_string(i.index); });

// -------------------------------------------------------------------------
// 50 control-flow / unsupported-op barrier scenarios. Each varies barrier
// opcode and register, and asserts that dependencies do not leak across it.
// -------------------------------------------------------------------------
struct BarrierCase { unsigned id; std::string barrier; unsigned reg; };
std::vector<BarrierCase> makeBarrierCases()
{
    const std::vector<std::string> barriers = {"jal", "jalr", "j", "jr", "ecall", "ebreak", "mret", "custom.unknown", "c.j", "c.ebreak"};
    std::vector<BarrierCase> v;
    for (unsigned i = 0; i < 50; ++i) v.push_back({i, barriers[i % barriers.size()], 1 + (i % 25)});
    return v;
}
class ControlBarrierDiversityTest : public ::testing::TestWithParam<BarrierCase> {};
TEST_P(ControlBarrierDiversityTest, DoesNotCarryRegisterDependencyAcrossBarrier)
{
    const auto c = GetParam();
    const std::string r = "x" + std::to_string(c.reg);
    const auto deps = Kites::optimizer::findRegisterDependencies(program({
        ins(0, "addi", r, "x0"),
        ins(1, c.barrier, "", "", ""),
        ins(2, "add", "x30", r, "x29")
    }));
    EXPECT_FALSE(hasDep(deps, 0, 2, r)) << "barrier=" << c.barrier << " scenario=" << c.id;
}
INSTANTIATE_TEST_SUITE_P(Unique50ControlBarrierScenarios, ControlBarrierDiversityTest,
                         ::testing::ValuesIn(makeBarrierCases()),
                         [](const auto &i) { return "Scenario" + std::to_string(i.index); });

} // namespace
