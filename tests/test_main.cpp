#include <iostream>

void RunCliTests();
void RunFilesystemAccessTests();
void RunIpcTests();
void RunPayloadBindingTests();
void RunPayloadTests();
void RunPathTests();
void RunPublishConfigTests();
void RunPublisherTests();
void RunResourceTests();
void RunSigningTests();

int main() {
  try {
    RunCliTests();
    RunFilesystemAccessTests();
    RunIpcTests();
    RunPayloadBindingTests();
    RunPayloadTests();
    RunPathTests();
    RunPublishConfigTests();
    RunPublisherTests();
    RunResourceTests();
    RunSigningTests();
    std::cout << "All tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
