#include "suffice/payload.hpp"
#include "suffice/benchmark_order.hpp"
#include "sqlite_gate.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/resource.h>
#endif

using namespace suffice;
using namespace suffice::payload;
namespace {
using Clock=std::chrono::steady_clock;
std::uint64_t ns(Clock::time_point a,Clock::time_point b) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count());
}
std::uint64_t number(const char* s) {
    const std::string text(s);std::size_t used=0;
    if(text.empty() || text.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("invalid unsigned number");
    auto value=std::stoull(text,&used);if(used!=text.size())throw std::runtime_error("invalid number");return value;
}
std::string quote(const std::string& s) {
    std::string r="\"";
    for(unsigned char c:s){if(c=='"'||c=='\\'){r+='\\';r+=char(c);}else if(c=='\n')r+="\\n";
        else if(c<32)throw std::runtime_error("unsupported JSON control");else r+=char(c);}
    return r+'"';
}
std::int32_t load(const std::uint8_t* p) {
    std::uint32_t bits=std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);
    return std::bit_cast<std::int32_t>(bits);
}
enum class Kind{And2,And8,Overlap4};
template<Kind K> bool test(const std::uint8_t* p,std::int32_t threshold) {
    if constexpr(K==Kind::And2)return load(p)>threshold && load(p+4)>threshold;
    else if constexpr(K==Kind::And8) {
        bool yes=true;
        for(unsigned i=0;i<8;++i)yes&=load(p+4*i)>threshold;
        return yes;
    } else return (load(p)>threshold || load(p+4)>threshold) &&
                   (load(p+8)>threshold || load(p+12)>threshold);
}
bool test_kind(Kind kind,const std::uint8_t* p,std::int32_t t) {
    if(kind==Kind::And2)return test<Kind::And2>(p,t);
    if(kind==Kind::And8)return test<Kind::And8>(p,t);
    return test<Kind::Overlap4>(p,t);
}
Program make_program(Kind kind,std::int32_t threshold) {
    Program p;
    auto a=p.load(0);
    if(kind==Kind::And2)a=p.min(a,p.load(1));
    else if(kind==Kind::And8)for(unsigned c=1;c<8;++c)a=p.min(a,p.load(c));
    else a=p.min(p.max(a,p.load(1)),p.max(p.load(2),p.load(3)));
    p.output(a,threshold);return p;
}
std::string make_sql(Kind kind,std::int32_t threshold,Decision constant) {
    if(constant==Decision::True)return "1";
    if(constant==Decision::False)return "0";
    auto c=[&](unsigned i){return "c"+std::to_string(i)+">"+std::to_string(threshold);};
    if(kind==Kind::And2)return c(0)+" AND "+c(1);
    if(kind==Kind::And8){std::string s;for(unsigned i=0;i<8;++i){if(i)s+=" AND ";s+=c(i);}return s;}
    return "("+c(0)+" OR "+c(1)+") AND ("+c(2)+" OR "+c(3)+")";
}
struct Entry {std::uint64_t row{},offset{},length{};friend bool operator==(const Entry&,const Entry&)=default;};
struct Output final:RowSink {
    std::vector<Entry> entries;std::vector<std::uint8_t> bytes;
    std::size_t used_rows{},used_bytes{};
    Output(std::size_t rows,std::size_t size):entries(rows),bytes(size){}
    void reset(){used_rows=used_bytes=0;}
    void row(std::uint64_t id,std::span<const std::uint8_t> value) override {
        if(used_rows>=entries.size() || value.size()>bytes.size()-used_bytes)throw std::runtime_error("output capacity exceeded");
        entries[used_rows++]={id,used_bytes,value.size()};
        if(!value.empty())std::memcpy(bytes.data()+used_bytes,value.data(),value.size());
        used_bytes+=value.size();
    }
};
struct Reference {std::vector<std::uint8_t> storage;std::vector<std::pair<std::size_t,std::size_t>> rows;};
std::uint64_t little64(const std::uint8_t* p){std::uint64_t x=0;for(unsigned i=0;i<8;++i)x|=std::uint64_t(p[i])<<(8*i);return x;}
Reference reference(const std::string& path,std::uint64_t rows) {
    const auto size=std::filesystem::file_size(path);
    if(size>600ULL*1024*1024 || size<16)throw std::runtime_error("reference extent");
    Reference result;result.storage.resize(static_cast<std::size_t>(size));
    std::ifstream file(path,std::ios::binary);
    if(!file.read(reinterpret_cast<char*>(result.storage.data()),static_cast<std::streamsize>(size)))throw std::runtime_error("reference read");
    if(std::memcmp(result.storage.data(),"SUFREF01",8)!=0 || little64(result.storage.data()+8)!=rows)throw std::runtime_error("reference identity");
    std::size_t cursor=16;result.rows.reserve(static_cast<std::size_t>(rows));
    for(std::uint64_t row=0;row<rows;++row) {
        if(result.storage.size()-cursor<4)throw std::runtime_error("reference truncated");
        const auto n=std::bit_cast<std::uint32_t>(load(result.storage.data()+cursor));cursor+=4;
        if(n>result.storage.size()-cursor)throw std::runtime_error("reference row truncated");
        result.rows.emplace_back(cursor,n);cursor+=n;
    }
    if(cursor!=result.storage.size())throw std::runtime_error("reference trailing bytes");
    return result;
}
std::uint64_t output_hash(const Output& out) {
    auto h=std::uint64_t{14695981039346656037ULL};
    auto byte=[&](std::uint8_t b){h=(h^b)*1099511628211ULL;};
    for(const auto& e:out.entries){for(unsigned i=0;i<8;++i)byte(std::uint8_t(e.row>>(8*i)));
        for(unsigned i=0;i<8;++i)byte(std::uint8_t(e.length>>(8*i)));
        for(std::size_t i=0;i<e.length;++i)byte(out.bytes[e.offset+i]);}
    return h;
}
void equal(const Output& a,const Output& expected) {
    if(a.used_rows!=expected.entries.size() || a.used_bytes!=expected.bytes.size() ||
       a.entries!=expected.entries || a.bytes!=expected.bytes)throw std::runtime_error("complete output differs from original plaintext oracle");
}
struct Counts {std::uint64_t selected{},decoded_rows{},compressed{},decoded{},zone_true{},zone_false{},zone_refined{};};
void emit(Decoder& decoder,const Snapshot& snapshot,std::uint64_t row,Output& output,Counts& counts) {
    const auto value=decoder.decode(row);output.row(row,value);
    ++counts.selected;++counts.decoded_rows;
    counts.compressed+=snapshot.frame(row).size();counts.decoded+=value.size();
}
template<Kind K> Counts fused(const Query& query,Decoder& decoder,Output& output,
    std::uint64_t first,std::uint64_t count,std::int32_t threshold,bool zones,bool eager) {
    Counts result;const auto& snapshot=*query.snapshot();const auto bytes=snapshot.raw_features().bytes();
    const auto stride=std::size_t(snapshot.channels())*4;
    auto segment=[&](std::uint64_t begin,std::uint64_t n,Decision constant){
        if(!eager && constant==Decision::False)return;
        for(auto row=begin;row<begin+n;++row) {
            if(eager) {
                const auto value=decoder.decode(row);++result.decoded_rows;result.decoded+=value.size();
                result.compressed+=snapshot.frame(row).size();
                if(constant==Decision::True || (constant==Decision::Unknown && test<K>(bytes.data()+row*stride,threshold))) {
                    output.row(row,value);++result.selected;
                }
            } else if(constant==Decision::True || test<K>(bytes.data()+row*stride,threshold))
                emit(decoder,snapshot,row,output,result);
        }
    };
    if(!zones)segment(first,count,query.constant_decision());
    else for(auto begin=first;begin<first+count;) {
        const auto block_rows=snapshot.packed_features().info().block_rows;
        const auto n=std::min(first+count-begin,std::uint64_t(block_rows)-begin%block_rows);
        const auto d=query.block_decisions()[static_cast<std::size_t>(begin/block_rows)];
        if(d==Decision::True)result.zone_true+=n;else if(d==Decision::False)result.zone_false+=n;else result.zone_refined+=n;
        segment(begin,n,d);begin+=n;
    }
    return result;
}
Counts fused_kind(Kind kind,const Query& query,Decoder& decoder,Output& output,
    std::uint64_t first,std::uint64_t count,std::int32_t threshold,bool zones,bool eager) {
    if(kind==Kind::And2)return fused<Kind::And2>(query,decoder,output,first,count,threshold,zones,eager);
    if(kind==Kind::And8)return fused<Kind::And8>(query,decoder,output,first,count,threshold,zones,eager);
    return fused<Kind::Overlap4>(query,decoder,output,first,count,threshold,zones,eager);
}
}
int main(int argc,char** argv) {
try {
    if(argc!=12)throw std::runtime_error("usage: benchmark RAW PACKED PAYLOAD REFERENCE CHANNELS QUERY THRESHOLD FIRST COUNT ROUNDS SEED");
    const auto channels64=number(argv[5]);if(channels64>1024 || channels64==0)throw std::runtime_error("channels");
    const auto channels=static_cast<std::uint32_t>(channels64);const std::string kind_text=argv[6];
    const auto kind=kind_text=="and2"?Kind::And2:kind_text=="and8"?Kind::And8:kind_text=="overlap4"?Kind::Overlap4:throw std::runtime_error("unknown query");
    if(channels!=(kind==Kind::And2?2:kind==Kind::And8?8:4))throw std::runtime_error("query channels");
    const auto threshold64=number(argv[7]);if(threshold64>INT32_MAX)throw std::runtime_error("threshold");
    const auto threshold=static_cast<std::int32_t>(threshold64);
    const auto first=number(argv[8]),count=number(argv[9]),rounds64=number(argv[10]),seed=number(argv[11]);
    if(rounds64==0 || rounds64>10000 || rounds64%18)throw std::runtime_error("rounds must complete an 18-row cycle");
    const auto rounds=static_cast<unsigned>(rounds64);
    auto start=Clock::now();auto snapshot=Snapshot::load(argv[1],channels,argv[2],argv[3]);
    const auto admission_ns=ns(start,Clock::now());
    if(first>snapshot->rows() || count>snapshot->rows()-first)throw std::runtime_error("range");
    start=Clock::now();auto query=Query::prepare(snapshot,make_program(kind,threshold));
    const auto query_ns=ns(start,Clock::now());
    if(!query->predicate().complete || !query->wordcode().complete)throw std::runtime_error("benchmark requires matched complete plans");
    start=Clock::now();Worker word(query),word_duplicate(query),zoned(query),dense(query);
    Decoder eager_decoder(snapshot),fused_decoder(snapshot),zoned_decoder(snapshot),sqlite_decoder(snapshot);
    const auto workers_ns=ns(start,Clock::now());
    start=Clock::now();SqliteGate sql(snapshot->raw_features(),make_sql(kind,threshold,query->constant_decision()));
    const auto sqlite_ns=ns(start,Clock::now());
    start=Clock::now();auto cache=ResultCache::build(query,Backend::Zoned,first,count,512ULL<<20);
    const auto cache_ns=ns(start,Clock::now());
    start=Clock::now();auto plaintext=reference(argv[4],snapshot->rows());
    const auto raw=snapshot->raw_features().bytes();std::vector<std::uint64_t> expected_ids;
    std::size_t expected_bytes=0;
    for(std::uint64_t row=first;row<first+count;++row)
        if(test_kind(kind,raw.data()+row*channels*4,threshold)) {
            expected_ids.push_back(row);
            expected_bytes+=plaintext.rows[static_cast<std::size_t>(row)].second;
        }
    if(expected_bytes>512ULL<<20)throw std::runtime_error("benchmark output budget");
    Output expected(expected_ids.size(),expected_bytes);
    for(const auto row:expected_ids){const auto [offset,length]=plaintext.rows[static_cast<std::size_t>(row)];expected.row(row,std::span<const std::uint8_t>(plaintext.storage).subspan(offset,length));}
    const auto oracle_ns=ns(start,Clock::now());
    const auto expected_checksum=output_hash(expected);
    // Plaintext source is no longer needed once the exact selected output exists.
    plaintext=Reference{};
    Output output(expected.entries.size(),expected.bytes.size());
    constexpr std::array<const char*,9> names{"eager","fused","fused-zoned","dense","wordcode","zoned","sqlite","cached","wordcode-duplicate"};
    std::array<std::vector<std::uint64_t>,9> timings;
    std::array<Counts,9> counters{};
    auto run=[&](std::size_t candidate) {
        if(candidate==0)return fused_kind(kind,*query,eager_decoder,output,first,count,threshold,false,true);
        if(candidate==1)return fused_kind(kind,*query,fused_decoder,output,first,count,threshold,false,false);
        if(candidate==2)return fused_kind(kind,*query,zoned_decoder,output,first,count,threshold,true,false);
        if(candidate==6){
            struct Context {Decoder& decoder;const Snapshot& snapshot;Output& output;Counts counts;};
            Context context{sqlite_decoder,*snapshot,output,{}};
            sql.select(first,count,&context,[](void* pointer,std::uint64_t row){auto& c=*static_cast<Context*>(pointer);emit(c.decoder,c.snapshot,row,c.output,c.counts);});
            return context.counts;
        }
        if(candidate==7){cache->replay(query,first,count,output);Counts c;c.selected=output.used_rows;return c;}
        Worker* worker=candidate==3?&dense:candidate==4?&word:candidate==5?&zoned:&word_duplicate;
        const auto backend=candidate==3?Backend::Dense:candidate==5?Backend::Zoned:Backend::Wordcode;
        const auto r=worker->run(backend,first,count,output);
        return Counts{r.selected_rows,r.selected_rows,r.compressed_payload_bytes,r.decoded_payload_bytes,
                      r.zone_true_rows,r.zone_false_rows,r.zone_refined_rows};
    };
    std::uint64_t complete_checks=0;
    for(std::size_t c=0;c<names.size();++c){output.reset();counters[c]=run(c);equal(output,expected);++complete_checks;}
    const auto orders=balanced_timing_orders(names.size(),rounds,seed);
    for(const auto& order:orders)for(auto c:order) {
        output.reset();
        // Poison is outside the measured interval; every accepted byte and entry
        // must be written again. The same output allocation is used by all paths.
        std::fill(output.bytes.begin(),output.bytes.end(),std::uint8_t{0xA5});
        std::fill(output.entries.begin(),output.entries.end(),Entry{UINT64_MAX,UINT64_MAX,UINT64_MAX});
        const auto before=Clock::now();const auto observed=run(c);const auto after=Clock::now();
        const auto elapsed=ns(before,after);
        if(!elapsed)throw std::runtime_error("zero timer duration");
        timings[c].push_back(elapsed);equal(output,expected);++complete_checks;
        if(observed.selected!=counters[c].selected || observed.decoded_rows!=counters[c].decoded_rows ||
           observed.compressed!=counters[c].compressed || observed.decoded!=counters[c].decoded)
            throw std::runtime_error("candidate work changed");
    }
    std::uint64_t peak_kib=0;
#ifndef _WIN32
    rusage usage{};if(getrusage(RUSAGE_SELF,&usage)==0)peak_kib=static_cast<std::uint64_t>(usage.ru_maxrss);
#endif
    const auto& ss=snapshot->stats();
    std::cout<<std::setprecision(17);
    std::cout<<"{\"schema_version\":10,\"status\":\"PASS\",\"rows\":"<<snapshot->rows()<<",\"channels\":"<<channels
      <<",\"query\":"<<quote(kind_text)<<",\"threshold\":"<<threshold<<",\"first\":"<<first<<",\"count\":"<<count
      <<",\"selected_rows\":"<<expected.entries.size()<<",\"output_bytes\":"<<expected.bytes.size()
      <<",\"output_fnv64\":"<<quote(std::to_string(expected_checksum))<<",\"rounds\":"<<rounds<<",\"seed\":"<<seed
      <<",\"complete_output_checks\":"<<complete_checks<<",\"codec_version\":"<<quote(zstd_version())
      <<",\"sqlite_version\":"<<quote(sql.version())<<",\"sqlite_reported_bytes\":"<<sql.database_bytes()
      <<",\"full_payload_validation\":true,\"wordcode_nodes\":"<<query->wordcode().nodes.size()
      <<",\"wordcode_planes\":"<<query->wordcode().plane_count<<",\"constant_decision\":"<<static_cast<unsigned>(query->constant_decision())
      <<",\"zone_blocks\":[";
    for(std::size_t i=0;i<query->block_decisions().size();++i){if(i)std::cout<<',';std::cout<<static_cast<unsigned>(query->block_decisions()[i]);}
    std::cout<<"],\"preparation_ns\":{\"snapshot_admission\":"<<admission_ns<<",\"query\":"<<query_ns
      <<",\"all_workers\":"<<workers_ns<<",\"sqlite_load_prepare\":"<<sqlite_ns<<",\"result_cache_build\":"<<cache_ns
      <<",\"independent_plaintext_oracle\":"<<oracle_ns<<"},\"memory\":{\"snapshot_raw_features\":"<<ss.raw_feature_bytes
      <<",\"snapshot_packed_features\":"<<ss.packed_feature_bytes<<",\"snapshot_payload_store\":"<<ss.payload_store_bytes
      <<",\"snapshot_block_bounds\":"<<ss.block_range_bytes<<",\"word_worker_scratch\":"<<word.scratch_bytes()
      <<",\"zoned_worker_scratch\":"<<zoned.scratch_bytes()<<",\"raw_decoder_scratch\":"<<fused_decoder.scratch_bytes()
      <<",\"cache_storage\":"<<cache->storage_bytes()<<",\"output_storage\":"<<output.bytes.capacity()+output.entries.capacity()*sizeof(Entry)
      <<",\"oracle_output_storage\":"<<expected.bytes.capacity()+expected.entries.capacity()*sizeof(Entry)
      <<",\"harness_peak_rss_kib\":"<<peak_kib<<"},\"timing_order\":[";
    for(std::size_t i=0;i<orders.size();++i){if(i)std::cout<<',';std::cout<<'[';
        for(std::size_t j=0;j<orders[i].size();++j){if(j)std::cout<<',';std::cout<<orders[i][j];}std::cout<<']';}
    std::cout<<"],\"candidates\":[";
    for(std::size_t c=0;c<names.size();++c) {
        if(c)std::cout<<',';
        auto sorted=timings[c];std::sort(sorted.begin(),sorted.end());
        const double median=sorted.size()%2?double(sorted[sorted.size()/2]):(double(sorted[sorted.size()/2-1])+double(sorted[sorted.size()/2]))/2;
        const auto& work=counters[c];
        std::cout<<"{\"name\":"<<quote(names[c])<<",\"median_ns\":"<<median<<",\"samples_ns\":[";
        for(std::size_t i=0;i<timings[c].size();++i){if(i)std::cout<<',';std::cout<<timings[c][i];}
        std::cout<<"],\"selected_rows\":"<<work.selected<<",\"decoded_rows\":"<<work.decoded_rows
          <<",\"compressed_bytes\":"<<work.compressed<<",\"decoded_bytes\":"<<work.decoded
          <<",\"output_bytes\":"<<output.bytes.size()<<",\"zone_true_rows\":"<<work.zone_true
          <<",\"zone_false_rows\":"<<work.zone_false<<",\"zone_refined_rows\":"<<work.zone_refined<<'}';
    }
    std::cout<<"]}\n";return 0;
} catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
