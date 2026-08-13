#include <iostream>

void RunPayloadTests();
void RunPathTests();

int main() {
  try {
    RunPayloadTests();
    RunPathTests();
    std::cout << "All tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}

