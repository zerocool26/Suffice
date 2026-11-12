// Build after emitting portfolio.hpp with namespace app_portfolio.
// The expected mask is supplied independently, for the identical input/program.
#include "portfolio.hpp"
#include "suffice/storage.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            std::cerr << "Usage: portfolio_runner input.sfp expected.mask expected_remaining_calls\n";
            return 2;
        }
        std::uint64_t reuse{};
        const std::string_view text(argv[3]);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), reuse);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            throw std::invalid_argument("expected_remaining_calls must be an unsigned integer");

        const suffice::PackedDataset data{std::filesystem::path(argv[1])};
        app_portfolio::PreparedEncodedPortfolio prepared(data);
        const auto rows = prepared.rows();
        const auto words = rows / 64 + (rows % 64 != 0);
        if (words > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t))
            throw std::length_error("mask exceeds addressable storage");
        std::vector<std::uint64_t> expected(static_cast<std::size_t>(words));
        std::vector<std::uint64_t> scratch(expected.size()), result(expected.size());
        std::ifstream stream(argv[2], std::ios::binary);
        if (!stream) throw std::runtime_error("cannot open expected mask");
        for (auto& word : expected) {
            std::array<unsigned char, 8> bytes{};
            if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
                throw std::runtime_error("expected mask is truncated");
            for (unsigned byte = 0; byte < 8; ++byte)
                word |= static_cast<std::uint64_t>(bytes[byte]) << (8 * byte);
        }
        if (stream.get() != std::char_traits<char>::eof())
            throw std::runtime_error("expected mask has excess bytes");
        if (!stream.eof()) throw std::runtime_error("expected mask read failed");

        // Four warmups plus three complete four-candidate Williams cycles:
        // 4 + 3*4*4 = 52 candidate calls. This is a hard row-work cap.
        const auto cap = rows > std::numeric_limits<std::uint64_t>::max() / 52
            ? std::numeric_limits<std::uint64_t>::max() : 52 * rows;
        const auto report = prepared.calibrate(0, rows, expected, scratch,
            {.max_row_evaluations=cap, .max_elapsed_nanoseconds=250000000,
             .expected_remaining_calls=reuse});
        const auto accepted = prepared.run(result);
        if (result != expected) throw std::runtime_error("fresh run differs from supplied exact mask");
        std::cout << "{\"selected\":\"" << prepared.selected_name()
                  << "\",\"reason\":\"" << suffice::portfolio_reason_name(report.reason)
                  << "\",\"stop_reason\":\"" << suffice::portfolio_stop_reason_name(report.stop_reason)
                  << "\",\"rows\":" << rows << ",\"accepted\":" << accepted
                  << ",\"calibration_calls\":" << report.total_calls
                  << ",\"calibration_rows\":" << report.row_evaluations
                  << ",\"completed_cycles\":" << report.completed_cycles
                  << ",\"calibration_ns\":" << report.elapsed_nanoseconds
                  << ",\"estimated_break_even_calls\":" << report.estimated_break_even_calls
                  << ",\"view_storage_bytes\":" << prepared.view_storage_bytes()
                  << ",\"fresh_output_verified\":true}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "portfolio_runner: " << error.what() << '\n';
        return 2;
    }
}
