#include "suffice/payload.hpp"
#include <zstd.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace suffice::payload {
namespace {
[[noreturn]] void fail(const std::string& s) { throw std::runtime_error("payload: "+s); }
std::uint32_t u32(const std::uint8_t* p) noexcept {
    return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);
}
std::uint64_t u64(const std::uint8_t* p) noexcept {return u32(p)|(std::uint64_t(u32(p+4))<<32);}
std::uint64_t add(std::uint64_t a,std::uint64_t b) {
    if(b>UINT64_MAX-a)fail("size overflow");
    return a+b;
}
std::uint64_t mul(std::uint64_t a,std::uint64_t b) {
    if(a && b>UINT64_MAX/a)fail("size overflow");
    return a*b;
}
void range(std::uint64_t rows,std::uint64_t first,std::uint64_t count) {
    if(first>rows || count>rows-first)fail("row range outside snapshot");
}
std::uint64_t metadata_hash(std::span<const std::uint8_t> b) {
    auto h=std::uint64_t{14695981039346656037ULL};
    for(std::size_t i=0;i<b.size();++i)h=(h^((i>=56 && i<64)?0:b[i]))*1099511628211ULL;
    return h;
}
std::vector<std::uint8_t> read_file(const std::filesystem::path& p,std::uint64_t cap) {
    if(!std::filesystem::is_regular_file(p))fail("input is not a regular file");
    const auto n=std::filesystem::file_size(p);
    if(n>cap || n>SIZE_MAX || n>std::uint64_t(std::numeric_limits<std::streamsize>::max()))fail("input exceeds byte budget");
    std::ifstream in(p,std::ios::binary);
    if(!in)fail("cannot open input");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n));
    if(n && !in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(n)))fail("short input read");
    char extra{};if(in.get(extra))fail("input grew during read");
    if(!in.eof())fail("input read failed");
    return bytes;
}
std::vector<Bounds> ranges(const Snapshot& s) {
    std::vector<Bounds> r;
    for(auto c:s.packed_features().info().ranges)r.push_back({c.min,c.max});
    return r;
}
std::uint64_t shifted_row(std::uint64_t row,int offset,std::uint64_t rows) {
    if(offset<0)return row<std::uint64_t(-offset)?0:row-std::uint64_t(-offset);
    const auto delta=std::uint64_t(offset);
    return delta>rows-1-row?rows-1:row+delta;
}
void merge_word(std::span<std::uint64_t> output,std::uint64_t offset,
                std::uint64_t bits,unsigned count) {
    if(count<64)bits&=(UINT64_C(1)<<count)-1;
    const auto index=static_cast<std::size_t>(offset/64);const auto shift=unsigned(offset%64);
    output[index]|=bits<<shift;
    if(shift && count>64-shift)output[index+1]|=bits>>(64-shift);
}
}
std::uint64_t fnv64(std::span<const std::uint8_t> b) noexcept {
    auto h=std::uint64_t{14695981039346656037ULL};
    for(auto v:b)h=(h^v)*1099511628211ULL;
    return h;
}
std::string zstd_version(){return ZSTD_versionString();}
const char* backend_name(Backend b) noexcept {
    switch(b){case Backend::Auto:return "auto";case Backend::Wordcode:return "wordcode";
    case Backend::Zoned:return "zoned";
    case Backend::Dense:return "dense";case Backend::Scalar:return "scalar";}
    return "invalid";
}
Snapshot::Snapshot(std::span<const std::uint8_t> raw,std::uint32_t channels,
    std::span<const std::uint8_t> packed,std::span<const std::uint8_t> payload,Limits limits)
    :raw_(RawDataset::copy_bytes(raw,channels)),packed_(PackedDataset::copy_bytes(packed)),
     payload_(payload.begin(),payload.end()) {
    if(raw_.info().rows>limits.max_rows)fail("row budget exceeded");
    if(raw_.info().rows!=packed_.info().rows || channels!=packed_.info().channels)fail("feature dimensions differ");
    packed_.verify_payload();
    // Exact byte/value equality, rather than just equality of range checksums.
    std::vector<std::int32_t> decoded(packed_.info().block_rows);
    const auto bound_bytes=mul(mul(packed_.block_count(),channels),sizeof(Bounds));
    if(add(add(add(raw.size(),packed.size()),payload.size()),bound_bytes)>limits.max_snapshot_bytes)
        fail("snapshot plus block bounds exceeds byte budget");
    block_ranges_.resize(static_cast<std::size_t>(packed_.block_count())*channels);
    std::uint64_t first=0;
    for(std::uint64_t block=0;block<packed_.block_count();++block) {
        const auto length=packed_.block_length(block);
        for(std::uint32_t c=0;c<channels;++c) {
            packed_.decode_block(block,c,decoded);
            const auto mm=std::minmax_element(decoded.begin(),decoded.begin()+length);
            block_ranges_[static_cast<std::size_t>(block)*channels+c]={*mm.first,*mm.second};
            for(std::uint32_t i=0;i<length;++i)
                if(decoded[i]!=raw_.value(first+i,c))fail("raw/packed feature identity differs");
        }
        first+=length;
    }
    if(payload_.size()<64 || std::memcmp(payload_.data(),"SUFPAY01",8)!=0)fail("bad store header");
    const auto* h=payload_.data();
    if(u32(h+8)!=1 || u32(h+12)!=1 || u32(h+28)!=24)fail("unsupported store format");
    if(u64(h+16)!=rows() || u32(h+24)!=channels)fail("payload dimensions differ");
    if(u64(h+32)!=fnv64(raw_.bytes()))fail("payload/feature binding checksum differs");
    const auto index_size=mul(rows(),24);
    const auto begin=add(64,index_size);
    if(u64(h+40)!=index_size || u64(h+48)!=payload_.size() || begin>payload_.size())fail("invalid index extent");
    if(u64(h+56)!=metadata_hash(std::span<const std::uint8_t>(payload_).first(static_cast<std::size_t>(begin))))fail("index checksum differs");
    std::uint64_t cursor=begin,total=0;
    std::uint32_t maximum=0;
    for(std::uint64_t r=0;r<rows();++r) {
        const auto* e=entry(r);const auto compressed=u32(e+8),uncompressed=u32(e+12);
        if(u64(e)!=cursor || compressed<4 || compressed>payload_.size()-cursor)fail("noncontiguous or invalid payload range");
        if(uncompressed>limits.max_row_bytes)fail("row decompression budget exceeded");
        total=add(total,uncompressed);if(total>limits.max_total_decoded_bytes)fail("total decompression budget exceeded");
        maximum=std::max(maximum,uncompressed);
        const auto* data=payload_.data()+cursor;
        if(u32(data)!=0xFD2FB528U)fail("payload must be an ordinary Zstandard frame");
        const auto frame_size=ZSTD_findFrameCompressedSize(data,compressed);
        if(ZSTD_isError(frame_size) || frame_size!=compressed)fail("payload must contain exactly one frame");
        const auto content=ZSTD_getFrameContentSize(data,compressed);
        if(content==ZSTD_CONTENTSIZE_ERROR || content==ZSTD_CONTENTSIZE_UNKNOWN || content!=uncompressed)fail("declared frame size differs");
        cursor=add(cursor,compressed);
    }
    if(cursor!=payload_.size())fail("trailing payload bytes");
    stats_={raw.size(),packed.size(),payload.size(),total,maximum,false};
    stats_.block_range_bytes=block_ranges_.capacity()*sizeof(Bounds);
}
std::shared_ptr<const Snapshot> Snapshot::copy_bytes(std::span<const std::uint8_t> raw,
    std::uint32_t channels,std::span<const std::uint8_t> packed,std::span<const std::uint8_t> payload,
    Limits limits,Validation validation) {
    if(validation!=Validation::Full && validation!=Validation::OnDemand)fail("invalid validation policy");
    if(channels==0 || channels>1024)fail("invalid channels");
    const auto width=mul(channels,4);
    if(raw.size()%width || raw.size()/width>limits.max_rows)fail("raw dimensions exceed budget");
    if(add(add(raw.size(),packed.size()),payload.size())>limits.max_snapshot_bytes)fail("snapshot byte budget exceeded");
    auto snapshot=std::shared_ptr<Snapshot>(new Snapshot(raw,channels,packed,payload,limits));
    if(validation==Validation::Full) {
        Decoder decoder(snapshot);
        for(std::uint64_t row=0;row<snapshot->rows();++row)(void)decoder.decode(row);
        snapshot->stats_.all_payloads_verified=true;
    }
    return snapshot;
}
std::shared_ptr<const Snapshot> Snapshot::load(const std::filesystem::path& raw,std::uint32_t channels,
    const std::filesystem::path& packed,const std::filesystem::path& payload,Limits limits,Validation validation) {
    // The transient input vectors are additional admission memory, explicitly
    // separate from the immutable retained-byte budget.
    auto r=read_file(raw,limits.max_snapshot_bytes);
    auto remaining=limits.max_snapshot_bytes-r.size();
    auto p=read_file(packed,remaining);remaining-=p.size();
    auto d=read_file(payload,remaining);
    return copy_bytes(r,channels,p,d,limits,validation);
}
const std::uint8_t* Snapshot::entry(std::uint64_t row) const {
    if(row>=rows())fail("payload row outside snapshot");
    return payload_.data()+64+static_cast<std::size_t>(row)*24;
}
std::span<const std::uint8_t> Snapshot::frame(std::uint64_t row) const {
    const auto* e=entry(row);return {payload_.data()+u64(e),u32(e+8)};
}
std::uint32_t Snapshot::decoded_size(std::uint64_t row) const{return u32(entry(row)+12);}
Bounds Snapshot::feature_bounds(std::uint32_t channel,std::uint64_t first,std::uint64_t count) const {
    if(channel>=channels() || count==0)fail("invalid feature bound request");
    range(rows(),first,count);
    const auto block_rows=packed_.info().block_rows;
    const auto begin=first/block_rows,end=(first+count-1)/block_rows;
    auto result=block_ranges_[static_cast<std::size_t>(begin)*channels()+channel];
    for(auto b=begin+1;b<=end;++b) {
        const auto r=block_ranges_[static_cast<std::size_t>(b)*channels()+channel];
        result.lo=std::min(result.lo,r.lo);result.hi=std::max(result.hi,r.hi);
    }
    return result;
}
Decoder::Decoder(std::shared_ptr<const Snapshot> snapshot):snapshot_(std::move(snapshot)) {
    if(!snapshot_)fail("null snapshot");
    scratch_.resize(snapshot_->stats().max_decoded_row_bytes);
    context_=ZSTD_createDCtx();if(!context_)throw std::bad_alloc();
}
Decoder::~Decoder(){if(context_)ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(context_));}
std::span<const std::uint8_t> Decoder::decode(std::uint64_t row) {
    const auto f=snapshot_->frame(row);const auto expected=snapshot_->decoded_size(row);
    const auto n=ZSTD_decompressDCtx(static_cast<ZSTD_DCtx*>(context_),
        scratch_.data(),scratch_.size(),f.data(),f.size());
    if(ZSTD_isError(n))fail(std::string("decompression failed: ")+ZSTD_getErrorName(n));
    if(n!=expected)fail("decoded length differs");
    const std::span<const std::uint8_t> result(scratch_.data(),n);
    // Full admission already verified these exact immutable bytes once. Avoid
    // repeating the additional FNV pass on each query; zstd still decodes and
    // checks its frame, and length is checked above. On-demand admission verifies
    // the selected row's content checksum here on every decode.
    if(!snapshot_->stats().all_payloads_verified &&
       fnv64(result)!=u64(snapshot_->entry(row)+16))fail("decoded checksum differs");
    return result;
}
std::uint64_t Decoder::scratch_bytes() const noexcept {
    return scratch_.capacity()+ZSTD_sizeof_DCtx(static_cast<const ZSTD_DCtx*>(context_));
}
Query::Query(std::shared_ptr<const Snapshot> snapshot,const Program& p):snapshot_(std::move(snapshot)) {
    if(!snapshot_)fail("null snapshot");
    program_=p.compile(ranges(*snapshot_));
    predicate_=compile_predicate(program_);
    wordcode_=compile_wordcode(predicate_);
    const auto& data=snapshot_->packed_features();
    block_decisions_.reserve(static_cast<std::size_t>(data.block_count()));
    for(std::uint64_t block=0;block<data.block_count();++block) {
        const auto first=block*data.info().block_rows,last=first+data.block_length(block)-1;
        std::array<Bounds,max_program_nodes> input{};std::size_t index=0;
        for(const auto& load:program_.loads()) {
            const auto lo=shifted_row(first,load.offset,snapshot_->rows());
            const auto hi=shifted_row(last,load.offset,snapshot_->rows());
            input[index++]=snapshot_->feature_bounds(load.channel,lo,hi-lo+1);
        }
        block_decisions_.push_back(program_.decide(input));
    }
}
std::shared_ptr<const Query> Query::prepare(std::shared_ptr<const Snapshot> s,const Program& p) {
    return std::shared_ptr<const Query>(new Query(std::move(s),p));
}
Decision Query::constant_decision() const noexcept{return program_.decide(program_.output_bounds());}
Worker::Worker(std::shared_ptr<const Query> query):query_(std::move(query)),
    decoder_(query_?query_->snapshot():std::shared_ptr<const Snapshot>{}) {
    if(!query_)fail("null query");
    const auto& s=*query_->snapshot();
    mask_.resize(static_cast<std::size_t>(s.rows()/64+(s.rows()%64!=0)));
    block_mask_.resize(s.packed_features().info().block_rows/64+
        (s.packed_features().info().block_rows%64!=0));
    if(query_->wordcode().complete)word_=std::make_unique<WordcodeExecutor>(s.packed_features(),query_->wordcode());
    if(query_->predicate().complete)dense_=std::make_unique<DensePredicateExecutor>(s.raw_features(),query_->predicate(),true,true);
}
RunStats Worker::select(Backend backend,std::uint64_t first,std::uint64_t count) {
    const auto& s=*query_->snapshot();range(s.rows(),first,count);
    if(backend==Backend::Auto)backend=Backend::Zoned;
    if(backend==Backend::Wordcode && !word_)fail("wordcode lowering incomplete");
    if(backend==Backend::Dense && !dense_)fail("dense predicate lowering incomplete");
    if(backend!=Backend::Wordcode && backend!=Backend::Zoned && backend!=Backend::Dense && backend!=Backend::Scalar)fail("invalid backend");
    active_words_=static_cast<std::size_t>(count/64+(count%64!=0));
    auto mask=std::span<std::uint64_t>(mask_).first(active_words_);
    std::fill(mask.begin(),mask.end(),0);
    RunStats result;result.backend=backend;result.first_row=first;result.row_count=count;
    if(count==0)return result;
    auto scalar=[&](std::uint64_t start,std::uint64_t length) {
        const auto& program=query_->program();std::array<i64,max_program_nodes> input{};
        for(std::uint64_t i=0;i<length;++i) {
            std::size_t j=0;
            for(const auto& load:program.loads())input[j++]=s.raw_features().value(
                shifted_row(start+i,load.offset,s.rows()),load.channel);
            if(program.evaluate_exact_unchecked(input)>program.threshold()) {
                const auto local=start+i-first;mask[local/64]|=UINT64_C(1)<<(local%64);
            }
        }
        result.feature_requested_bytes=add(result.feature_requested_bytes,mul(mul(length,program.loads().size()),4));
    };
    if(backend==Backend::Wordcode)result.feature_requested_bytes=word_->run_range(first,count,mask).requested_payload_bytes;
    else if(backend==Backend::Dense)result.feature_requested_bytes=dense_->run_range(first,count,mask).requested_payload_bytes;
    else if(backend==Backend::Scalar)scalar(first,count);
    else for(auto start=first;start<first+count;) {
        const auto block_rows=s.packed_features().info().block_rows;
        const auto block=start/block_rows;
        const auto length=std::min(first+count-start,std::uint64_t(block_rows)-start%block_rows);
        const auto decision=query_->block_decisions()[static_cast<std::size_t>(block)];
        if(decision==Decision::False)result.zone_false_rows+=length;
        else if(decision==Decision::True) {
            result.zone_true_rows+=length;
            for(std::uint64_t i=0;i<length;i+=64)merge_word(mask,start+i-first,UINT64_MAX,
                static_cast<unsigned>(std::min<std::uint64_t>(64,length-i)));
        } else {
            result.zone_refined_rows+=length;
            if(word_) {
                auto temporary=std::span<std::uint64_t>(block_mask_).first(static_cast<std::size_t>((length+63)/64));
                result.feature_requested_bytes+=word_->run_range(start,length,temporary).requested_payload_bytes;
                for(std::uint64_t i=0;i<length;i+=64)merge_word(mask,start+i-first,temporary[static_cast<std::size_t>(i/64)],
                    static_cast<unsigned>(std::min<std::uint64_t>(64,length-i)));
            } else scalar(start,length);
        }
        start+=length;
    }
    return result;
}
Worker::SelectionSize Worker::selection_size(const RunStats& selected) const {
    SelectionSize result;
    const auto mask=last_mask();
    for(std::size_t w=0;w<mask.size();++w) {
        auto bits=mask[w];
        if(w+1==mask.size() && selected.row_count%64)
            bits&=(UINT64_C(1)<<(selected.row_count%64))-1;
        while(bits) {
            const auto row=selected.first_row+w*64+std::countr_zero(bits);
            result.rows=add(result.rows,1);
            result.bytes=add(result.bytes,query_->snapshot()->decoded_size(row));
            bits&=bits-1;
        }
    }
    return result;
}
RunStats Worker::deliver(RunStats result,RowSink& sink,ResultCache* capture,double* capture_ms) {
    const auto& s=*query_->snapshot();
    const auto first=result.first_row,count=result.row_count;
    const auto mask=last_mask();
    std::size_t index=0,offset=0;
    for(std::size_t w=0;w<mask.size();++w) {
        auto bits=mask[w];
        if(w+1==mask.size() && count%64)bits&=(UINT64_C(1)<<(count%64))-1;
        while(bits) {
            const auto bit=std::countr_zero(bits);const auto row=first+w*64+bit;
            const auto decoded=decoder_.decode(row);
            sink.row(row,decoded);
            if(capture) {
                const auto start=std::chrono::steady_clock::now();
                if(index>=capture->rows_.size() || offset>capture->bytes_.size() ||
                   decoded.size()>capture->bytes_.size()-offset)
                    fail("capture selection identity changed");
                capture->rows_[index++]={row,offset,decoded.size()};
                if(!decoded.empty())std::memcpy(capture->bytes_.data()+offset,decoded.data(),decoded.size());
                offset+=decoded.size();
                *capture_ms+=std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-start).count();
            }
            ++result.selected_rows;result.compressed_payload_bytes+=s.frame(row).size();
            result.decoded_payload_bytes+=decoded.size();result.output_bytes+=decoded.size();
            bits&=bits-1;
        }
    }
    if(capture && (index!=capture->rows_.size() || offset!=capture->bytes_.size()))
        fail("capture selection extent changed");
    return result;
}
RunStats Worker::run(Backend backend,std::uint64_t first,std::uint64_t count,RowSink& sink,OutputLimits limits) {
    auto selected=select(backend,first,count);
    if(limits.max_rows!=UINT64_MAX || limits.max_bytes!=UINT64_MAX) {
        const auto size=selection_size(selected);
        if(size.rows>limits.max_rows || size.bytes>limits.max_bytes)
            fail("selected output budget exceeded");
    }
    return deliver(selected,sink);
}
CaptureReport Worker::run_captured(Backend backend,std::uint64_t first,std::uint64_t count,
                                  RowSink& sink,std::uint64_t budget,OutputLimits limits) {
    auto selected=select(backend,first,count);
    CaptureReport report;
    const auto start=std::chrono::steady_clock::now();
    const auto size=selection_size(selected);
    if(size.rows>limits.max_rows || size.bytes>limits.max_bytes)
        fail("selected output budget exceeded");
    // Subtraction/division avoid overflow before any size_t conversion. The
    // authoritative capacity check also covers implementations that overallocate.
    if(size.bytes>budget || size.rows>(budget-size.bytes)/sizeof(ResultCache::Entry) ||
       size.bytes>SIZE_MAX || size.rows>SIZE_MAX/sizeof(ResultCache::Entry)) {
        report.oversized=true;
    } else {
        try {
            auto candidate=std::unique_ptr<ResultCache>(new ResultCache);
            candidate->query_=query_;candidate->first_=first;candidate->count_=count;
            candidate->rows_.resize(static_cast<std::size_t>(size.rows));
            candidate->bytes_.resize(static_cast<std::size_t>(size.bytes));
            if(candidate->storage_bytes()>budget)report.oversized=true;
            else report.cache=std::move(candidate);
        } catch(const std::bad_alloc&) {
            report.failed=true;
        } catch(const std::length_error&) {
            report.failed=true;
        }
    }
    report.capture_ms=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-start).count();
    report.pipeline=deliver(selected,sink,report.cache.get(),&report.capture_ms);
    return report;
}
std::span<const std::uint64_t> Worker::last_mask() const noexcept {return std::span<const std::uint64_t>(mask_).first(active_words_);}
std::uint64_t Worker::scratch_bytes() const noexcept {
    return (mask_.capacity()+block_mask_.capacity())*sizeof(std::uint64_t)+decoder_.scratch_bytes()+
        (word_?word_->scratch_bytes():0)+(dense_?dense_->scratch_bytes():0);
}
std::unique_ptr<ResultCache> ResultCache::build(std::shared_ptr<const Query> q,Backend b,
    std::uint64_t first,std::uint64_t count,std::uint64_t budget) {
    if(!q)fail("null query");
    range(q->snapshot()->rows(),first,count);
    auto result=std::unique_ptr<ResultCache>(new ResultCache);
    result->query_=std::move(q);result->first_=first;result->count_=count;
    // Two passes keep retained capacities exact: first count output bytes/rows,
    // then allocate once. This extra work belongs to cache construction cost.
    struct Counter final:RowSink {
        std::uint64_t rows{},bytes{},budget{};
        explicit Counter(std::uint64_t cap):budget(cap){}
        void row(std::uint64_t,std::span<const std::uint8_t> b) override {
            rows=add(rows,1);bytes=add(bytes,b.size());
            if(add(mul(rows,sizeof(Entry)),bytes)>budget)fail("result cache byte budget exceeded");
        }
    } counter(budget);
    Worker worker(result->query_);worker.run(b,first,count,counter);
    result->rows_.resize(static_cast<std::size_t>(counter.rows));
    result->bytes_.resize(static_cast<std::size_t>(counter.bytes));
    if(result->storage_bytes()>budget)fail("result cache capacity exceeds budget");
    struct Writer final:RowSink {
        ResultCache& result;std::size_t index{},offset{};
        explicit Writer(ResultCache& value):result(value){}
        void row(std::uint64_t row,std::span<const std::uint8_t> b) override {
            if(index>=result.rows_.size() || b.size()>result.bytes_.size()-offset)fail("cache construction changed");
            result.rows_[index++]={row,offset,b.size()};
            if(!b.empty())std::memcpy(result.bytes_.data()+offset,b.data(),b.size());
            offset+=b.size();
        }
    } writer(*result);
    worker.run(b,first,count,writer);
    if(writer.index!=result->rows_.size() || writer.offset!=result->bytes_.size())fail("cache construction changed");
    return result;
}
void ResultCache::replay(const std::shared_ptr<const Query>& query,std::uint64_t first,
    std::uint64_t count,RowSink& sink) const {
    if(query.get()!=query_.get() || first!=first_ || count!=count_)fail("cache query/snapshot/range identity differs");
    for(const auto& e:rows_)sink.row(e.row,std::span<const std::uint8_t>(bytes_).subspan(static_cast<std::size_t>(e.offset),static_cast<std::size_t>(e.length)));
}
std::uint64_t ResultCache::storage_bytes() const noexcept {
    return rows_.capacity()*sizeof(Entry)+bytes_.capacity();
}
} // namespace suffice::payload
