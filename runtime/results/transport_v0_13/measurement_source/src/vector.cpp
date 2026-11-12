#include "suffice/vector.hpp"
#include "suffice/row_range.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SUFFICE_VECTOR_X86 1
#include <immintrin.h>
#else
#define SUFFICE_VECTOR_X86 0
#endif
#if SUFFICE_VECTOR_X86 && (defined(__GNUC__) || defined(__clang__))
#define SUFFICE_VECTOR_AVX2 __attribute__((target("avx2")))
#else
#define SUFFICE_VECTOR_AVX2
#endif

namespace suffice {
namespace {

std::uint64_t clamp_row(std::uint64_t row, int offset, std::uint64_t rows) noexcept {
    if (offset < 0) {
        const auto d = static_cast<std::uint64_t>(-offset);
        return row < d ? 0 : row - d;
    }
    const auto d = static_cast<std::uint64_t>(offset);
    return d >= rows || row >= rows - d ? rows - 1 : row + d;
}

std::uint64_t low_mask(unsigned n) noexcept {
    return n >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << n) - 1;
}

std::int32_t read_raw(const std::uint8_t* bytes, std::uint64_t row,
                      std::uint32_t channel, std::uint32_t channels) noexcept {
    const auto* p = bytes + (row * channels + channel) * 4;
    std::uint32_t word;
    if constexpr (std::endian::native == std::endian::little) std::memcpy(&word, p, 4);
    else word = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    return std::bit_cast<std::int32_t>(word);
}

unsigned operands(const Instruction& instruction) noexcept {
    switch (instruction.op) {
    case Op::Load: case Op::Constant: return 0;
    case Op::MulConst: case Op::Abs: case Op::Square: return 1;
    default: return 2;
    }
}

void scalar_operation(const Instruction& instruction, const i64* a, const i64* b,
                      i64* out, std::size_t count) noexcept {
    switch (instruction.op) {
    case Op::Constant: std::fill_n(out, count, instruction.immediate); break;
    case Op::Add: for (std::size_t i=0;i<count;++i) out[i]=a[i]+b[i]; break;
    case Op::Sub: for (std::size_t i=0;i<count;++i) out[i]=a[i]-b[i]; break;
    case Op::MulConst: for (std::size_t i=0;i<count;++i) out[i]=a[i]*instruction.immediate; break;
    case Op::Abs: for (std::size_t i=0;i<count;++i) out[i]=a[i]<0?-a[i]:a[i]; break;
    case Op::Square: for (std::size_t i=0;i<count;++i) out[i]=a[i]*a[i]; break;
    case Op::Min: for (std::size_t i=0;i<count;++i) out[i]=std::min(a[i],b[i]); break;
    case Op::Max: for (std::size_t i=0;i<count;++i) out[i]=std::max(a[i],b[i]); break;
    case Op::Load: break;
    }
}

#if SUFFICE_VECTOR_X86
SUFFICE_VECTOR_AVX2 __m256i multiply_low64(__m256i a, __m256i b) noexcept {
    // The low 64 product bits are identical for signed and unsigned operands.
    // The IR has already proved that the signed mathematical result fits i64.
    const auto low = _mm256_mul_epu32(a, b);
    const auto cross1 = _mm256_mul_epu32(_mm256_srli_epi64(a,32), b);
    const auto cross2 = _mm256_mul_epu32(a, _mm256_srli_epi64(b,32));
    return _mm256_add_epi64(low, _mm256_slli_epi64(_mm256_add_epi64(cross1,cross2),32));
}

SUFFICE_VECTOR_AVX2 void widen_contiguous(const std::int32_t* source,i64* out,std::size_t count) noexcept {
    std::size_t i=0;
    for(;i+4<=count;i+=4)_mm256_storeu_si256(reinterpret_cast<__m256i*>(out+i),
        _mm256_cvtepi32_epi64(_mm_loadu_si128(reinterpret_cast<const __m128i*>(source+i))));
    for(;i<count;++i)out[i]=source[i];
}

SUFFICE_VECTOR_AVX2 void vector_operation(const Instruction& instruction, const i64* a,
                                          const i64* b, i64* out, std::size_t count) noexcept {
    const auto immediate = _mm256_set1_epi64x(instruction.immediate);
    const auto zero = _mm256_setzero_si256();
    std::size_t i=0;
    // Dispatch once per operator/tile, rather than once per value/operator.
    switch (instruction.op) {
    case Op::Constant:
        for (;i+4<=count;i+=4) _mm256_storeu_si256(reinterpret_cast<__m256i*>(out+i),immediate);
        break;
    case Op::Add:
        for (;i+4<=count;i+=4) _mm256_storeu_si256(reinterpret_cast<__m256i*>(out+i),
            _mm256_add_epi64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+i)),_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+i))));
        break;
    case Op::Sub:
        for (;i+4<=count;i+=4) _mm256_storeu_si256(reinterpret_cast<__m256i*>(out+i),
            _mm256_sub_epi64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+i)),_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+i))));
        break;
    case Op::MulConst:
        for (;i+4<=count;i+=4) _mm256_storeu_si256(reinterpret_cast<__m256i*>(out+i),
            multiply_low64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+i)),immediate));
        break;
    case Op::Abs:
        for (;i+4<=count;i+=4) {
            const auto x=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+i));
            const auto sign=_mm256_cmpgt_epi64(zero,x);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out+i),_mm256_sub_epi64(_mm256_xor_si256(x,sign),sign));
        }
        break;
    case Op::Square:
        for (;i+4<=count;i+=4) {const auto x=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out+i),multiply_low64(x,x));}
        break;
    case Op::Min: case Op::Max:
        for (;i+4<=count;i+=4) {
            const auto x=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+i));
            const auto y=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+i));
            const auto greater=_mm256_cmpgt_epi64(x,y);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out+i),instruction.op==Op::Min?
                _mm256_blendv_epi8(x,y,greater):_mm256_blendv_epi8(y,x,greater));
        }
        break;
    case Op::Load: return;
    }
    scalar_operation(instruction,a+i,b+i,out+i,count-i);
}

