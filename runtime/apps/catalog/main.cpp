#include "suffice/catalog_generation.hpp"
#include "digest.hpp"
#include "wire.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace cat=suffice::catalog;
namespace payload=suffice::payload;
namespace wire=suffice::catalog::wire;
using Clock=std::chrono::steady_clock;
namespace {
std::string quote(std::string_view value) {
    constexpr char hex[]="0123456789abcdef";std::string out="\"";
    for(const unsigned char c:value) {
        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
        else if(c<32||c>=127){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=char(c);
    }
    return out+'"';
}
std::uint64_t number(std::string_view text) {
    std::uint64_t result{};const auto p=std::from_chars(text.data(),text.data()+text.size(),result);
    if(text.empty()||text.front()<'0'||text.front()>'9'||p.ec!=std::errc{}||p.ptr!=text.data()+text.size())
        throw std::invalid_argument("invalid unsigned decimal protocol field");
    return result;
}
std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> result;std::size_t first=0;
    for(;;){if(result.size()==9)throw std::invalid_argument("too many protocol fields");
        auto end=line.find('\t',first);if(end==line.npos){result.push_back(line.substr(first));break;}
        result.push_back(line.substr(first,end-first));first=end+1;}
    return result;
}
std::string text(std::string_view encoded,std::uint64_t maximum,bool allow_empty=false) {
    if(encoded.size()>maximum*2)throw std::length_error("protocol field exceeds byte limit");
    auto decoded=cat::detail::unhex(encoded);
    if((decoded.empty()&&!allow_empty)||decoded.find('\0')!=decoded.npos||!cat::detail::utf8(decoded))
        throw std::invalid_argument("protocol text must be non-NUL UTF-8");
    return decoded;
}
std::filesystem::path path(std::string_view value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),value.size()));
}
double milliseconds(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
void stats_json(std::ostream& output,const cat::Stats& s) {
    output<<"{\"project_slots\":"<<s.project_slots<<",\"active_projects\":"<<s.active_projects
        <<",\"live_snapshots\":"<<s.live_snapshots<<",\"live_generations\":"<<s.live_generations
        <<",\"snapshot_bytes\":"<<s.snapshot_bytes<<",\"metadata_bytes\":"<<s.metadata_bytes
        <<",\"retired_snapshot_bytes\":"<<s.retired_snapshot_bytes<<",\"retired_metadata_bytes\":"<<s.retired_metadata_bytes
        <<",\"result_entries\":"<<s.result_entries<<",\"result_bytes\":"<<s.result_bytes
        <<",\"result_key_bytes\":"<<s.result_key_bytes<<",\"plan_entries\":"<<s.plan_entries
        <<",\"plan_text_bytes\":"<<s.plan_text_bytes<<",\"plan_worker_bytes\":"<<s.plan_worker_bytes
        <<",\"worker_entries\":"<<s.worker_entries<<",\"worker_preparations\":"<<s.worker_preparations
        <<",\"worker_reuses\":"<<s.worker_reuses<<",\"worker_evictions\":"<<s.worker_evictions
        <<",\"worker_invalidations\":"<<s.worker_invalidations<<",\"worker_oversized_runs\":"<<s.worker_oversized_runs
        <<",\"publications\":"<<s.publications
        <<",\"drops\":"<<s.drops<<",\"runs\":"<<s.runs<<",\"cache_hits\":"<<s.cache_hits
        <<",\"cache_misses\":"<<s.cache_misses<<",\"cache_admissions\":"<<s.cache_admissions
        <<",\"cache_evictions\":"<<s.cache_evictions<<",\"cache_invalidations\":"<<s.cache_invalidations
        <<",\"plan_preparations\":"<<s.plan_preparations<<",\"plan_evictions\":"<<s.plan_evictions
        <<",\"plan_invalidations\":"<<s.plan_invalidations<<'}';
}
struct Sink:payload::RowSink {
    const cat::Lease& lease;wire::ResponseWriter& output;std::uint64_t sequence;bool emit;
    std::uint64_t rows{},bytes{},fingerprint{14695981039346656037ULL};
    Sink(const cat::Lease& l,wire::ResponseWriter& o,std::uint64_t s,bool e):lease(l),output(o),sequence(s),emit(e){}
    void digest(std::uint8_t byte){fingerprint=(fingerprint^byte)*1099511628211ULL;}
    void scalar(std::uint64_t value){for(unsigned i=0;i<8;++i)digest(std::uint8_t(value>>(8*i)));}
    void row(std::uint64_t id,std::span<const std::uint8_t> body) override {
        if(id>=lease.artifacts().size())throw std::logic_error("catalog row outside metadata");
        const auto& artifact=lease.artifacts()[static_cast<std::size_t>(id)];
        if(body.size()!=artifact.size)throw std::logic_error("catalog output extent differs from artifact metadata");
        scalar(id);scalar(artifact.path.size());for(unsigned char c:artifact.path)digest(c);
        scalar(body.size());for(auto c:body)digest(c);
        if(emit)output.row(sequence,id,artifact.path,body);
        ++rows;bytes+=body.size();
    }
};
// Return 0=EOF,1=line,2=overlong,3=unterminated. Never allocate beyond cap.
int line(std::string& value) {
    constexpr std::size_t cap=256U<<10;value.clear();bool over=false;char c;
    while(std::cin.get(c)){if(c=='\n')return over?2:1;if(value.size()<cap)value+=c;else over=true;}
    return value.empty()&&!over?0:3;
}
void help() {
    std::cout<<R"(suffice_catalog 0.13.0 - persistent exact artifact catalog
Usage: suffice_catalog serve [--max-snapshot-bytes N] [--max-cache-bytes N]
       [--max-metadata-bytes N] [--max-projects N] [--max-cache-entries N]
       [--max-plans N] [--max-leases N]
       [--max-worker-bytes N] [--max-worker-entries N]
       [--population capture|recompute] [--transport jsonl|binary]
The bounded stdin protocol uses TAB fields and lowercase hex for text:
  OPEN <project_hex> <catalog_root_hex> <expected_revision>
  STATE <project_hex>
  PIN <alias_hex> <project_hex>
  UNPIN <alias_hex>
  RUN <project_hex> <program_hex> <first> <count|all> <bypass|use|populate> <hex|hash> <max_rows> <max_bytes>
  RUNPIN <alias_hex> <program_hex> <first> <count|all> <bypass|use|populate> <hex|hash> <max_rows> <max_bytes>
  DROP <project_hex> <expected_revision>
  STATS
  CLEAR <results|workers|plans|all>
  QUIT
Responses default to JSON lines; --transport binary uses bounded SFC1 frames.
Each request has zero or more complete row events followed by one result event.
Binary full rows use raw path/body bytes; the legacy output-mode name is hex.
The binary payload cap is 4 MiB. See docs/binary_protocol_v0_13.md.
Errors identify a delivered prefix. Hash mode consumes full output into an
ordered diagnostic FNV hash; it does not return file contents. Publication is
in-memory after full generation admission. Disk capture is a separate command:
  python scripts/artifact_catalog.py --help
Use scripts/catalog_client.py for the Python API and JSONL job interface.
)";
}
} // namespace

