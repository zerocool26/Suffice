#include "suffice/runtime.hpp"
#include "suffice/row_range.hpp"
#include "suffice/encoded_ranges.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SUFFICE_X86 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#else
#define SUFFICE_X86 0
#endif
#if SUFFICE_X86 && (defined(__GNUC__) || defined(__clang__))
#define SUFFICE_AVX2 __attribute__((target("avx2")))
#else
#define SUFFICE_AVX2
#endif

namespace suffice {
namespace {
std::uint64_t low_mask(unsigned n) noexcept { return n >= 64 ? ~std::uint64_t{} : ((std::uint64_t{1} << n) - 1); }
std::uint64_t clamp_index(std::uint64_t row, int delta, std::uint64_t rows) noexcept {
    if (delta < 0) {
        const auto d = static_cast<std::uint64_t>(-delta);
        return row < d ? 0 : row-d;
    }
    const auto d = static_cast<std::uint64_t>(delta);
    return d >= rows || row >= rows-d ? rows-1 : row+d;
}
void set_bits(std::span<std::uint64_t> out, std::uint64_t first, std::uint64_t bits, unsigned n) noexcept {
    bits &= low_mask(n);
    const auto word = static_cast<std::size_t>(first/64);
    const auto shift = static_cast<unsigned>(first%64);
    out[word] |= bits << shift;
    if (shift && n > 64-shift) out[word+1] |= bits >> (64-shift);
}
void set_range(std::span<std::uint64_t> out, std::uint64_t first, std::uint64_t n) noexcept {
    while(n) { const auto take = static_cast<unsigned>(std::min<std::uint64_t>(64-first%64,n)); set_bits(out,first,low_mask(take),take); first+=take; n-=take; }
}
void validate_mask(std::span<std::uint64_t> out, std::uint64_t rows) {
    if(out.size()!=mask_words(rows)) throw std::invalid_argument("output mask size does not match dataset");
    std::fill(out.begin(),out.end(),0);
}
std::int32_t raw_value(const std::uint8_t* data, std::uint64_t row, std::uint32_t channel, std::uint32_t channels) noexcept {
    const auto* p=data+(row*channels+channel)*4;
    const std::uint32_t v=std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8) | (std::uint32_t(p[2])<<16) | (std::uint32_t(p[3])<<24);
    return std::bit_cast<std::int32_t>(v);
}
i64 absolute(i64 x) noexcept { return x<0 ? -x : x; }
std::uint64_t line_bytes(std::span<const std::uint8_t> bytes) noexcept {
    if(bytes.empty()) return 0;
    const auto offset=reinterpret_cast<std::uintptr_t>(bytes.data())%64;
    return ((offset+bytes.size()+63)/64)*64;
}
bool constant_decision(const CompiledProgram& plan, std::uint64_t rows, std::span<std::uint64_t> mask, ExecutionStats& s) {
    const auto d=plan.decide(plan.output_bounds());
    if(d==Decision::Unknown) return false;
    if(d==Decision::True && rows) { set_range(mask,0,rows); s.accepted=rows; }
    s.resolved_by_bits[0]=rows;
    s.bulk_resolutions=rows ? 1 : 0;
    s.kernel="constant-contract";
    return true;
}

#if SUFFICE_X86
SUFFICE_AVX2 std::uint64_t count_avx2(std::span<const std::uint64_t> mask) noexcept {
    std::uint64_t a=0,b=0,c=0,d=0;std::size_t i=0;
    for(;i+4<=mask.size();i+=4) {
        a+=std::popcount(mask[i]);b+=std::popcount(mask[i+1]);
        c+=std::popcount(mask[i+2]);d+=std::popcount(mask[i+3]);
    }
    for(;i<mask.size();++i)a+=std::popcount(mask[i]);
    return a+b+c+d;
}
SUFFICE_AVX2 std::uint64_t fused_avx2(const RawDataset& data, const CompiledProgram& plan,
                                     std::span<std::uint64_t> mask, std::string_view name,
                                     std::uint64_t first_row, std::uint64_t row_count) noexcept {
    const auto rows=data.info().rows;
    const auto end_row=first_row+row_count;
    const auto* bytes=data.bytes().data();
    const auto threshold=plan.threshold();
    std::uint64_t count=0,row=first_row;
    auto scalar=[&](std::uint64_t r) {
        const i64 current=raw_value(bytes,r,0,1);
        i64 score=current;
        if(name=="delta") score=absolute(current-raw_value(bytes,r ? r-1 : 0,0,1));
        else if(name=="fir3") score=absolute(i64(raw_value(bytes,r ? r-1 : 0,0,1))-2*current+raw_value(bytes,std::min(r+1,rows-1),0,1));
        else if(name=="energy")score=current*current;
        const bool yes=score>threshold;
        if(yes) { mask[static_cast<std::size_t>((r-first_row)/64)] |= std::uint64_t{1}<<((r-first_row)%64); ++count; }
    };
    if(name=="identity" && threshold>=std::numeric_limits<std::int32_t>::min() && threshold<=std::numeric_limits<std::int32_t>::max()) {
        const auto t=_mm256_set1_epi32(static_cast<std::int32_t>(threshold));
        for(; row+8<=end_row; row+=8) {
            const auto x=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(bytes+row*4));
            const auto cmp=_mm256_cmpgt_epi32(x,t);
            const auto bits=threshold==-1 ? (static_cast<unsigned>(~_mm256_movemask_ps(_mm256_castsi256_ps(x)))&255u)
                :static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(cmp)));
            set_bits(mask,row-first_row,bits,8);
        }
    } else if(name=="energy") {
        const auto t=_mm256_set1_epi64x(threshold);
        auto spread=[](unsigned x){x=(x|(x<<2))&0x33u;return (x|(x<<1))&0x55u;};
        for(;row+8<=end_row;row+=8) {
            const auto x=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(bytes+row*4));
            const auto odd=_mm256_srli_epi64(x,32);
            const auto evens=_mm256_mul_epi32(x,x),odds=_mm256_mul_epi32(odd,odd);
            const auto e=static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(evens,t))));
            const auto o=static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(odds,t))));
            const auto bits=spread(e)|(spread(o)<<1);
            set_bits(mask,row-first_row,bits,8);
        }
    } else {
        for(; row<end_row && row<1; ++row) scalar(row);
        const auto t=_mm256_set1_epi64x(threshold),zero=_mm256_setzero_si256();
        for(; row+4<=end_row && row+4<rows; row+=4) {
            const auto a=_mm256_cvtepi32_epi64(_mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes+row*4)));
            auto v=a;
            if(name=="delta") {
                const auto prev=_mm256_cvtepi32_epi64(_mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes+(row-1)*4)));
                v=_mm256_sub_epi64(a,prev);
            } else if(name=="fir3") {
                const auto prev=_mm256_cvtepi32_epi64(_mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes+(row-1)*4)));
                const auto next=_mm256_cvtepi32_epi64(_mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes+(row+1)*4)));
                v=_mm256_sub_epi64(_mm256_add_epi64(prev,next),_mm256_slli_epi64(a,1));
            }
            if(name!="identity") { const auto sign=_mm256_cmpgt_epi64(zero,v); v=_mm256_sub_epi64(_mm256_xor_si256(v,sign),sign); }
            const auto bits=static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(v,t))));
            set_bits(mask,row-first_row,bits,4);
        }
    }
    for(;row<end_row;++row) scalar(row);
    count=0;for(const auto word:mask)count+=std::popcount(word);
    return count;
}