SUFFICE_VECTOR_AVX2 void emit_vector(const i64* values, std::size_t count,
                                    std::uint64_t first, i64 threshold,
                                    std::span<std::uint64_t> mask) noexcept {
    const auto t=_mm256_set1_epi64x(threshold);std::size_t i=0;
    for (;i+4<=count;i+=4) {
        const auto v=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(values+i));
        const auto bits=static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(v,t))));
        const auto row=first+i;const auto shift=static_cast<unsigned>(row%64);
        mask[static_cast<std::size_t>(row/64)]|=std::uint64_t(bits)<<shift;
        if(shift>60)mask[static_cast<std::size_t>(row/64)+1]|=std::uint64_t(bits)>>(64-shift);
    }
    for (;i<count;++i) if(values[i]>threshold)mask[static_cast<std::size_t>((first+i)/64)]|=std::uint64_t{1}<<((first+i)%64);
}
#endif

} // namespace

VectorExecutor::VectorExecutor(const RawDataset& data,const CompiledProgram& plan,std::size_t tile_rows,bool allow_simd,bool inputs_already_verified)
    :raw_(&data),plan_(plan),tile_rows_(tile_rows) {
    prepare();
    use_simd_=use_simd_ && allow_simd;
    std::array<bool,1024> validated{};
    for(const auto& load:plan_.loads()) if(!inputs_already_verified && !validated[load.channel]) {
        for(std::uint64_t row=0;row<data.info().rows;++row) {
            const auto value=data.value(row,load.channel);
            if(value<load.range.lo || value>load.range.hi)throw std::invalid_argument("raw value outside vector plan range");
        }
        validated[load.channel]=true;
    }
}

VectorExecutor::VectorExecutor(const PackedDataset& data,const CompiledProgram& plan,std::size_t tile_rows,bool allow_simd)
    :packed_(&data),plan_(plan),tile_rows_(tile_rows) {
    prepare();
    use_simd_=use_simd_ && allow_simd;
    for(const auto& load:plan_.loads()) {
        const auto range=data.info().ranges[load.channel];
        if(load.range.lo>range.min || load.range.hi<range.max)throw std::invalid_argument("packed range exceeds vector plan range");
    }
    const auto values=std::uint64_t{3}*channels_.size()*data.info().block_rows;
    if(values>std::numeric_limits<std::size_t>::max()/sizeof(std::int32_t))throw std::overflow_error("vector decode workspace overflow");
    decoded_.resize(static_cast<std::size_t>(values));
}

