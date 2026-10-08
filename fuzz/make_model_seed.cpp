// Writes the small test model to a file, as a starting point for the model fuzzer.
#include <fstream>

#include "model_builder.hpp"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto bytes = testutil::stub_model_bytes();
    std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out ? 0 : 1;
}