SUFFICE_AVX2 std::uint64_t ranges_avx2(const RawDataset& data,std::span<const Bounds> accepted,std::span<std::uint64_t> mask,std::uint64_t first_row,std::uint64_t row_count) noexcept {
    const auto end_row=first_row+row_count;const auto* bytes=data.bytes().data();std::uint64_t row=first_row,count=0;
    const auto ones=_mm256_set1_epi32(-1);
    for(;row+8<=end_row;row+=8) {
        const auto x=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(bytes+row*4));auto result=_mm256_setzero_si256();
        for(const auto range:accepted) {
            auto inside=ones;
            if(range.lo>std::numeric_limits<std::int32_t>::min())inside=_mm256_cmpgt_epi32(x,_mm256_set1_epi32(static_cast<std::int32_t>(range.lo-1)));
            if(range.hi<std::numeric_limits<std::int32_t>::max())inside=_mm256_andnot_si256(_mm256_cmpgt_epi32(x,_mm256_set1_epi32(static_cast<std::int32_t>(range.hi))),inside);
            result=_mm256_or_si256(result,inside);
        }
        const auto bits=static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(result)));set_bits(mask,row-first_row,bits,8);
    }
    for(;row<end_row;++row) {
        const i64 x=raw_value(bytes,row,0,1);bool yes=false;
        for(const auto range:accepted)yes=yes || (x>=range.lo&&x<=range.hi);
        if(yes){set_bits(mask,row-first_row,1,1);++count;}
    }
    count=0;for(const auto word:mask)count+=std::popcount(word);
    return count;
}
#endif
} // namespace

