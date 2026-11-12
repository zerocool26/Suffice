#include "../apps/catalog/digest.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <utility>
using namespace suffice::catalog::detail;
int main() {
    assert(sha256(std::string_view{})=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(sha256(std::string_view("abc"))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(sha256(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    assert(sha256(std::string_view(std::string(1000000,'a')))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    const std::vector<std::pair<std::size_t,std::string>> vectors{
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {1, "4a64a107f0cb32536e5bce6c98c393db21cca7f4ea187ba8c4dca8b51d4ea80a"},
        {2, "b27fc04e05a940672b77fb0d1aaf6a1f6e3675603da78e6791a66b6523c46fd4"},
        {3, "abf0f7ab4223b86bbafb06e42d0caf0e889d27619b946d440a128bb405b7c5a3"},
        {7, "3ee1a2306d13d6997cfc2dd52277bf6e774e81900ca5be136a1e046486840aa5"},
        {31, "eb652a88693e86197777a1c1d3b70c9048aec9fbe3d8cc82d7fc5fa3634b23ed"},
        {55, "27d8c9cbd89e4948898160251e415299e428bfc4303eaacd91d688e238e3295f"},
        {56, "78ecd53d548f32413485715cf0827b9ed9039ea29c1700c0d9f6356c7cd85a57"},
        {57, "48cf2f620936f64ac263f05540d03bb83e1753ffce24fbb73b85e39f8763cff6"},
        {63, "b67a66299d2ac96db533a78f2049dafa1a0ae69bc7847ad50f7ff8ec7e48eb4a"},
        {64, "3de2d8dc9d4b2ed670902a2a0fc489bb3aeacbb998f1199523cd341c559ad65d"},
        {65, "dc2727271774ede217c5573ab43d316e3ba3a43d404f7ac845c984e3dbfed1c3"},
        {119, "0676f58e69c74117e721cd084242d33bab20061364873cb97c1b18482518951b"},
        {120, "f6f2c77228651bfe336e9a279403d80087b4df2d88edb75f8943895beb5a5c45"},
        {127, "b7f38fe5639e3765210590f4df7f4ad53f074030671ae50b33256d9908a8e4f8"},
        {128, "9a33f178a6820866234b772e0daef0fc6a05b01aac4ea68b79fcd1a9b07b644f"},
        {129, "86b3524cc02fe0b3b03e0153391d361b66448746198d1ba69ca5068eb64e1b1f"},
        {255, "cfb060a1ab67f924cb892fd010b6cb9bbd0cc04f8b21686cee0e7cd66b9ccf0c"},
        {256, "1e988ca88fc8da79517742379b1572ae7bf0444bc8ada2a2f14afee11b652ff5"},
        {511, "e88db73d03e28127fe42a6943cc2cdb30f7a4beebe36579ac9f7111423e45575"},
        {512, "28eec35f03ca235530466ce8ed775850fd2d5d0118560096c706d1bc00204fcb"},
        {1023, "8eb384993e9faa4deea70b65924cfbb60e9d5697b5d13757852eb380d8d5f260"},
        {1024, "f0534be1d23cc783e43934494ad8c4785e9f9ef17bb03fcb8b5b0aff6887f202"},
        {4096, "2f059d57d5d4093e6162d725918d3f928b7a09d595d3d7746b54157f87b9003d"},
        {16384, "1c73adda60cd5c142f7dd9e5a822cb40a3a63349d4de10fa05083bbb39f54027"}
    };
    for(const auto& [length,expected]:vectors){std::vector<std::uint8_t> body(length);for(std::size_t i=0;i<length;++i)body[i]=std::uint8_t(i*131+i/7+17);assert(sha256(body)==expected);}
    const std::vector<std::vector<std::uint8_t>> valid{{},{0},{0x7f},{0xc2,0x80},{0xdf,0xbf},{0xe0,0xa0,0x80},{0xed,0x9f,0xbf},{0xf0,0x90,0x80,0x80},{0xf4,0x8f,0xbf,0xbf}};
    const std::vector<std::vector<std::uint8_t>> invalid{{0x80},{0xc0,0x80},{0xc1,0xbf},{0xc2},{0xe0,0x80,0x80},{0xed,0xa0,0x80},{0xf0,0x80,0x80,0x80},{0xf4,0x90,0x80,0x80},{0xf5,0x80,0x80,0x80},{0xff},{0xc2,0x41}};
    for(const auto& body:valid)assert(utf8(body));
    for(const auto& body:invalid)assert(!utf8(body));
    assert(unhex("00617fff")==std::string("\0a\x7f\xff",4));
    unsigned rejected=0;for(auto bad:{"0","AA","0g","-1"}){try{(void)unhex(bad);}catch(const std::invalid_argument&){++rejected;}}
    assert(rejected==4);
    std::cout<<"PASS: 29 SHA-256 vectors; 20 UTF-8 boundaries; hexadecimal roundtrip and 4 rejections.\n";
}
