#include <iostream>

void RunCliTests();
void RunFilesystemAccessTests();
void RunIpcTests();
void RunPayloadTests();
void RunPathTests();
void RunResourceTests();

int main() {
  try {
    RunCliTests();
    RunFilesystemAccessTests();
    RunIpcTests();
    RunPayloadTests();
    RunPathTests();
    RunResourceTests();
    std::cout << "All tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