std::size_t mask_words(std::uint64_t rows) {
    if(rows>std::numeric_limits<std::size_t>::max()-63) throw std::overflow_error("mask size overflow");
    return static_cast<std::size_t>((rows+63)/64);
}
std::uint64_t mask_checksum(std::span<const std::uint64_t> mask) noexcept {
    std::uint64_t h=1469598103934665603ull;
    for(auto word:mask) { for(unsigned i=0;i<8;++i) {h^=word&255;h*=1099511628211ull;word>>=8;} }
    return h;
}
bool avx2_available() noexcept {
#if SUFFICE_X86 && (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("avx2");
#elif SUFFICE_X86 && defined(_MSC_VER)
    int regs[4]{}; __cpuid(regs,1);
    if((regs[2]&(1<<27))==0 || (regs[2]&(1<<28))==0 || (_xgetbv(0)&6)!=6) return false;
    __cpuidex(regs,7,0);return (regs[1]&(1<<5))!=0;
#else
    return false;
#endif
}
std::uint64_t mask_popcount(std::span<const std::uint64_t> mask) noexcept {
#if SUFFICE_X86
    if(avx2_available())return count_avx2(mask);
#endif
    std::uint64_t count=0;for(const auto word:mask)count+=std::popcount(word);return count;
}
bool is_fused_builtin(const CompiledProgram& p, std::string_view name) noexcept {
    if(name!="identity" && name!="delta" && name!="fir3" && name!="energy") return false;
    for(const auto& load:p.loads()) if(load.channel!=0) return false;
    // The caller must pass the actual builtin name; compare full instruction
    // structure as a defense against accidentally timing a different program.
    try {
        Bounds r{std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()};
        const auto b=builtin_program(name,p.threshold()).compile(std::span<const Bounds>(&r,1));
        if(b.nodes().size()!=p.nodes().size() || b.loads().size()!=p.loads().size() || b.output_node()!=p.output_node()) return false;
        for(std::size_t i=0;i<b.loads().size();++i) if(b.loads()[i].channel!=p.loads()[i].channel || b.loads()[i].offset!=p.loads()[i].offset) return false;
        for(std::size_t i=0;i<b.nodes().size();++i) {
            const auto& x=b.nodes()[i];const auto& y=p.nodes()[i];
            if(x.op!=y.op || x.a!=y.a || x.b!=y.b || x.immediate!=y.immediate || x.load_index!=y.load_index) return false;
        }
        return true;
    } catch(...) {return false;}
}

