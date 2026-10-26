/**
 * @file forwarding_instruction_scheduler.cpp
 * @brief Isolated forwarding-aware scheduler; does not alter the Phase 1 path.
 */
#include "optimizer/forwarding_instruction_scheduler.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace Kites::optimizer
{
namespace
{
const std::unordered_set<std::string> kLoads = {"lb", "lh", "lw", "ld", "lbu", "lhu", "lwu"};
const std::unordered_set<std::string> kStores = {"sb", "sh", "sw", "sd"};
const std::unordered_set<std::string> kRType = {
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
    "addw", "subw", "sllw", "srlw", "sraw", "mul", "mulh", "mulhsu", "mulhu",
    "div", "divu", "rem", "remu", "mulw", "divw", "divuw", "remw", "remuw"};
const std::unordered_set<std::string> kIType = {
    "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
    "addiw", "slliw", "srliw", "sraiw"};
const std::unordered_set<std::string> kControl = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "jal", "jalr", "c.j", "c.jr",
    "ecall", "ebreak", "mret", "sret"};

struct Info { bool ok=false, load=false, memory=false; std::vector<std::string> reads, writes; };
bool has(const std::unordered_set<std::string>& set, const std::string& value) { return set.count(value) != 0; }
void read(Info& i, const std::string& r) { if (!r.empty()) i.reads.push_back(r); }
void write(Info& i, const std::string& r) { if (!r.empty() && r != "x0") i.writes.push_back(r); }
Info inspect(const ICUnit& u)
{
    Info i; const auto op=u.getOpcode();
    if (!u.getLabel().empty() || has(kControl, op)) return i;
    if (has(kRType,op)) { i.ok=true; read(i,u.getRs1()); read(i,u.getRs2()); write(i,u.getRd()); }
    else if (has(kIType,op)) { i.ok=true; read(i,u.getRs1()); write(i,u.getRd()); }
    else if (has(kLoads,op)) { i.ok=true; i.load=true; i.memory=true; read(i,u.getRs1()); write(i,u.getRd()); }
    else if (has(kStores,op)) { i.ok=true; i.memory=true; read(i,u.getRs1()); read(i,u.getRs2()); }
    else if (op=="lui" || op=="auipc") { i.ok=true; write(i,u.getRd()); }
    return i;
}
bool intersects(const std::vector<std::string>& a,const std::vector<std::string>& b)
{ for (const auto& x:a) if (std::find(b.begin(),b.end(),x)!=b.end()) return true; return false; }
bool dependency(const Info& a,const Info& b)
{ return intersects(a.writes,b.reads) || intersects(a.reads,b.writes) || intersects(a.writes,b.writes); }
void refreshIndices(ScheduleResult& r,const std::vector<ICUnit>& original)
{ if (!r.moved_instructions || original.empty()) return; const auto first=original.front().getInstructionIndex(); for(std::size_t n=0;n<r.instructions.size();++n) r.instructions[n].setInstructionIndex(first+static_cast<unsigned int>(n)); }
}

ScheduleResult scheduleForwardingBasicBlock(const std::vector<ICUnit>& block, std::size_t window_size)
{
    ScheduleResult result; result.instructions=block;
    if (block.size()<3 || window_size<3) return result;
    for (std::size_t load_pos=0; load_pos+2<result.instructions.size(); ++load_pos)
    {
        const Info producer=inspect(result.instructions[load_pos]);
        if (!producer.ok || !producer.load || producer.writes.empty()) continue;
        const Info consumer=inspect(result.instructions[load_pos+1]);
        if (!consumer.ok || !intersects(producer.writes,consumer.reads)) continue;
        const auto end=load_pos+std::min(window_size,result.instructions.size()-load_pos);
        for (std::size_t candidate_pos=load_pos+2; candidate_pos<end; ++candidate_pos)
        {
            const Info candidate=inspect(result.instructions[candidate_pos]);
            if (!candidate.ok || candidate.memory || dependency(candidate,producer) || dependency(candidate,consumer)) continue;
            bool safe=true;
            for (std::size_t crossed=load_pos+1; crossed<candidate_pos; ++crossed)
            {
                const Info other=inspect(result.instructions[crossed]);
                if (!other.ok || other.memory || dependency(candidate,other)) { safe=false; break; }
            }
            if (!safe) continue;
            ICUnit moving=result.instructions[candidate_pos];
            result.instructions.erase(result.instructions.begin()+static_cast<std::ptrdiff_t>(candidate_pos));
            result.instructions.insert(result.instructions.begin()+static_cast<std::ptrdiff_t>(load_pos+1),moving);
            ++result.moved_instructions;
            break;
        }
    }
    refreshIndices(result,block); return result;
}
} // namespace Kites::optimizer
