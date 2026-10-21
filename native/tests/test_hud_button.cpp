#include <cassert>
#include <cstdint>
#include "hud_button.hpp"

int main() {
    native_badge::HudButton button;
    assert(!button.update(0, false));
    assert(!button.update(1, true));
    assert(!button.update(10, false));  // bounce never becomes a press
    assert(!button.update(11, true));
    assert(!button.update(30, true));
    assert(button.update(31, true));
    assert(!button.update(999999, true));  // held through a long video stall
    assert(!button.update(1000000, false));
    assert(!button.update(1000020, false));
    assert(!button.update(UINT32_MAX - 10, true));
    assert(!button.update(8, true));
    assert(button.update(9, true));  // elapsed timer wraps
    assert(!button.update(10, true));
}
