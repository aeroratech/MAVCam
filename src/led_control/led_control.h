#pragma once

namespace mavcam {

enum class LedMode {
    Normal,  ///< normal mode
    Recording,
};

void switch_led_mode(LedMode mode);

}