ProgressiveExecutor::ProgressiveExecutor(const PackedDataset& data,const CompiledProgram& plan,ExecutionOptions options)
    :data_(data),plan_(plan),options_(std::move(options)) {
    if(!plan.node_count())throw std::invalid_argument("executor requires a compiled program");
    if(options_.stages.empty() || options_.stages.back()!=32) throw std::invalid_argument("stages must end at 32");
    unsigned previous=0;
    for(auto b:options_.stages) {if(b<=previous || b>32) throw std::invalid_argument("stages must be strictly increasing in 1..32");previous=b;}
    const auto nodes=plan.nodes();
    bitset_plan_=options_.specialize && nodes.size()==1 && nodes[0].op==Op::Load && plan.loads().size()==1 && plan.loads()[0].offset==0;
    for(const auto& load:plan.loads()) {
        if(load.channel>=data.info().channels) throw std::invalid_argument("load channel outside dataset");
        const auto range=data.info().ranges[load.channel];
        if(load.range.lo>range.min || load.range.hi<range.max) throw std::invalid_argument("compiled input range does not cover dataset");
        if(!bitset_plan_ && std::find(channels_.begin(),channels_.end(),load.channel)==channels_.end()) channels_.push_back(load.channel);
    }
    if(bitset_plan_) {
        // The biased sign plane is already the exact mask for x > -1.
        // It needs neither comparator state nor a scalar representation.
        if(plan_.threshold()!=-1 || std::endian::native!=std::endian::little) {
            equal_bits_.resize((data.info().block_rows+63)/64);greater_bits_.resize(equal_bits_.size());
        }
        return;
    }
    if(options_.specialize && plan.loads().size()==1 && plan.loads()[0].offset==0) {
        const auto channel=plan.loads()[0].channel;const auto range=data.info().ranges[channel];
        preimage_=compile_preimage(plan,Bounds{range.min,range.max});
        if(preimage_.complete) {
            range_executor_=std::make_unique<EncodedRangeExecutor>(data,channel,
                std::span<const Bounds>(preimage_.accepted.data(),preimage_.count),options_.stages.front());
            channels_.clear();channels_.shrink_to_fit();
            return;
        }
    }
    const auto blocks_per_window=2u+static_cast<unsigned>((plan.max_offset()-plan.min_offset())/static_cast<int>(data.info().block_rows));
    cache_.resize(std::max<std::size_t>(1,channels_.size()*(blocks_per_window+1)));
    for(auto& r:cache_)r.prefix.resize(data.info().block_rows);
    unresolved_.resize(data.info().block_rows);
    needed_.resize(cache_.size());
}
ProgressiveExecutor::~ProgressiveExecutor()=default;
std::string ProgressiveExecutor::lowering_description() const {
    if(bitset_plan_)return plan_.threshold()==-1 && std::endian::native==std::endian::little
        ? "exact answer projection from the biased sign plane" : "direct bit-sliced input comparison";
    if(range_executor_) {
        std::ostringstream out;out<<"exact predicate preimage; visited="<<preimage_.visited<<"; accepted=";
        for(std::size_t i=0;i<preimage_.count;++i) {if(i)out<<',';out<<'['<<preimage_.accepted[i].lo<<','<<preimage_.accepted[i].hi<<']';}
        if(!preimage_.count)out<<"empty";
        return out.str();
    }
    return preimage_.reason.empty()?"generic progressive interval evaluation":"generic interval fallback: "+preimage_.reason;
}
std::uint64_t ProgressiveExecutor::scratch_bytes() const noexcept {
    if(range_executor_)return range_executor_->scratch_bytes()+options_.stages.capacity()*sizeof(unsigned)+sizeof(PredicatePreimage)+preimage_.reason.capacity();
    std::uint64_t total=cache_.capacity()*sizeof(Region)+channels_.capacity()*sizeof(std::uint32_t)+unresolved_.capacity()+needed_.capacity();
    total+=(equal_bits_.capacity()+greater_bits_.capacity())*sizeof(std::uint64_t)+options_.stages.capacity()*sizeof(unsigned);
    for(const auto& r:cache_)total+=r.prefix.capacity()*sizeof(std::uint32_t);
    if(bitset_plan_)return total;
    // Fixed evaluation arrays on the run() stack are included.
    return total+3*max_program_nodes*sizeof(Bounds)+max_program_nodes*sizeof(i64);
}
std::size_t ProgressiveExecutor::region(std::uint64_t block,std::uint32_t channel,ExecutionStats& s) {
    ++tick_;
    for(std::size_t i=0;i<cache_.size();++i) if(cache_[i].valid&&cache_[i].block==block&&cache_[i].channel==channel) {
        cache_[i].stamp=tick_;cache_[i].pinned=true;++s.cache_reuses;return i;
    }
    std::size_t choice=cache_.size();
    for(std::size_t i=0;i<cache_.size();++i) if(!cache_[i].pinned && (choice==cache_.size() || !cache_[i].valid || cache_[i].stamp<cache_[choice].stamp)) {choice=i;if(!cache_[i].valid)break;}
    if(choice==cache_.size())throw std::logic_error("prepared region cache is too small");
    auto& r=cache_[choice];r.valid=true;r.pinned=true;r.block=block;r.channel=channel;r.rows=data_.block_length(block);r.bits=0;r.stamp=tick_;
    const auto range=data_.info().ranges[channel];r.aggregate={range.min,range.max};
    std::fill_n(r.prefix.begin(),r.rows,0);++s.region_loads;return choice;
}
std::size_t ProgressiveExecutor::find_region(std::uint64_t block,std::uint32_t channel) const {
    for(std::size_t i=0;i<cache_.size();++i) if(cache_[i].valid&&cache_[i].block==block&&cache_[i].channel==channel)return i;
    throw std::logic_error("missing prepared input region");
}
Bounds ProgressiveExecutor::at(const Region& r,std::uint32_t local) const noexcept {
    const auto low=i64(r.prefix[local])-2147483648ll;
    const auto tail=static_cast<i64>((std::uint64_t{1}<<(32-r.bits))-1);
    const auto range=data_.info().ranges[r.channel];
    return {std::max<i64>(low,range.min),std::min<i64>(low+tail,range.max)};
}
void ProgressiveExecutor::refine(Region& r,unsigned bits,ExecutionStats& s) {
    if(bits<=r.bits)return;
    const auto bytes=data_.plane_view(r.block,r.channel,r.bits,bits-r.bits);
    const auto stride=data_.plane_bytes(r.block);
    s.requested_payload_bytes+=bytes.size();s.estimated_cacheline_bytes+=line_bytes(bytes);s.loaded_planes+=bits-r.bits;
    for(unsigned b=r.bits;b<bits;++b) {
        const auto* plane=bytes.data()+(b-r.bits)*stride;const auto weight=std::uint32_t{1}<<(31-b);
        for(std::uint32_t row=0;row<r.rows;row+=8) {
            const auto byte=plane[row/8];const auto take=std::min<std::uint32_t>(8,r.rows-row);
            for(std::uint32_t k=0;k<take;++k) r.prefix[row+k] |= ((byte>>k)&1u)*weight;
        }
    }
    r.bits=bits;r.aggregate=at(r,0);
    for(std::uint32_t row=1;row<r.rows;++row) {const auto bound=at(r,row);r.aggregate.lo=std::min(r.aggregate.lo,bound.lo);r.aggregate.hi=std::max(r.aggregate.hi,bound.hi);}
}
ExecutionStats ProgressiveExecutor::run_bitset(std::uint64_t first_row,std::uint64_t row_count,
                                                std::span<std::uint64_t> mask) {
    ExecutionStats s;s.rows=row_count;s.output_bytes=mask.size_bytes();s.peak_scratch_bytes=scratch_bytes();s.kernel="bitplane-predicate";
    const auto channel=plan_.loads()[0].channel;
    const auto block_rows=data_.info().block_rows;
    const auto end_row=first_row+row_count;
    const auto first_block=first_row/block_rows,last_block=(end_row-1)/block_rows;
    if constexpr(std::endian::native==std::endian::little) {
        if(plan_.threshold()==-1) {
            s.kernel="bitplane-answer-projection";
            for(auto block=first_block;block<=last_block;++block) {
                const auto block_first=block*block_rows;
                const auto begin=std::max(first_row,block_first);
                const auto end=std::min(end_row,block_first+data_.block_length(block));
                const auto plane=data_.plane_view(block,channel,0,1);
                range_detail::copy(mask,begin-first_row,plane,begin-block_first,end-begin);
                const auto byte_first=static_cast<std::size_t>((begin-block_first)/8);
                const auto byte_end=static_cast<std::size_t>((end-block_first+7)/8);
                const auto accessed=plane.subspan(byte_first,byte_end-byte_first);
                s.requested_payload_bytes+=accessed.size();s.estimated_cacheline_bytes+=line_bytes(accessed);
                ++s.loaded_planes;++s.region_loads;
            }
            s.accepted=mask_popcount(mask);s.resolved_by_bits[1]=row_count;
            return s;
        }
    }
    const auto t=static_cast<std::uint32_t>(plan_.threshold()+2147483648ll);
    for(auto block=first_block;block<=last_block;++block) {
        const auto block_first=block*block_rows;
        const auto length=data_.block_length(block);
        const auto begin=std::max(first_row,block_first)-block_first;
        const auto end=std::min(end_row,block_first+length)-block_first;
        const auto first_word=static_cast<std::size_t>(begin/64),end_word=static_cast<std::size_t>((end+63)/64);
        for(auto w=first_word;w<end_word;++w) {
            const auto lo=static_cast<unsigned>(std::max<std::uint64_t>(begin,w*64)-w*64);
            const auto hi=static_cast<unsigned>(std::min<std::uint64_t>(end,w*64+64)-w*64);
            equal_bits_[w]=low_mask(hi)&~low_mask(lo);greater_bits_[w]=0;
        }
        std::uint64_t remaining=end-begin;
        for(unsigned bit=0;bit<32&&remaining;++bit) {
            const auto plane=data_.plane_view(block,channel,bit,1);
            const auto accessed=plane.subspan(first_word*8,std::min(plane.size(),end_word*8)-first_word*8);
            s.requested_payload_bytes+=accessed.size();s.estimated_cacheline_bytes+=line_bytes(accessed);++s.loaded_planes;
            const bool threshold_one=(t & (std::uint32_t{1}<<(31-bit)))!=0;
            std::uint64_t next_remaining=0;
            for(auto w=first_word;w<end_word;++w) {
                const auto count=static_cast<unsigned>(std::min<std::uint64_t>(64,length-w*64));
                const auto x=range_detail::read(plane,w*64,count);
                const auto equal=equal_bits_[w];
                if(threshold_one)equal_bits_[w]=equal&x;
                else {greater_bits_[w]|=equal&x;equal_bits_[w]=equal&~x;}
                next_remaining+=std::popcount(equal_bits_[w]);
            }
            s.resolved_by_bits[bit+1]+=remaining-next_remaining;remaining=next_remaining;
            const auto suffix=low_mask(31-bit);
            if((std::uint64_t(t)&suffix)==suffix) {s.resolved_by_bits[bit+1]+=remaining;remaining=0;}
            if(bit+1==options_.stages.front())s.refined_rows+=remaining;
        }
        for(auto w=first_word;w<end_word;++w) {
            const auto lo=std::max<std::uint64_t>(begin,w*64);
            const auto hi=std::min<std::uint64_t>(end,w*64+64);
            const auto shift=static_cast<unsigned>(lo-w*64);
            set_bits(mask,block_first+lo-first_row,greater_bits_[w]>>shift,static_cast<unsigned>(hi-lo));
            s.accepted+=std::popcount(greater_bits_[w]);
        }
        ++s.region_loads;
    }
    return s;
}
ExecutionStats ProgressiveExecutor::run(std::span<std::uint64_t> mask) {
    return run_range(0,data_.info().rows,mask);
}
ExecutionStats ProgressiveExecutor::run_range(std::uint64_t first_row,std::uint64_t row_count,
                                              std::span<std::uint64_t> mask) {
    const auto rows=data_.info().rows;const auto end_row=range_detail::validate(rows,first_row,row_count,mask);
    ExecutionStats s;s.rows=row_count;s.output_bytes=mask.size_bytes();s.peak_scratch_bytes=scratch_bytes();s.kernel="progressive-interval";
    if(!row_count || constant_decision(plan_,row_count,mask,s))return s;
    if(bitset_plan_)return run_bitset(first_row,row_count,mask);
    if(range_executor_) {auto result=range_executor_->run_range(first_row,row_count,mask);result.peak_scratch_bytes=scratch_bytes();return result;}
    tick_=0;for(auto& r:cache_){r.valid=false;r.pinned=false;}
    const auto block_rows=data_.info().block_rows;
    std::array<Bounds,max_program_nodes> load_bounds{},aggregate_bounds{};
    std::array<i64,max_program_nodes> exact{};
    for(std::uint64_t output_block=first_row/block_rows;output_block<=(end_row-1)/block_rows;++output_block) {
        const auto block_first=output_block*block_rows;
        const auto first=std::max(first_row,block_first);
        const auto end=std::min(end_row,block_first+data_.block_length(output_block));
        const auto length=static_cast<std::uint32_t>(end-first);
        for(auto& r:cache_)r.pinned=false;
        const auto first_source=clamp_index(first,plan_.min_offset(),rows)/block_rows;
        const auto last_source=clamp_index(first+length-1,plan_.max_offset(),rows)/block_rows;
        for(auto channel:channels_) for(auto block=first_source;block<=last_source;++block)(void)region(block,channel,s);
        std::fill_n(unresolved_.begin(),length,1);std::uint64_t remaining=length;
        for(std::size_t stage=0;stage<options_.stages.size()&&remaining;++stage) {
            const auto bits=options_.stages[stage];std::fill(needed_.begin(),needed_.end(),0);
            for(std::uint32_t local=0;local<length;++local)if(unresolved_[local]) {
                for(const auto& load:plan_.loads()) {const auto row=clamp_index(first+local,load.offset,rows);needed_[find_region(row/block_rows,load.channel)]=1;}
            }
            for(std::size_t i=0;i<cache_.size();++i)if(needed_[i])refine(cache_[i],bits,s);
            std::size_t j=0;
            for(const auto& load:plan_.loads()) {
                Bounds bound{std::numeric_limits<i64>::max(),std::numeric_limits<i64>::min()};
                for(const auto& r:cache_)if(r.valid&&r.pinned&&r.channel==load.channel) {bound.lo=std::min(bound.lo,r.aggregate.lo);bound.hi=std::max(bound.hi,r.aggregate.hi);}
                aggregate_bounds[j++]={std::max(bound.lo,load.range.lo),std::min(bound.hi,load.range.hi)};
            }
            ++s.interval_evaluations;
            const auto bulk=plan_.decide(plan_.evaluate_bounds_unchecked(std::span<const Bounds>(aggregate_bounds.data(),plan_.loads().size())));
            if(bulk!=Decision::Unknown) {
                for(std::uint32_t local=0;local<length;++local)if(unresolved_[local]) {if(bulk==Decision::True){set_bits(mask,first+local-first_row,1,1);++s.accepted;}unresolved_[local]=0;}
                s.resolved_by_bits[bits]+=remaining;remaining=0;++s.bulk_resolutions;
            } else {
                for(std::uint32_t local=0;local<length;++local)if(unresolved_[local]) {
                    std::size_t k=0;
                    for(const auto& load:plan_.loads()) {const auto row=clamp_index(first+local,load.offset,rows);const auto& r=cache_[find_region(row/block_rows,load.channel)];load_bounds[k]=at(r,static_cast<std::uint32_t>(row%block_rows));exact[k]=load_bounds[k].lo;++k;}
                    Decision d;
                    if(bits==32) {++s.exact_evaluations;d=plan_.evaluate_exact_unchecked(std::span<const i64>(exact.data(),k))>plan_.threshold()?Decision::True:Decision::False;}
                    else {++s.interval_evaluations;d=plan_.decide(plan_.evaluate_bounds_unchecked(std::span<const Bounds>(load_bounds.data(),k)));}
                    if(d!=Decision::Unknown) {unresolved_[local]=0;--remaining;++s.resolved_by_bits[bits];if(d==Decision::True){set_bits(mask,first+local-first_row,1,1);++s.accepted;}}
                }
            }
            if(stage==0)s.refined_rows+=remaining;
        }
        if(remaining)throw std::logic_error("exact fallback left unresolved outputs");
    }
    return s;
}

