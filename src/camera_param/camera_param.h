#pragma once

#include <fstream>
#include <string>

#include "json/json.h"

namespace mavcam {

class CameraParam final {
public:
    std::string get_value(const std::string &key);
    bool set_value(const std::string &key, const std::string &value);
public:
    CameraParam();
    ~CameraParam();
private:
    std::ifstream _ifstream;
    Json::Value _root;
};

}  // namespace mavcam