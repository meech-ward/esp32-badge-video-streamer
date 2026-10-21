#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "jpeg_pipeline.hpp"

int main() {
    native_badge::DecodeForecast prediction;
    assert(!prediction.due(100000, 1, 6000));  // first frame must complete first
    prediction.completed(24000, false);
    assert(!prediction.estimate_us());
    prediction.completed(24000, true);
    assert(!prediction.due(18999, 1000, 6000));
    assert(prediction.due(19000, 1000, 6000));
    assert(!prediction.due(100000, 1000, 0));
    assert(!prediction.due(100000, 0, 6000));
    prediction.completed(1, true);
    assert(prediction.estimate_us() == 5000);
    assert(!prediction.due(1999, 1000, 12000));
    assert(prediction.due(2000, 1000, 12000));  // never grant at dispatch
    prediction.completed(200000, true);
    assert(prediction.estimate_us() == 100000);
    prediction.completed(24000, true);
    const uint32_t start = UINT32_MAX - 1000;
    assert(!prediction.due(start + 17999, start, 6000));
    assert(prediction.due(start + 18000, start, 6000));
    prediction.reset();
    assert(!prediction.due(start + 18000, start, 6000));  // reconnect starts cold

    std::vector<uint8_t> memory(native_badge::GuardedJpegInputs::bytes);
    native_badge::GuardedJpegInputs inputs{memory.data()};
    inputs.initialize_guards();
    assert(inputs.intact());
    for (unsigned slot = 0; slot < 2; ++slot) {
        std::fill_n(inputs.slot(slot), inputs.capacity, static_cast<uint8_t>(slot + 1));
        for (uint8_t* guard : {inputs.slot(slot) - inputs.guard, inputs.slot(slot) + inputs.capacity}) {
            for (size_t index : {size_t{0}, inputs.guard - 1}) {
                guard[index] ^= 1;
                assert(!inputs.intact());
                guard[index] ^= 1;
                assert(inputs.intact());
            }
        }
    }
    assert(inputs.slot(0) + inputs.capacity + inputs.guard <= inputs.slot(1) - inputs.guard);
    std::puts("native JPEG pipeline: first/failed decode, lead/clamps/wrap and both-slot guards passed");
}
