#include "suffice/catalog_generation.hpp"
#include "digest.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace suffice::catalog {
namespace {
namespace fs=std::filesystem;
using Bytes=std::vector<std::uint8_t>;
std::uint64_t integer(std::string_view text) {
    std::uint64_t result{};
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
    if(text.empty()||(text.size()>1&&text.front()=='0')||text.front()<'0'||text.front()>'9'
       ||parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())
        throw std::invalid_argument("noncanonical unsigned integer in generation");
    return result;
}
std::uint64_t add(std::uint64_t a,std::uint64_t b) {
    if(b>UINT64_MAX-a)throw std::overflow_error("generation size overflow");
    return a+b;
}
void regular(const fs::path& path,bool directory=false) {
    const auto s=fs::symlink_status(path);
    if(fs::is_symlink(s)||(directory?!fs::is_directory(s):!fs::is_regular_file(s)))
        throw std::invalid_argument("generation component is missing, linked, or wrong file type");
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::invalid_argument("generation component uses a reparse point");
#endif
}
Bytes read(const fs::path& path,std::uint64_t limit,std::uint64_t exact=UINT64_MAX) {
    regular(path);
    const auto size=fs::file_size(path);
    if(size>limit||size>std::uint64_t(std::numeric_limits<std::streamsize>::max())
       ||size>std::numeric_limits<std::size_t>::max()||(exact!=UINT64_MAX&&size!=exact))
        throw std::length_error("generation component exceeds limit or differs from manifest size");
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::runtime_error("cannot open generation component");
    Bytes result(static_cast<std::size_t>(size));
    if(size&&!input.read(reinterpret_cast<char*>(result.data()),static_cast<std::streamsize>(size)))
        throw std::runtime_error("short generation component read");
    char extra{};
    if(input.get(extra)||!input.eof())throw std::runtime_error("generation component grew or read failed");
    return result;
}
std::string_view view(const Bytes& value) {
    return std::string_view(reinterpret_cast<const char*>(value.data()),value.size());
}
std::vector<std::string_view> split(std::string_view text,char delimiter) {
    std::vector<std::string_view> result;
    std::size_t first=0;
    for(;;){const auto end=text.find(delimiter,first);if(end==text.npos){result.push_back(text.substr(first));break;}
        result.push_back(text.substr(first,end-first));first=end+1;}
    return result;
}
std::vector<std::string_view> lines(std::string_view text) {
    if(text.empty()||text.back()!='\n'||text.find('\r')!=text.npos||text.find('\0')!=text.npos)
        throw std::invalid_argument("generation text needs LF termination and no CR/NUL");
    text.remove_suffix(1);return split(text,'\n');
}
template<std::size_t N>
std::array<std::string_view,N> fixed_fields(std::string_view text,char delimiter) {
    std::array<std::string_view,N> result;
    for(std::size_t i=0;i<N;++i) {
        const auto end=text.find(delimiter);
        if(i+1==N) {
            if(end!=text.npos)throw std::invalid_argument("extra metadata field");
            result[i]=text;
        } else {
            if(end==text.npos)throw std::invalid_argument("missing metadata field");
            result[i]=text.substr(0,end);text.remove_prefix(end+1);
        }
    }
    return result;
}
void validate_path(std::string_view path,std::uint64_t max_bytes) {
    if(path.empty()||path.size()>max_bytes||!detail::utf8(path)||path.find('\0')!=path.npos
       ||path.find('\\')!=path.npos||path.find(':')!=path.npos)
        throw std::invalid_argument("invalid artifact path encoding or extent");
    for(auto part:split(path,'/'))if(part.empty()||part=="."||part=="..")
        throw std::invalid_argument("artifact paths must be canonical relative paths");
}
bool byte_less(std::string_view a,std::string_view b) {
    return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),
        [](char x,char y){return static_cast<unsigned char>(x)<static_cast<unsigned char>(y);});
}
std::int32_t extension_class(std::string_view path) {
    const auto slash=path.find_last_of('/');const auto base=slash==path.npos?0:slash+1;
    const auto dot=path.find_last_of('.');if(dot==path.npos||dot<=base)return 0;
    std::string ext(path.substr(dot));for(auto& c:ext)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
    constexpr std::array<std::string_view,8> groups{
        ".rs", ".c .h .cc .hh .cpp .hpp .cxx .hxx .cu .cuh",
        ".js .jsx .ts .tsx .mjs .cjs", ".py .pyi",
        ".json .jsonc .toml .yaml .yml .ini .cfg .xml",
        ".md .markdown .rst", ".glsl .wgsl .vert .frag .hlsl .metal", ".log .txt"};
    for(std::size_t group=0;group<groups.size();++group)
        for(auto suffix:split(groups[group],' '))if(suffix==ext)return std::int32_t(group+1);
    return 0;
}
struct Component {std::string name;std::uint64_t size{};std::string digest;};
} // namespace

LoadedGeneration load_generation(const fs::path& root,GenerationLimits limits) {
    regular(root,true);
    const auto pointer=read(root/"CURRENT",33);
    if(pointer.size()!=33||pointer.back()!='\n')throw std::invalid_argument("invalid CURRENT generation pointer");
    return load_generation_id(root,std::string(view(pointer).substr(0,32)),limits);
}

