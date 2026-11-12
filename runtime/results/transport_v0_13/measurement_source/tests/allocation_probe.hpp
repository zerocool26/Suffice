#pragma once
#include <cstdint>
namespace allocation_probe {
void start() noexcept;
std::uint64_t stop() noexcept;
}
