// SPDX-License-Identifier: MIT
#include "../../src/internal.hpp"
#include <cassert>

int main() {
    using irisva::capture_pool_size;
    constexpr size_t mib = 1024 * 1024;
    // Small frames preserve renderer hints; large frames are byte-budgeted.
    assert(capture_pool_size(32, 6, 1920 * 1088 * 3 / 2) == 32);
    assert(capture_pool_size(52, 6, 3840 * 2176 * 3 / 2) == 52);
    assert(capture_pool_size(32, 6, 7680 * 4320 * 3 / 2) == 16);
    assert(capture_pool_size(52, 6, 7680 * 4320 * 3 / 2) == 16);
    assert(capture_pool_size(32, 6, 7680 * 4320 * 3) == 8);
    // The negotiated allocation size includes padding, regardless of format.
    assert(capture_pool_size(32, 6, 48 * mib) == 16);
    assert(capture_pool_size(32, 6, 48 * mib + 1) == 15);
    assert(capture_pool_size(4, 6, mib) == 6);
    assert(capture_pool_size(100, 6, mib) == 64);
    assert(capture_pool_size(32, 16, 48 * mib) == 16);
    try {
        capture_pool_size(32, 17, 48 * mib);
        assert(false);
    } catch (const irisva::Error &e) {
        assert(e.status == VA_STATUS_ERROR_ALLOCATION_FAILED);
    }
    for (auto minimum : {0u, 65u}) {
        try {
            capture_pool_size(32, minimum, mib);
            assert(false);
        } catch (const irisva::Error &) {
        }
    }
    try {
        capture_pool_size(32, 6, 0);
        assert(false);
    } catch (const irisva::Error &) {
    }
}