LoadedGeneration load_generation_id(const fs::path& root,const std::string& generation,
                                    GenerationLimits limits) {
    if(!detail::lower_hex(generation,32))throw std::invalid_argument("invalid generation id");
    regular(root,true);regular(root/"generations",true);
    const auto directory=root/"generations"/generation;regular(directory,true);
    const auto manifest=read(directory/"manifest.txt",4096);
    const auto records=lines(view(manifest));
    if(records.size()!=9||records[0]!="SUFCAT01")throw std::invalid_argument("invalid generation manifest grammar");
    const auto field=[&](std::size_t i,std::string_view expected) {
        const auto pair=split(records[i],'\t');
        if(pair.size()!=2||pair[0]!=expected)throw std::invalid_argument("invalid generation manifest field/order");
        return pair[1];
    };
    if(field(1,"generation")!=generation)throw std::invalid_argument("manifest generation differs from directory");
    const auto count=integer(field(2,"rows"));
    if(count>limits.snapshot.max_rows||count>std::numeric_limits<std::size_t>::max())
        throw std::length_error("generation row limit exceeded");
    if(field(3,"channels")!="4"||field(4,"schema")!="1")throw std::invalid_argument("unsupported artifact feature schema");
    constexpr std::array<std::string_view,4> names{"features.i32","features.sfp","payload.spp","rows.tsv"};
    std::array<Component,4> components{};
    std::uint64_t total=0;
    for(std::size_t i=0;i<components.size();++i) {
        const auto cells=split(records[5+i],'\t');
        if(cells.size()!=4||cells[0]!="component"||cells[1]!=names[i]||!detail::lower_hex(cells[3],64))
            throw std::invalid_argument("invalid generation component descriptor");
        components[i]={std::string(cells[1]),integer(cells[2]),std::string(cells[3])};
        total=add(total,components[i].size);
    }
    if(total>limits.max_component_bytes||components[3].size>limits.max_metadata_bytes)
        throw std::length_error("generation component/metadata budget exceeded");
    if(total-components[3].size>limits.snapshot.max_snapshot_bytes)
        throw std::length_error("generation snapshot inputs exceed available retained headroom");
    if(count>UINT64_MAX/16||components[0].size!=count*16)
        throw std::invalid_argument("raw feature extent differs from row count");
    std::array<Bytes,4> data;
    for(std::size_t i=0;i<components.size();++i) {
        data[i]=read(directory/components[i].name,limits.max_component_bytes,components[i].size);
        if(detail::sha256(data[i])!=components[i].digest)throw std::invalid_argument("generation component SHA-256 mismatch: "+components[i].name);
    }
    LoadedGeneration result;
    result.generation=generation;result.manifest_sha256=detail::sha256(manifest);result.component_bytes=total;
    // Even the shortest canonical row needs 72 bytes. Reject impossible counts
    // before allocating the output vector from an untrusted manifest count.
    if(count>data[3].size()/72)throw std::invalid_argument("metadata too short for declared row count");
    result.artifacts.reserve(static_cast<std::size_t>(count));
    if(count==0) {if(!data[3].empty())throw std::invalid_argument("nonempty metadata for empty generation");}
    else {
        auto remaining=view(data[3]);
        for(std::uint64_t i=0;i<count;++i) {
            const auto end=remaining.find('\n');
            if(end==remaining.npos)throw std::invalid_argument("missing metadata row or LF terminator");
            const auto row_text=remaining.substr(0,end);remaining.remove_prefix(end+1);
            const auto cells=fixed_fields<4>(row_text,'\t');
            if(integer(cells[0])!=i||!detail::lower_hex(cells[3],64))
                throw std::invalid_argument("invalid artifact metadata row grammar");
            if(cells[1].size()%2||cells[1].size()/2>limits.max_path_bytes)
                throw std::length_error("artifact path exceeds encoded byte cap");
            auto path=detail::unhex(cells[1]);validate_path(path,limits.max_path_bytes);
            if(i&&!byte_less(result.artifacts.back().path,path))throw std::invalid_argument("artifact paths are not strictly byte-sorted");
            const auto size=integer(cells[2]);
            if(size>limits.snapshot.max_row_bytes||size>INT32_MAX)throw std::length_error("artifact decoded row limit exceeded");
            result.artifacts.push_back({std::move(path),std::string(cells[3]),size});
        }
        if(!remaining.empty())throw std::invalid_argument("extra artifact metadata rows");
    }
    result.snapshot=payload::Snapshot::copy_bytes(data[0],4,data[1],data[2],limits.snapshot,payload::Validation::Full);
    if(result.snapshot->rows()!=count)throw std::invalid_argument("admitted row count differs from manifest");
    // Re-derive the extractor contract independently from the producer. This
    // additional full pass is deliberately charged to admission, not queries.
    payload::Decoder decoder(result.snapshot);
    for(std::size_t row=0;row<result.artifacts.size();++row) {
        const auto& artifact=result.artifacts[row];const auto body=decoder.decode(row);
        if(body.size()!=artifact.size||detail::sha256(body)!=artifact.sha256)
            throw std::invalid_argument("artifact metadata/body identity mismatch");
        const auto lf=std::count(body.begin(),body.end(),std::uint8_t('\n'));
        const bool text=detail::utf8(body)&&std::find(body.begin(),body.end(),std::uint8_t(0))==body.end();
        const std::array<std::int32_t,4> expected{std::int32_t(body.size()),extension_class(artifact.path),std::int32_t(lf),text?1:0};
        for(std::uint32_t channel=0;channel<4;++channel)
            if(result.snapshot->raw_features().value(row,channel)!=expected[channel])
                throw std::invalid_argument("artifact feature semantics mismatch");
        result.verified_body_bytes=add(result.verified_body_bytes,body.size());
    }
    return result;
}
} // namespace suffice::catalog