void VectorExecutor::prepare() {
    if(!plan_.node_count())throw std::invalid_argument("vector executor requires a compiled program");
    if(!tile_rows_ || tile_rows_>65536)throw std::invalid_argument("vector tile rows must be in 1..65536");
    const auto& info=raw_?raw_->info():packed_->info();
    std::size_t load_index=0;
    for(const auto& load:plan_.loads()) {
        if(load.channel>=info.channels)throw std::invalid_argument("vector input channel outside dataset");
        auto it=std::find(channels_.begin(),channels_.end(),load.channel);
        if(it==channels_.end()) {channels_.push_back(load.channel);load_channels_[load_index]=static_cast<std::uint16_t>(channels_.size()-1);}
        else load_channels_[load_index]=static_cast<std::uint16_t>(it-channels_.begin());
        ++load_index;
    }
    std::array<unsigned,max_program_nodes> uses{};
    for(const auto& instruction:plan_.nodes()) {
        const auto count=operands(instruction);
        if(count>=1)++uses[instruction.a];
        if(count>=2)++uses[instruction.b];
    }
    ++uses[plan_.output_node()];
    std::array<std::uint16_t,max_program_nodes> free_slots{};std::size_t free_count=0;
    std::size_t index=0;
    for(const auto& instruction:plan_.nodes()) {
        const auto count=operands(instruction);
        bool reused=false;std::uint16_t output_slot=0;
        if(count>=1 && uses[instruction.a]==(count==2 && instruction.a==instruction.b?2u:1u)) {output_slot=slots_[instruction.a];reused=true;}
        else if(count==2 && uses[instruction.b]==1) {output_slot=slots_[instruction.b];reused=true;}
        if(!reused)output_slot=free_count?free_slots[--free_count]:static_cast<std::uint16_t>(slot_count_++);
        slots_[index++]=output_slot;
        auto consume=[&](NodeId id){if(--uses[id]==0 && slots_[id]!=output_slot)free_slots[free_count++]=slots_[id];};
        if(count>=1)consume(instruction.a);
        if(count>=2)consume(instruction.b);
    }
    arena_.resize(slot_count_*tile_rows_);
    use_simd_=avx2_available();
}

std::uint64_t VectorExecutor::scratch_bytes() const noexcept {
    return arena_.capacity()*sizeof(i64)+decoded_.capacity()*sizeof(std::int32_t)+
        channels_.capacity()*sizeof(std::uint32_t)+sizeof(slots_)+sizeof(load_channels_)+sizeof(resident_)+(packed_?128:0);
}

std::string VectorExecutor::description() const {
    std::ostringstream out;out<<(raw_?"raw":"packed")<<" tiled integer DAG; "<<slot_count_
        <<" live vector slots for "<<plan_.node_count()<<" nodes; tile="<<tile_rows_
        <<"; "<<(use_simd_?"AVX2":"scalar")<<"; decoded channels="<<channels_.size();return out.str();
}

void VectorExecutor::decode_region(std::uint64_t block,ExecutionStats& stats) {
    if(resident_[block%3]==block){++stats.cache_reuses;return;}
    const auto stride=packed_->info().block_rows;
    for(std::size_t i=0;i<channels_.size();++i) {
        const auto offset=static_cast<std::size_t>((block%3)*channels_.size()+i)*stride;
        packed_->decode_block(block,channels_[i],std::span<std::int32_t>(decoded_).subspan(offset,stride));
        const auto bytes=packed_->plane_view(block,channels_[i],0,32);
        stats.requested_payload_bytes+=bytes.size();
        const auto alignment=reinterpret_cast<std::uintptr_t>(bytes.data())%64;
        stats.estimated_cacheline_bytes+=((alignment+bytes.size()+63)/64)*64;
        ++stats.region_loads;stats.loaded_planes+=32;
    }
    resident_[block%3]=block;
}

ExecutionStats VectorExecutor::run(std::span<std::uint64_t> output) {
    return run_range(0, raw_ ? raw_->info().rows : packed_->info().rows, output);
}

