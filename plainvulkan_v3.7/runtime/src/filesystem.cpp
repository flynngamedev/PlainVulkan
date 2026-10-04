// filesystem.cpp -- Pv::FileExists / ReadFile / WriteFile / ... . Plain
// <filesystem>/<fstream>, genuinely functional, no tiers to speak of.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace pv {
namespace rt {

Value FileExists(const Value& filepath) { return Value(std::filesystem::exists(filepath.asString())); }

Value ReadFile(const Value& filepath) {
    std::ifstream f(filepath.asString(), std::ios::binary);
    if (!f.is_open()) {
        logLine("ERROR", "Pv::ReadFile: could not open '" + filepath.asString() + "'");
        return Value();
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return Value(ss.str());
}

Value WriteFile(const Value& filepath, const Value& data) {
    std::ofstream f(filepath.asString(), std::ios::binary);
    if (!f.is_open()) {
        logLine("ERROR", "Pv::WriteFile: could not open '" + filepath.asString() + "' for writing");
        return Value(false);
    }
    std::string s = data.asString();
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    return Value(true);
}

Value DeleteFile(const Value& filepath) {
    std::error_code ec;
    bool removed = std::filesystem::remove(filepath.asString(), ec);
    return Value(removed && !ec);
}

Value GetWorkingDir() { return Value(std::filesystem::current_path().string()); }

Value SetWorkingDir(const Value& path) {
    std::error_code ec;
    std::filesystem::current_path(path.asString(), ec);
    return Value(!ec);
}

} // namespace rt
} // namespace pv
