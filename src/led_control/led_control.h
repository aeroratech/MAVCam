#pragma once

namespace mavcam {

enum class LedMode {
    Normal,  ///< normal mode
    Dead,    ///< mav server has quit
    Recording,
};

void switch_led_mode(LedMode mode);

}  // namespace mavcam