int main(int argc,char** argv) {
    std::ios::sync_with_stdio(false);std::cout<<std::setprecision(12);
    if(argc==2&&(std::string_view(argv[1])=="--help"||std::string_view(argv[1])=="help")){help();return 0;}
    if(argc==2&&std::string_view(argv[1])=="--version"){std::cout<<"suffice_catalog 0.13.0\n";return 0;}
    try {
        if(argc<2||std::string_view(argv[1])!="serve")throw std::invalid_argument("expected serve; use --help");
        cat::Limits limits;std::uint64_t max_leases=64;std::map<std::string,bool> supplied;
        cat::PopulateStrategy population=cat::PopulateStrategy::Capture;
        wire::Transport transport=wire::Transport::Jsonl;
        for(int i=2;i<argc;++i) {
            const std::string name=argv[i];
            if(supplied.contains(name)||i+1==argc)throw std::invalid_argument("duplicate or valueless option");
            supplied[name]=true;const std::string_view argument=argv[++i];
            if(name=="--transport") {
                if(argument=="jsonl")transport=wire::Transport::Jsonl;
                else if(argument=="binary")transport=wire::Transport::Binary;
                else throw std::invalid_argument("transport must be jsonl or binary");
                continue;
            }
            if(name=="--population") {
                if(argument=="capture")population=cat::PopulateStrategy::Capture;
                else if(argument=="recompute")population=cat::PopulateStrategy::Recompute;
                else throw std::invalid_argument("population must be capture or recompute");
                continue;
            }
            const auto value=number(argument);
            if(name=="--max-snapshot-bytes")limits.max_snapshot_bytes=value;
            else if(name=="--max-cache-bytes")limits.max_result_bytes=value;
            else if(name=="--max-metadata-bytes")limits.max_metadata_bytes=value;
            else if(name=="--max-worker-bytes")limits.max_worker_bytes=value;
            else if(name=="--max-leases")max_leases=value;
            else if(name=="--max-projects"||name=="--max-cache-entries"||name=="--max-plans"||name=="--max-worker-entries") {
                if(value>UINT32_MAX)throw std::invalid_argument("count option exceeds uint32");
                if(name=="--max-projects")limits.max_project_slots=std::uint32_t(value);
                else if(name=="--max-cache-entries")limits.max_result_entries=std::uint32_t(value);
                else if(name=="--max-worker-entries")limits.max_worker_entries=std::uint32_t(value);
                else limits.max_plan_entries=std::uint32_t(value);
            } else throw std::invalid_argument("unknown option "+name);
        }
        if(max_leases>65536)throw std::invalid_argument("max-leases exceeds implementation limit");
#ifdef _WIN32
        // Both serve modes implement a byte protocol with exact LF. Set the CRT
        // descriptors before any server I/O; binary bodies may contain CR/LF/EOF.
        if(_setmode(_fileno(stdin),_O_BINARY)==-1||_setmode(_fileno(stdout),_O_BINARY)==-1)
            throw std::runtime_error("cannot put catalog stdin/stdout in binary mode");
#endif
        wire::ResponseWriter output(std::cout,transport);
        cat::Catalog catalog(limits);std::map<std::string,cat::Lease> pins;
        std::string input;std::uint64_t sequence=0;
        for(;;) {
            const int input_state=line(input);if(!input_state)break;
            if(sequence==UINT64_MAX)throw std::overflow_error("catalog request sequence exhausted");
            ++sequence;const auto started=Clock::now();std::uint64_t emitted_rows=0,emitted_bytes=0;
            std::ostringstream response;response.exceptions(std::ios::badbit|std::ios::failbit);
            response<<std::setprecision(12);bool quit=false;
            try {
                if(input_state!=1)throw std::invalid_argument(input_state==2?"protocol line exceeds 256 KiB":"unterminated protocol line");
                if(input.find('\r')!=input.npos||input.find('\0')!=input.npos)throw std::invalid_argument("protocol line contains CR/NUL");
                const auto f=fields(input);const auto op=f[0];
                const auto require=[&](std::size_t n){if(f.size()!=n)throw std::invalid_argument("wrong protocol field count");};
                if(op=="OPEN") {
                    require(4);auto project=text(f[1],limits.max_label_bytes);auto root=text(f[2],32768);
                    const auto expected=number(f[3]);
                    if(catalog.state(project).revision!=expected)throw std::invalid_argument("stale expected revision before admission");
                    const auto retained=catalog.stats();cat::GenerationLimits admission;
                    admission.snapshot.max_snapshot_bytes=limits.max_snapshot_bytes-retained.snapshot_bytes;
                    admission.max_metadata_bytes=std::min(admission.max_metadata_bytes,limits.max_metadata_bytes-retained.metadata_bytes);
                    const auto start_load=Clock::now();auto loaded=cat::load_generation(path(root),admission);const auto load_ms=milliseconds(start_load);
                    const auto component_bytes=loaded.component_bytes,verified_bytes=loaded.verified_body_bytes;
                    const auto manifest=loaded.manifest_sha256;
                    auto lease=catalog.publish(std::move(project),expected,loaded.generation,std::move(loaded.snapshot),std::move(loaded.artifacts));
                    response<<"{\"type\":\"result\",\"seq\":"<<sequence<<",\"status\":\"ok\",\"op\":\"OPEN\",\"revision\":"<<lease.revision()
                        <<",\"generation\":"<<quote(lease.generation())<<",\"rows\":"<<lease.rows()<<",\"load_ms\":"<<load_ms
                        <<",\"component_bytes\":"<<component_bytes<<",\"verified_body_bytes\":"<<verified_bytes
                        <<",\"manifest_sha256\":"<<quote(manifest)<<",\"wall_ms\":"<<milliseconds(started)<<"}";
                } else if(op=="STATE") {
                    require(2);const auto state=catalog.state(text(f[1],limits.max_label_bytes));
                    response<<"{\"type\":\"result\",\"seq\":"<<sequence<<",\"status\":\"ok\",\"op\":\"STATE\",\"loaded\":"<<(state.loaded?"true":"false")
                        <<",\"revision\":"<<state.revision<<",\"generation\":"<<quote(state.generation)<<",\"rows\":"<<state.rows<<"}";
                } else if(op=="PIN") {
                    require(3);auto alias=text(f[1],limits.max_label_bytes);
                    if(pins.contains(alias))throw std::invalid_argument("lease alias already exists");
                    if(pins.size()>=max_leases)throw std::length_error("lease slot budget exceeded");
                    auto lease=catalog.acquire(text(f[2],limits.max_label_bytes));const auto revision=lease.revision();
                    pins.emplace(std::move(alias),std::move(lease));
                    response<<"{\"type\":\"result\",\"seq\":"<<sequence<<",\"status\":\"ok\",\"op\":\"PIN\",\"revision\":"<<revision<<"}";
                } else if(op=="UNPIN") {
                    require(2);if(!pins.erase(text(f[1],limits.max_label_bytes)))throw std::invalid_argument("unknown lease alias");
                    catalog.collect();response<<"{\"type\":\"result\",\"seq\":"<<sequence<<",\"status\":\"ok\",\"op\":\"UNPIN\"}";
                } else if(op=="RUN"||op=="RUNPIN") {
                    require(9);auto name=text(f[1],limits.max_label_bytes);cat::Lease lease;
                    if(op=="RUN")lease=catalog.acquire(name);
                    else {const auto found=pins.find(name);if(found==pins.end())throw std::invalid_argument("unknown lease alias");lease=found->second;}
                    const auto program=text(f[2],limits.max_program_bytes);const auto first=number(f[3]);
                    if(first>lease.rows())throw std::out_of_range("first row exceeds generation");
                    const auto count=f[4]=="all"?lease.rows()-first:number(f[4]);
                    cat::RunOptions options;options.population=population;
                    if(f[5]=="bypass")options.cache=cat::CacheMode::Bypass;
                    else if(f[5]=="use")options.cache=cat::CacheMode::Use;
                    else if(f[5]=="populate")options.cache=cat::CacheMode::Populate;
                    else throw std::invalid_argument("unknown cache mode");
                    if(f[6]!="hex"&&f[6]!="hash")throw std::invalid_argument("unknown output mode");
                    options.output={number(f[7]),number(f[8])};Sink sink(lease,output,sequence,f[6]=="hex");
                    cat::RunReport report;
                    try{report=catalog.run(lease,program,first,count,sink,options);}
                    catch(...){emitted_rows=sink.rows;emitted_bytes=sink.bytes;throw;}
                    emitted_rows=sink.rows;emitted_bytes=sink.bytes;
                    const auto& p=report.pipeline;
                    response<<"{\"type\":\"result\",\"seq\":"<<sequence<<",\"status\":\"ok\",\"op\":"<<quote(op)
                        <<",\"generation\":"<<quote(lease.generation())<<",\"revision\":"<<lease.revision()
                        <<",\"selected_rows\":"<<sink.rows<<",\"output_bytes\":"<<sink.bytes<<",\"output_fnv64\":"<<quote(std::to_string(sink.fingerprint))
                        <<",\"plan_reused\":"<<(report.plan_reused?"true":"false")<<",\"cache_hit\":"<<(report.cache_hit?"true":"false")
                        <<",\"worker_reused\":"<<(report.worker_reused?"true":"false")
                        <<",\"worker_retained\":"<<(report.worker_retained?"true":"false")
                        <<",\"worker_oversized\":"<<(report.worker_oversized?"true":"false")
                        <<",\"population_strategy\":"<<quote(population==cat::PopulateStrategy::Capture?"capture":"recompute")
                        <<",\"cache_stored\":"<<(report.cache_stored?"true":"false")<<",\"cache_oversized\":"<<(report.cache_oversized?"true":"false")
                        <<",\"retired_generation\":"<<(report.retired_generation?"true":"false")
                        <<",\"cache_build_failed\":"<<(report.cache_build_failed?"true":"false")<<",\"cache_storage_bytes\":"<<report.cache_storage_bytes
                        <<",\"worker_scratch_bytes\":"<<report.worker_scratch_bytes<<",\"plan_ms\":"<<report.plan_ms
                        <<",\"execution_ms\":"<<report.execution_ms<<",\"cache_build_ms\":"<<report.cache_build_ms
                        <<",\"decoded_payload_bytes\":"<<p.decoded_payload_bytes<<",\"compressed_payload_bytes\":"<<p.compressed_payload_bytes
                        <<",\"zone_true_rows\":"<<p.zone_true_rows<<",\"zone_false_rows\":"<<p.zone_false_rows<<",\"zone_refined_rows\":"<<p.zone_refined_rows
                        <<",\"wall_ms\":"<<milliseconds(started)<<"}";
                } else if(op=="DROP") {
                    require(3);catalog.drop(text(f[1],limits.max_label_bytes),number(f[2]));
                    response<<"{\"type\":\"result\",\"seq\":"<<sequence<<",\"status\":\"ok\",\"op\":\"DROP\"}";
                } else if(op=="STATS") {
                    require(1);response<<"{\"type\":\"result\",\"seq\":"<<sequence<<",\"status\":\"ok\",\"op\":\"STATS\",\"stats\":";
                    stats_json(response,catalog.stats());response<<",\"pinned_aliases\":"<<pins.size()<<"}";
                } else if(op=="CLEAR") {
                    require(2);if(f[1]!="results"&&f[1]!="workers"&&f[1]!="plans"&&f[1]!="all")throw std::invalid_argument("unknown CLEAR target");
                    if(f[1]=="results"||f[1]=="all")catalog.clear_results();
                    if(f[1]=="workers")catalog.clear_workers();
                    if(f[1]=="plans"||f[1]=="all")catalog.clear_plans();
                    catalog.collect();
                    response<<"{\"type\":\"result\",\"seq\":"<<sequence<<",\"status\":\"ok\",\"op\":\"CLEAR\"}";
                } else if(op=="QUIT") {
                    require(1);response<<"{\"type\":\"result\",\"seq\":"<<sequence<<",\"status\":\"ok\",\"op\":\"QUIT\"}";quit=true;
                } else throw std::invalid_argument("unknown protocol operation");
                output.result(response.view());
            } catch(const std::exception& error) {
                if(output.poisoned())return 2;
                quit=false;response.str(std::string{});response.clear();response<<std::setprecision(12);
                response<<"{\"type\":\"result\",\"seq\":"<<sequence<<",\"status\":\"error\",\"error\":"<<quote(error.what())
                    <<",\"delivered_rows\":"<<emitted_rows<<",\"delivered_bytes\":"<<emitted_bytes<<",\"wall_ms\":"<<milliseconds(started)<<"}";
                output.result(response.view());
            }
            output.flush();if(quit)break;
        }
        return 0;
    } catch(const wire::OutputFailure&){return 2;}
    catch(const std::exception& error){std::cerr<<"suffice_catalog: "<<error.what()<<'\n';return 1;}
}