static ExecutionStats run_dense_prepared(const RawDataset& data,const CompiledProgram& plan,std::span<std::uint64_t> mask,std::string_view builtin,bool allow_simd,std::uint64_t first_row,std::uint64_t row_count) {
    const auto rows=data.info().rows;const auto end_row=range_detail::validate(rows,first_row,row_count,mask);ExecutionStats s;s.rows=row_count;s.output_bytes=mask.size_bytes();s.peak_scratch_bytes=max_program_nodes*sizeof(i64);
    if(!row_count || constant_decision(plan,row_count,mask,s))return s;
    const bool fused=!builtin.empty();s.kernel=fused?"fused-scalar":"dense-ir";
    std::array<std::uint32_t,max_program_nodes> used{};std::size_t used_count=0;
    for(const auto& load:plan.loads())if(std::find(used.begin(),used.begin()+static_cast<std::ptrdiff_t>(used_count),load.channel)==used.begin()+static_cast<std::ptrdiff_t>(used_count))used[used_count++]=load.channel;
    s.requested_payload_bytes=row_count*4*used_count;
#if SUFFICE_X86
    if(fused && data.info().channels==1 && allow_simd && avx2_available()) {s.accepted=fused_avx2(data,plan,mask,builtin,first_row,row_count);s.kernel="fused-avx2";s.exact_evaluations=row_count;s.resolved_by_bits[32]=row_count;return s;}
#else
    (void)allow_simd;
#endif
    const auto* bytes=data.bytes().data();std::array<i64,max_program_nodes> values{};
    for(std::uint64_t row=first_row;row<end_row;++row) {
        i64 score{};
        if(fused) {
            const i64 curr=raw_value(bytes,row,0,data.info().channels);score=curr;
            if(builtin=="delta")score=absolute(curr-raw_value(bytes,row?row-1:0,0,data.info().channels));
            else if(builtin=="fir3")score=absolute(i64(raw_value(bytes,row?row-1:0,0,data.info().channels))-2*curr+raw_value(bytes,std::min(row+1,rows-1),0,data.info().channels));
            else if(builtin=="energy")score=curr*curr;
        } else {
            std::size_t k=0;for(const auto& load:plan.loads())values[k++]=raw_value(bytes,clamp_index(row,load.offset,rows),load.channel,data.info().channels);
            score=plan.evaluate_exact_unchecked(std::span<const i64>(values.data(),k));
        }
        if(score>plan.threshold()){set_bits(mask,row-first_row,1,1);++s.accepted;}
    }
    s.exact_evaluations=row_count;s.resolved_by_bits[32]=row_count;return s;
}
DenseExecutor::DenseExecutor(const RawDataset& data,const CompiledProgram& plan,std::string_view builtin,bool allow_simd,bool lower_preimage,bool inputs_already_verified)
    :data_(data),plan_(plan),allow_simd_(allow_simd) {
    if(!plan.node_count())throw std::invalid_argument("executor requires a compiled program");
    std::array<bool,1024> validated{};
    for(const auto& load:plan.loads()) {
        if(load.channel>=data.info().channels)throw std::invalid_argument("load channel outside raw dataset");
        if(!inputs_already_verified && !validated[load.channel]) {
            for(std::uint64_t row=0;row<data.info().rows;++row) {const auto value=data.value(row,load.channel);if(value<load.range.lo || value>load.range.hi)throw std::invalid_argument("raw value outside compiled input range");}
            validated[load.channel]=true;
        }
    }
    if(is_fused_builtin(plan,builtin))builtin_=builtin;
    if(lower_preimage && plan.loads().size()==1 && plan.loads()[0].offset==0 && builtin_!="identity") {
        const auto domain=plan.loads()[0].range;preimage_=compile_preimage(plan,domain);
        if(preimage_.complete && preimage_.count) {
            if(preimage_.accepted[0].lo==domain.lo)preimage_.accepted[0].lo=std::numeric_limits<std::int32_t>::min();
            if(preimage_.accepted[preimage_.count-1].hi==domain.hi)preimage_.accepted[preimage_.count-1].hi=std::numeric_limits<std::int32_t>::max();
        }
    }
}
ExecutionStats DenseExecutor::run(std::span<std::uint64_t> mask) const {
    return run_range(0,data_.info().rows,mask);
}
ExecutionStats DenseExecutor::run_range(std::uint64_t first_row,std::uint64_t row_count,
                                        std::span<std::uint64_t> mask) const {
    if(!preimage_.complete)return run_dense_prepared(data_,plan_,mask,builtin_,allow_simd_,first_row,row_count);
    const auto end_row=range_detail::validate(data_.info().rows,first_row,row_count,mask);
    const auto rows=row_count;ExecutionStats s;s.rows=rows;s.output_bytes=mask.size_bytes();s.peak_scratch_bytes=sizeof(PredicatePreimage);s.kernel="dense-preimage-scalar";
    if(!rows || constant_decision(plan_,rows,mask,s))return s;
    if(!preimage_.count){s.resolved_by_bits[0]=rows;s.kernel="constant-preimage";return s;}
    if(preimage_.count==1 && preimage_.accepted[0].lo==std::numeric_limits<std::int32_t>::min() && preimage_.accepted[0].hi==std::numeric_limits<std::int32_t>::max()) {
        set_range(mask,0,rows);s.accepted=rows;s.resolved_by_bits[0]=rows;s.kernel="constant-preimage";return s;
    }
    s.requested_payload_bytes=rows*4;const auto accepted=std::span<const Bounds>(preimage_.accepted.data(),preimage_.count);
#if SUFFICE_X86
    if(data_.info().channels==1 && plan_.loads()[0].channel==0 && allow_simd_ && avx2_available()) {s.accepted=ranges_avx2(data_,accepted,mask,first_row,row_count);s.kernel="dense-preimage-avx2";}
    else
#endif
    {
        const auto* bytes=data_.bytes().data();const auto channel=plan_.loads()[0].channel;
        for(std::uint64_t row=first_row;row<end_row;++row) {const i64 x=raw_value(bytes,row,channel,data_.info().channels);bool yes=false;for(const auto r:accepted)yes=yes||(x>=r.lo&&x<=r.hi);if(yes){set_bits(mask,row-first_row,1,1);++s.accepted;}}
    }
    s.exact_evaluations=row_count;s.resolved_by_bits[32]=row_count;return s;
}
ExecutionStats run_dense(const RawDataset& data,const CompiledProgram& plan,std::span<std::uint64_t> mask,std::string_view builtin,bool allow_simd) {
    return DenseExecutor(data,plan,builtin,allow_simd).run(mask);
}
ExecutionStats run_packed_full(const PackedDataset& data,const CompiledProgram& plan,std::span<std::uint64_t> mask,std::span<std::int32_t> scratch) {
    if(!plan.node_count())throw std::invalid_argument("executor requires a compiled program");
    const auto rows=data.info().rows;validate_mask(mask,rows);ExecutionStats s;s.rows=rows;s.output_bytes=mask.size_bytes();s.kernel="packed-full-ir";
    // Three source blocks suffice because offsets are bounded by16 and blocks>=32.
    const auto stride=data.info().block_rows;const std::size_t slots=3*data.info().channels;
    if(scratch.size()<slots*stride)throw std::invalid_argument("packed-full scratch is too small");
    std::array<std::uint32_t,max_program_nodes> used_channels{};std::size_t used_count=0;
    for(const auto& load:plan.loads()) {
        if(load.channel>=data.info().channels)throw std::invalid_argument("load channel outside packed dataset");
        const auto r=data.info().ranges[load.channel];
        if(load.range.lo>r.min || load.range.hi<r.max)throw std::invalid_argument("compiled range does not cover packed dataset");
        if(std::find(used_channels.begin(),used_channels.begin()+static_cast<std::ptrdiff_t>(used_count),load.channel)==used_channels.begin()+static_cast<std::ptrdiff_t>(used_count))used_channels[used_count++]=load.channel;
    }
    s.peak_scratch_bytes=scratch.size_bytes()+max_program_nodes*sizeof(i64)+128;
    if(!rows || constant_decision(plan,rows,mask,s))return s;
    std::array<i64,max_program_nodes> values{};
    std::array<std::uint64_t,3> resident{};resident.fill(std::numeric_limits<std::uint64_t>::max());
    for(std::uint64_t block=0;block<data.block_count();++block) {
        const auto first=block*stride;const auto length=data.block_length(block);
        const auto begin=clamp_index(first,plan.min_offset(),rows)/stride,end=clamp_index(first+length-1,plan.max_offset(),rows)/stride;
        if(end-begin>=3)throw std::logic_error("packed full window exceeds scratch");
        for(auto b=begin;b<=end;++b)if(resident[b%3]!=b) {
            for(std::size_t c=0;c<used_count;++c) {
                const auto channel=used_channels[c];
                auto dest=scratch.subspan(static_cast<std::size_t>((b%3)*data.info().channels+channel)*stride,stride);
                data.decode_block(b,channel,dest);s.requested_payload_bytes+=data.plane_bytes(b)*32;++s.region_loads;
            }
            resident[b%3]=b;
        }
        for(std::uint32_t local=0;local<length;++local) {
            std::size_t k=0;for(const auto& load:plan.loads()) {const auto row=clamp_index(first+local,load.offset,rows);const auto pos=static_cast<std::size_t>(((row/stride)%3)*data.info().channels+load.channel)*stride+row%stride;values[k++]=scratch[pos];}
            if(plan.evaluate_exact_unchecked(std::span<const i64>(values.data(),k))>plan.threshold()) {set_bits(mask,first+local,1,1);++s.accepted;}
        }
    }
    s.exact_evaluations=rows;s.resolved_by_bits[32]=rows;return s;
}
} // namespace suffice
