#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "led_control/led_control.h"

bool isProcessRunning(const std::string &processName) {
    std::string command = "pgrep -x " + processName + " > /dev/null 2>&1";
    int ret = std::system(command.c_str());

    if (ret == -1) {
        std::cerr << "Failed to execute system command." << std::endl;
        return false;  // Treat as not running if command execution fails
    }

    return (ret == 0);
}

int main() {
    const std::string processName = "mav_client";
    const int checkInterval = 5;  // in seconds

    std::cout << "Monitoring process: " << processName << std::endl;

    while (true) {
        if (!isProcessRunning(processName)) {
            mavcam::switch_led_mode(mavcam::LedMode::Dead);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::seconds(checkInterval));
    }

    return 0;
}
