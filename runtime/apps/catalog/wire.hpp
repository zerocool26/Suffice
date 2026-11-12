#pragma once

#include "digest.hpp"
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string_view>

namespace suffice::catalog::wire {

enum class Transport { Jsonl, Binary };
inline constexpr std::size_t max_payload_bytes = 4U << 20;
inline constexpr std::size_t max_json_line_bytes = 4U << 20;
inline constexpr std::size_t max_path_bytes = 32768;
inline constexpr std::size_t frame_header_bytes = 16;
inline constexpr std::size_t row_prefix_bytes = 32;

class OutputFailure final : public std::runtime_error {
public:
    OutputFailure() : std::runtime_error("catalog output stream failed") {}
};

// The caller formats terminal JSON and supplies admitted, canonical row paths.
// This writer checks wire extents before output, emits no body-sized temporary,
// and cannot resume after any output/flush failure, even if the stream is cleared.
class ResponseWriter {
public:
    ResponseWriter(std::ostream& output, Transport transport) : output_(output), transport_(transport) {
        if(transport!=Transport::Jsonl&&transport!=Transport::Binary)
            throw std::invalid_argument("invalid catalog transport");
    }
    [[nodiscard]] bool poisoned() const noexcept { return poisoned_; }

    void row(std::uint64_t sequence,std::uint64_t id,std::string_view path,
             std::span<const std::uint8_t> body) {
        ready();
        if(path.empty()||path.size()>max_path_bytes||path.find('\0')!=path.npos||!detail::utf8(path))
            throw std::invalid_argument("catalog output path is not bounded non-NUL UTF-8");
        if(transport_==Transport::Binary) {
            // Bound each component before adding, so large size_t spans cannot wrap.
            if(path.size()>max_payload_bytes-row_prefix_bytes||
               body.size()>max_payload_bytes-row_prefix_bytes-path.size())
                throw std::length_error("catalog binary row exceeds frame payload limit");
            const auto length=std::uint64_t(row_prefix_bytes+path.size()+body.size());
            std::array<std::uint8_t,row_prefix_bytes> prefix{};
            little(prefix.data(),sequence);little(prefix.data()+8,id);
            little(prefix.data()+16,path.size());little(prefix.data()+24,body.size());
            emit([&]{header(2,length);raw(prefix);raw(path);raw(body);});
        } else {
            const auto fixed=row_open.size()+after_sequence.size()+after_id.size()+after_path.size()+row_close.size()
                +decimal_size(sequence)+decimal_size(id);
            const auto room=(max_json_line_bytes-fixed)/2;
            if(path.size()>room||body.size()>room-path.size())
                throw std::length_error("catalog JSON row exceeds response line limit");
            emit([&]{raw(row_open);decimal(sequence);raw(after_sequence);decimal(id);raw(after_id);
                hex(std::span(reinterpret_cast<const std::uint8_t*>(path.data()),path.size()));
                raw(after_path);hex(body);raw(row_close);});
        }
    }

    void result(std::string_view json) {
        ready();
        const auto maximum=transport_==Transport::Binary?max_payload_bytes:max_json_line_bytes-1;
        if(json.empty()||json.size()>maximum||json.find('\n')!=json.npos||json.find('\r')!=json.npos)
            throw std::length_error("catalog terminal JSON exceeds single-response bounds");
        emit([&]{if(transport_==Transport::Binary)header(1,json.size());
            raw(json);if(transport_==Transport::Jsonl)raw("\n");});
    }

    void flush() { emit([&]{output_.flush();}); }

private:
    static_assert(std::numeric_limits<std::streamsize>::max()>=max_payload_bytes);
    static constexpr std::string_view row_open="{\"type\":\"row\",\"seq\":";
    static constexpr std::string_view after_sequence=",\"row\":";
    static constexpr std::string_view after_id=",\"path_hex\":\"";
    static constexpr std::string_view after_path="\",\"data_hex\":\"";
    static constexpr std::string_view row_close="\"}\n";
    std::ostream& output_;
    Transport transport_;
    bool poisoned_{};

    [[noreturn]] void fail() { poisoned_=true;throw OutputFailure(); }
    void ready() { if(poisoned_||!output_)fail(); }
    template<class Function> void emit(Function function) {
        ready();
        try { function(); }
        catch(...) { fail(); }
        if(!output_)fail();
    }
    static void little(std::uint8_t* output,std::uint64_t value) noexcept {
        for(unsigned i=0;i<8;++i)output[i]=std::uint8_t(value>>(8*i));
    }
    static std::size_t decimal_size(std::uint64_t value) noexcept {
        std::size_t digits=1;while(value>=10){value/=10;++digits;}return digits;
    }
    void raw(std::string_view bytes) {
        if(!bytes.empty())output_.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        if(!output_)throw OutputFailure();
    }
    void raw(std::span<const std::uint8_t> bytes) {
        if(!bytes.empty())raw(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
    }
    void header(std::uint8_t kind,std::uint64_t length) {
        std::array<std::uint8_t,frame_header_bytes> bytes{'S','F','C','1',kind,0,0,0};
        little(bytes.data()+8,length);raw(bytes);
    }
    void decimal(std::uint64_t value) {
        char buffer[20];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value);
        if(result.ec!=std::errc{})throw std::logic_error("uint64 decimal conversion failed");
        raw(std::string_view(buffer,static_cast<std::size_t>(result.ptr-buffer)));
    }
    void hex(std::span<const std::uint8_t> bytes) {
        constexpr char digits[]="0123456789abcdef";
        std::array<char,8192> buffer{};std::size_t used=0;
        for(const auto byte:bytes) {
            buffer[used++]=digits[byte>>4];buffer[used++]=digits[byte&15];
            if(used==buffer.size()){raw(std::string_view(buffer.data(),used));used=0;}
        }
        if(used)raw(std::string_view(buffer.data(),used));
    }
};
} // namespace suffice::catalog::wire
