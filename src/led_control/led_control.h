#pragma once

namespace mavcam {

enum class LedMode {
    Normal,       ///< normal mode
    Dead,         ///< mav server has quit
    SDCardError,  ///< sdcard error
    Recording,    ///< start video recording
    TakePhoto,    ///< take photo
};

void switch_led_mode(LedMode mode);

}  // namespace mavcam