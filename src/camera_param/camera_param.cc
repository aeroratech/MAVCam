#include "camera_param.h"

#include "base/log.h"

namespace mavcam {

std::string kDefaultStorePath = "/data/camera/cam_param.bin";

std::string CameraParam::get_value(const std::string &key) {
    return _root[key].asString();
}

bool CameraParam::set_value(const std::string &key, const std::string &value) {
    std::ofstream ofs(kDefaultStorePath, std::ofstream::trunc);
    if (!ofs.is_open()) {
        base::LogError() << "Error: Could not open file " << kDefaultStorePath << " for writing";
        return false;
    }

    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "    ";
    std::unique_ptr<Json::StreamWriter> writer(writerBuilder.newStreamWriter());
    _root[key] = value;
    writer->write(_root, &ofs);  // write back to file
    ofs.close();
    return true;
}

CameraParam::CameraParam() {
    _ifstream.open(kDefaultStorePath, std::ifstream::binary);
    if (!_ifstream.is_open()) {
        base::LogError() << "Error: Could not open file ";
        return;
    }
    Json::CharReaderBuilder builder;
    builder["collectComments"] = true;
    JSONCPP_STRING errs;
    if (!parseFromStream(builder, _ifstream, &_root, &errs)) {
        base::LogError() << errs;
        _ifstream.close();
        return;
    }
    base::LogDebug() << "Init camera param " << _root;
    _ifstream.close();
}

CameraParam::~CameraParam() {}

}  // namespace mavcam