// delta_test.cpp — Step 4: prove the compression ratio and losslessness
// with real measurements, not assumed numbers.

#include "delta_codec.h"
#include <vector>
#include <random>
#include <iostream>
#include <fstream>
#include <iomanip>

// Verifies decode(encode(x)) == x exactly, and prints real compression stats.
void run_case(const std::string &label, const std::vector<double> &values) {
    auto encoded = delta_encode(values.data(), values.size());
    auto decoded = delta_decode(encoded, values.size());

    bool exact = (decoded.size() == values.size());
    for (size_t i = 0; exact && i < values.size(); ++i)
        if (d2bits(decoded[i]) != d2bits(values[i])) exact = false;

    size_t raw_bytes = values.size() * sizeof(double);
    double ratio = raw_bytes / (double)encoded.size();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "[" << label << "]\n"
              << "  samples:       " << values.size() << "\n"
              << "  raw size:      " << raw_bytes << " bytes\n"
              << "  encoded size:  " << encoded.size() << " bytes\n"
              << "  ratio:         " << ratio << "x\n"
              << "  lossless:      " << (exact ? "PASS (bit-exact)" : "FAIL") << "\n\n";
}

int main(int argc, char **argv) {
    // Case 1: realistic slowly-varying sensor data with small random walk noise
    // (this is the honest "typical telemetry" case — not cherry-picked).
    {
        std::mt19937 rng(42);
        std::normal_distribution<double> noise(0.0, 0.01);
        std::vector<double> values;
        double v = 10.0;
        for (int i = 0; i < 100000; ++i) {
            v += noise(rng);
            values.push_back(v);
        }
        run_case("synthetic sensor random-walk, N=100000", values);
    }

    // Case 2: your actual telemetry.bin from the Step 3 mmap test, if present.
    if (argc > 1) {
        std::ifstream f(argv[1], std::ios::binary);
        if (f) {
            std::vector<double> values;
            double v;
            while (f.read(reinterpret_cast<char *>(&v), sizeof(v)))
                values.push_back(v);
            if (!values.empty())
                run_case(std::string("real file: ") + argv[1], values);
            else
                std::cout << "[warning] " << argv[1] << " had no readable doubles\n";
        } else {
            std::cout << "[warning] could not open " << argv[1] << "\n";
        }
    } else {
        std::cout << "(pass a path to telemetry.bin as argv[1] to test real captured data)\n";
    }

    return 0;
}
