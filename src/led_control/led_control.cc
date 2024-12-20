#include "led_control.h"

#include <cstdlib>  // for std::system
#include <string>

namespace mavcam {

void switch_led_mode(LedMode mode) {
    std::string command;
    if (mode == LedMode::Normal) {
        command = "leds-mode --combination 8";
        printf("switch to normal\n");
    } else if (mode == LedMode::Dead) {
        command = "leds-mode --combination 1";
    } else if (mode == LedMode::SDCardError) {
        command = "leds-mode --combination 4 --blink";
    } else if (mode == LedMode::Recording) {
        command = "leds-mode --combination 9";
    } else if (mode == LedMode::TakePhoto) {
        command = "leds-mode --combination 10";
    }
    if (!command.empty()) {
        std::system(command.c_str());
    }
}

}  // namespace mavcam