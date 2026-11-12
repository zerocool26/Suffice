#include <exception>
#include <iostream>

void test_payload();

int main() {
    try {
        test_payload();
        std::cout << "All payload pipeline tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