ExecutionStats VectorExecutor::run_range(std::uint64_t first_row, std::uint64_t row_count,
                                         std::span<std::uint64_t> output) {
    const auto& info=raw_?raw_->info():packed_->info();const auto rows=info.rows;
    const auto end_row=range_detail::validate(rows,first_row,row_count,output);
    ExecutionStats stats;stats.rows=row_count;stats.output_bytes=output.size_bytes();stats.peak_scratch_bytes=scratch_bytes();
    stats.kernel=raw_?(use_simd_?"vector-raw-avx2":"vector-raw-scalar"):(use_simd_?"vector-packed-avx2":"vector-packed-scalar");
    if(!row_count)return stats;
    const auto decision=plan_.decide(plan_.output_bounds());
    if(decision!=Decision::Unknown) {
        if(decision==Decision::True){std::fill(output.begin(),output.end(),~std::uint64_t{0});if(row_count%64)output.back()&=low_mask(static_cast<unsigned>(row_count%64));stats.accepted=row_count;}
        stats.resolved_by_bits[0]=row_count;stats.kernel="vector-constant";return stats;
    }
    if(raw_)stats.requested_payload_bytes=row_count*4*channels_.size();
    resident_.fill(std::numeric_limits<std::uint64_t>::max());
    const auto* bytes=raw_?raw_->bytes().data():nullptr;
    for(std::uint64_t first=first_row;first<end_row;) {
        auto length=std::min<std::uint64_t>(tile_rows_,end_row-first);
        if(packed_) {
            length=std::min<std::uint64_t>(length,info.block_rows-first%info.block_rows);
            const auto begin=clamp_row(first,plan_.min_offset(),rows)/info.block_rows;
            const auto end=clamp_row(first+length-1,plan_.max_offset(),rows)/info.block_rows;
            for(auto block=begin;block<=end;++block)decode_region(block,stats);
        }
        std::size_t node=0;
        for(const auto& instruction:plan_.nodes()) {
            auto* out=arena_.data()+slots_[node]*tile_rows_;
            if(instruction.op==Op::Load) {
                const auto& load=plan_.loads()[instruction.load_index];
                std::size_t i=0;
                while(i<length) {
                    const auto current=first+i;
                    const auto row=clamp_row(current,load.offset,rows);
                    std::size_t count=static_cast<std::size_t>(length-i);
                    const bool left_clamp=load.offset<0 && current<static_cast<std::uint64_t>(-load.offset);
                    const bool right_clamp=load.offset>0 && (static_cast<std::uint64_t>(load.offset)>=rows || current>=rows-static_cast<std::uint64_t>(load.offset));
                    if(left_clamp)count=std::min<std::size_t>(count,static_cast<std::uint64_t>(-load.offset)-current);
                    else if(!right_clamp)count=std::min<std::size_t>(count,rows-row);
                    const bool repeated=left_clamp || right_clamp;
                    if(raw_) {
                        if(repeated)std::fill_n(out+i,count,read_raw(bytes,row,load.channel,info.channels));
                        else if(info.channels==1 && std::endian::native==std::endian::little) {
#if SUFFICE_VECTOR_X86
                            if(use_simd_)widen_contiguous(reinterpret_cast<const std::int32_t*>(bytes+row*4),out+i,count);
                            else
#endif
                                for(std::size_t k=0;k<count;++k)out[i+k]=read_raw(bytes,row+k,load.channel,info.channels);
                        } else for(std::size_t k=0;k<count;++k)out[i+k]=read_raw(bytes,row+k,load.channel,info.channels);
                    } else {
                        const auto local=row%info.block_rows;
                        const auto offset=static_cast<std::size_t>(((row/info.block_rows)%3)*channels_.size()+load_channels_[instruction.load_index])*info.block_rows+local;
                        if(repeated)std::fill_n(out+i,count,decoded_[offset]);
                        else {
                            count=std::min<std::size_t>(count,info.block_rows-local);
#if SUFFICE_VECTOR_X86
                            if(use_simd_)widen_contiguous(decoded_.data()+offset,out+i,count);
                            else
#endif
                                for(std::size_t k=0;k<count;++k)out[i+k]=decoded_[offset+k];
                        }
                    }
                    i+=count;
                }
            } else {
                const auto* a=arena_.data()+slots_[instruction.a]*tile_rows_;
                const auto* b=arena_.data()+slots_[instruction.b]*tile_rows_;
#if SUFFICE_VECTOR_X86
                if(use_simd_)vector_operation(instruction,a,b,out,static_cast<std::size_t>(length));
                else
#endif
                    scalar_operation(instruction,a,b,out,static_cast<std::size_t>(length));
            }
            ++node;
        }
        const auto* result=arena_.data()+slots_[plan_.output_node()]*tile_rows_;
#if SUFFICE_VECTOR_X86
        if(use_simd_)emit_vector(result,static_cast<std::size_t>(length),first-first_row,plan_.threshold(),output);
        else
#endif
            for(std::size_t i=0;i<length;++i)if(result[i]>plan_.threshold())output[static_cast<std::size_t>((first-first_row+i)/64)]|=std::uint64_t{1}<<((first-first_row+i)%64);
        first+=length;
    }
    stats.accepted=mask_popcount(output);stats.exact_evaluations=row_count;stats.resolved_by_bits[32]=row_count;return stats;
}

} // namespace suffice
