#include "suffice/boolean_proof.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace suffice;
void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(std::string("Boolean proof test: ")+message);
}
template<class Exception,class F> void rejects(F&& f,const char* message) {
    bool caught=false;try {f();} catch(const Exception&) {caught=true;}require(caught,message);
}
void all_three_variable_functions() {
    BooleanProof proof(1024,8000000);
    const std::array<BooleanId,3> variables{proof.variable(0),proof.variable(1),proof.variable(2)};
    std::array<BooleanId,8> minterms{};
    for(unsigned assignment=0;assignment<8;++assignment) {
        auto term=BooleanId{1};
        for(unsigned bit=0;bit<3;++bit)term=proof.apply(BooleanOp::And,term,
            (assignment&(1U<<bit))?variables[bit]:proof.negate(variables[bit]));
        minterms[assignment]=term;
    }
    std::array<BooleanId,256> functions{};
    for(unsigned truth=0;truth<256;++truth) {
        auto id=BooleanId{0};
        for(unsigned assignment=0;assignment<8;++assignment)if(truth&(1U<<assignment))
            id=proof.apply(BooleanOp::Or,id,minterms[assignment]);
        functions[truth]=id;
        for(unsigned assignment=0;assignment<8;++assignment) {
            const std::array<std::uint8_t,3> bits{std::uint8_t(assignment&1),std::uint8_t((assignment>>1)&1),std::uint8_t((assignment>>2)&1)};
            require(proof.evaluate(id,bits)==bool(truth&(1U<<assignment)),"DNF differs from independent truth table");
        }
    }
    // All Boolean binary operators on every pair of three-input functions.
    // Expected tables use integer bitwise operations, not BDD traversal.
    for(unsigned a=0;a<256;++a)for(unsigned b=0;b<256;++b) {
        require(proof.apply(BooleanOp::And,functions[a],functions[b])==functions[a&b],"AND canonical table mismatch");
        require(proof.apply(BooleanOp::Or,functions[a],functions[b])==functions[a|b],"OR canonical table mismatch");
        require(proof.apply(BooleanOp::Xor,functions[a],functions[b])==functions[a^b],"XOR canonical table mismatch");
        const unsigned care=(a*37+b*19)&255;
        require(proof.equivalent_under(functions[care],functions[a],functions[b])==(((a^b)&care)==0),"care miter table mismatch");
    }
    for(unsigned a=0;a<256;++a)require(proof.negate(functions[a])==functions[a^255],"complement canonical mismatch");
    require(proof.node_count()<=1024 && proof.steps()<=8000000,"declared budgets exceeded");
}
void cumulative_substitution_regression() {
    BooleanProof proof(128,10000);
    const auto a=proof.variable(0),b=proof.variable(1);
    const auto care=proof.apply(BooleanOp::And,a,b),original=proof.apply(BooleanOp::Or,a,b);
    // Replacing A by false and replacing B by false are individually valid
    // against the ORIGINAL expression on this care set. Applying both makes
    // false and is wrong. A rewrite must check its cumulative candidate.
    require(proof.equivalent_under(care,original,b),"first substitution should be valid");
    require(proof.equivalent_under(care,original,a),"isolated second substitution should be valid");
    require(!proof.equivalent_under(care,original,0),"jointly invalid substitutions accepted");
    require(!proof.equivalent_under(care,b,0),"current-to-cumulative miter lost mismatch");
    require(proof.equivalent_under(0,a,b),"empty care set not recognized");
    require(!proof.equivalent_under(1,a,b),"unconstrained unequal variables equated");
}
void bounds_and_errors() {
    rejects<BooleanProofExhausted>([]{BooleanProof p(1,100);},"missing terminal budget accepted");
    rejects<BooleanProofExhausted>([]{BooleanProof p(2,100);(void)p.variable(0);},"node budget exceeded");
    rejects<BooleanProofExhausted>([]{BooleanProof p(10,0);(void)p.variable(0);},"zero work budget exceeded");
    BooleanProof p(32,1000);
    const auto a=p.variable(5);
    require(a==p.variable(5),"variable unique table changed identity");
    rejects<std::invalid_argument>([&]{(void)p.variable(std::numeric_limits<std::uint32_t>::max());},"terminal variable accepted");
    rejects<std::invalid_argument>([&]{(void)p.apply(BooleanOp::And,99,a);},"out-of-range node accepted");
    rejects<std::invalid_argument>([&]{(void)p.apply(static_cast<BooleanOp>(99),0,1);},"invalid opcode accepted");
    rejects<std::invalid_argument>([&]{const std::array<std::uint8_t,2> small{};(void)p.evaluate(a,small);},"short assignment accepted");
    BooleanProof deep(10000,100000);
    auto chain=BooleanId{1};
    // Descending construction adds each node above the existing chain, so it
    // fits the work budget. Negation then traverses the full decision depth.
    for(unsigned i=2048;i--;)chain=deep.apply(BooleanOp::And,deep.variable(i),chain);
    rejects<BooleanProofExhausted>([&]{(void)deep.negate(chain);},"proof recursion depth was unbounded");
    require(deep.node_count()<=10000 && deep.steps()<=100000,"depth exhaustion violated other limits");
}
} // namespace

void test_boolean_proof() {
    all_three_variable_functions();cumulative_substitution_regression();bounds_and_errors();
}
