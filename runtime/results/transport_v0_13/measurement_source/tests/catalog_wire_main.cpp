#include "../apps/catalog/wire.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

namespace wire=suffice::catalog::wire;
namespace {
std::uint64_t groups{},checks{},rejections{},poison_cases{};
void check(bool condition,const char* message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
template<class Exception,class Function> void rejects(Function function) {
    try { function(); }
    catch(const Exception&) { ++rejections;return; }
    throw std::runtime_error("expected exception was not thrown");
}
template<class Function> void group(Function function) { function();++groups; }
std::string from_hex(std::string_view value) {
    std::string result;
    const auto digit=[](char c)->unsigned{return c<='9'?unsigned(c-'0'):unsigned(c-'a'+10);};
    for(std::size_t i=0;i<value.size();i+=2)result+=char(digit(value[i])*16+digit(value[i+1]));
    return result;
}
std::uint64_t little_at(std::string_view value,std::size_t first) {
    std::uint64_t result{};
    for(unsigned i=0;i<8;++i)result|=std::uint64_t(static_cast<unsigned char>(value.at(first+i)))<<(8*i);
    return result;
}
struct LimitedBuffer final:std::streambuf {
    std::string data;
    std::size_t limit;
    bool fail_flush{},throw_write{};
    std::uint64_t calls{};
    explicit LimitedBuffer(std::size_t maximum):limit(maximum){}
    std::streamsize xsputn(const char* source,std::streamsize count) override {
        ++calls;
        if(throw_write)throw std::runtime_error("injected stream-buffer write failure");
        const auto accepted=std::min<std::size_t>(static_cast<std::size_t>(count),limit-data.size());
        data.append(source,accepted);return static_cast<std::streamsize>(accepted);
    }
    int_type overflow(int_type value) override {
        if(traits_type::eq_int_type(value,traits_type::eof()))return traits_type::not_eof(value);
        const auto c=traits_type::to_char_type(value);
        return xsputn(&c,1)==1?value:traits_type::eof();
    }
    int sync() override { ++calls;return fail_flush?-1:0; }
};
void verify_poison(wire::ResponseWriter& writer,std::ostream& output,LimitedBuffer& buffer) {
    check(writer.poisoned(),"failed output must poison writer");
    const auto old_size=buffer.data.size(),old_calls=buffer.calls;
    output.clear();buffer.fail_flush=false;buffer.throw_write=false;
    rejects<wire::OutputFailure>([&]{writer.result("{}");});
    rejects<wire::OutputFailure>([&]{writer.flush();});
    check(buffer.data.size()==old_size&&buffer.calls==old_calls,"poisoned writer attempted more output");
    ++poison_cases;
}
} // namespace

int main() {
    try {
        group([]{
            std::ostringstream output;wire::ResponseWriter writer(output,wire::Transport::Binary);
            const std::array<std::uint8_t,5> body{0,10,13,26,255};
            writer.row(0x0807060504030201ULL,0x1817161514131211ULL,"x",body);
            const auto expected=from_hex(
                "53464331" "02000000" "2600000000000000"
                "0102030405060708" "1112131415161718"
                "0100000000000000" "0500000000000000" "78" "000a0d1aff");
            check(output.str()==expected,"golden binary row bytes differ");
            output.str(std::string{});writer.result("{}");writer.flush();
            check(output.str()==from_hex("534643310100000002000000000000007b7d"),"golden terminal header or body differs");
        });
        group([]{
            std::ostringstream output;wire::ResponseWriter writer(output,wire::Transport::Jsonl);
            const std::array<std::uint8_t,5> body{0,10,13,26,255};
            writer.row(1,2,"x",body);writer.result("{}");writer.flush();
            check(output.str()=="{\"type\":\"row\",\"seq\":1,\"row\":2,\"path_hex\":\"78\",\"data_hex\":\"000a0d1aff\"}\n{}\n",
                  "legacy JSON row or terminal bytes differ");
            std::ostringstream maximum;wire::ResponseWriter integers(maximum,wire::Transport::Jsonl);
            integers.row(UINT64_MAX,UINT64_MAX,"x",{});
            check(maximum.str()=="{\"type\":\"row\",\"seq\":18446744073709551615,\"row\":18446744073709551615,\"path_hex\":\"78\",\"data_hex\":\"\"}\n",
                  "uint64 decimal boundary differs");
        });
        group([]{
            std::ostringstream output;wire::ResponseWriter writer(output,wire::Transport::Binary);
            const std::string path="\xc3\xa9.txt";
            writer.row(1,9,path,{});
            const auto bytes=output.str();
            check(bytes.size()==16+32+path.size(),"empty body frame extent differs");
            check(little_at(bytes,8)==32+path.size()&&little_at(bytes,32)==path.size()&&little_at(bytes,40)==0,
                  "empty body lengths differ");
            check(bytes.substr(48)==path,"UTF-8 path was translated");
        });
        group([]{
            const auto exact=wire::max_payload_bytes-wire::row_prefix_bytes-1;
            std::vector<std::uint8_t> body(exact+1,0xa5);
            std::ostringstream output;wire::ResponseWriter writer(output,wire::Transport::Binary);
            writer.row(1,2,"x",std::span(body).first(exact));
            const auto bytes=output.str();
            check(bytes.size()==wire::frame_header_bytes+wire::max_payload_bytes,"exact binary payload cap failed");
            check(little_at(bytes,8)==wire::max_payload_bytes&&little_at(bytes,40)==exact,"exact binary cap header differs");
            check(std::all_of(bytes.begin()+49,bytes.end(),[](unsigned char c){return c==0xa5;}),"bounded body copy differs");
            std::ostringstream rejected;wire::ResponseWriter recovery(rejected,wire::Transport::Binary);
            rejects<std::length_error>([&]{recovery.row(1,2,"x",body);});
            check(rejected.str().empty()&&!recovery.poisoned(),"one-over binary cap emitted bytes or poisoned output");
            recovery.result("{}");check(rejected.str().size()==18,"preflight error prevented later terminal");
        });
        group([]{
            const std::string empty="{\"type\":\"row\",\"seq\":1,\"row\":1,\"path_hex\":\"61\",\"data_hex\":\"\"}\n";
            const auto exact=(wire::max_json_line_bytes-empty.size())/2;
            std::vector<std::uint8_t> body(exact+1,0xab);
            std::ostringstream output;wire::ResponseWriter writer(output,wire::Transport::Jsonl);
            writer.row(1,1,"a",std::span(body).first(exact));
            check(output.str().size()==empty.size()+2*exact,"maximum fitting JSON row extent differs");
            check(output.str().size()<=wire::max_json_line_bytes,"JSON cap was exceeded");
            std::ostringstream rejected;wire::ResponseWriter recovery(rejected,wire::Transport::Jsonl);
            rejects<std::length_error>([&]{recovery.row(1,1,"a",body);});
            check(rejected.str().empty()&&!recovery.poisoned(),"one-over JSON capacity wrote a partial row");
        });
        group([]{
            for(const auto transport:{wire::Transport::Jsonl,wire::Transport::Binary}) {
                const auto maximum=transport==wire::Transport::Binary?wire::max_payload_bytes:wire::max_json_line_bytes-1;
                std::string payload="{\"pad\":\""+std::string(maximum-10,'a')+"\"}";
                check(payload.size()==maximum,"terminal fixture size differs");
                std::ostringstream output;wire::ResponseWriter writer(output,transport);writer.result(payload);
                check(output.str().size()==maximum+(transport==wire::Transport::Binary?16:1),"exact terminal cap differs");
                payload.insert(payload.size()-2,1,'a');
                std::ostringstream rejected;wire::ResponseWriter recovery(rejected,transport);
                rejects<std::length_error>([&]{recovery.result(payload);});
                rejects<std::length_error>([&]{recovery.result("");});
                rejects<std::length_error>([&]{recovery.result("{}\n");});
                rejects<std::length_error>([&]{recovery.result("{}\r");});
                check(rejected.str().empty()&&!recovery.poisoned(),"invalid terminal preflight emitted bytes");
            }
        });
        group([]{
            for(const auto transport:{wire::Transport::Jsonl,wire::Transport::Binary}) {
                std::ostringstream output;wire::ResponseWriter writer(output,transport);
                const auto bad_paths=std::array<std::string,5>{"",std::string(wire::max_path_bytes+1,'a'),
                    std::string("a\0b",3),std::string("\xed\xa0\x80",3),std::string("\xc0\xaf",2)};
                for(const auto& path:bad_paths)rejects<std::invalid_argument>([&]{writer.row(1,2,path,{});});
                check(output.str().empty()&&!writer.poisoned(),"invalid path produced output");
                writer.row(1,2,std::string(wire::max_path_bytes,'a'),{});
                check(!output.str().empty(),"exact path cap failed");
            }
            std::ostringstream output;
            rejects<std::invalid_argument>([&]{wire::ResponseWriter invalid(output,static_cast<wire::Transport>(-1));});
        });
        group([]{
            const std::array<std::uint8_t,5> body{0,10,13,26,255};
            for(const auto transport:{wire::Transport::Jsonl,wire::Transport::Binary}) {
                for(const bool exceptions:{false,true}) {
                    for(const auto maximum:{0U,5U,16U,20U,48U,49U,52U}) {
                        LimitedBuffer buffer(maximum);std::ostream output(&buffer);
                        if(exceptions)output.exceptions(std::ios::badbit|std::ios::failbit);
                        wire::ResponseWriter writer(output,transport);
                        rejects<wire::OutputFailure>([&]{writer.row(1,2,"x",body);});
                        check(buffer.data.size()==maximum,"short-write fixture did not reach expected boundary");
                        verify_poison(writer,output,buffer);
                    }
                }
            }
        });
        group([]{
            for(const auto transport:{wire::Transport::Jsonl,wire::Transport::Binary}) {
                for(const bool exceptions:{false,true}) {
                    LimitedBuffer buffer(128);buffer.fail_flush=true;std::ostream output(&buffer);
                    if(exceptions)output.exceptions(std::ios::badbit|std::ios::failbit);
                    wire::ResponseWriter writer(output,transport);writer.result("{}");
                    rejects<wire::OutputFailure>([&]{writer.flush();});
                    verify_poison(writer,output,buffer);
                    LimitedBuffer throwing(128);throwing.throw_write=true;std::ostream failed(&throwing);
                    if(exceptions)failed.exceptions(std::ios::badbit|std::ios::failbit);
                    wire::ResponseWriter failure(failed,transport);
                    rejects<wire::OutputFailure>([&]{failure.result("{}");});
                    verify_poison(failure,failed,throwing);
                }
            }
        });
        std::cout<<"{\"status\":\"passed\",\"groups\":"<<groups<<",\"checks\":"<<checks
                 <<",\"expected_rejections\":"<<rejections<<",\"poison_cases\":"<<poison_cases<<"}\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"catalog wire test: "<<error.what()<<'\n';return 1; }
}
