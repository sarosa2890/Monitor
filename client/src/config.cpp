#include "config.h"

#include <windows.h>
#include <fstream>
#include <sstream>

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool toBool(const std::string& v) {
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool loadConfig(Config& cfg, const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        for (auto& c : key) c = (char)tolower((unsigned char)c);

        if (key == "host")                       cfg.host = val;
        else if (key == "port")                  cfg.port = atoi(val.c_str());
        else if (key == "tls")                   cfg.tls = toBool(val);
        else if (key == "client_id")             cfg.client_id = val;
        else if (key == "client_key")            cfg.client_key = val;
        else if (key == "screen_fps")            cfg.screen_fps = max(1, min(60, atoi(val.c_str())));
        else if (key == "camera_fps")            cfg.camera_fps = max(1, min(60, atoi(val.c_str())));
        else if (key == "jpeg_quality")          cfg.jpeg_quality = max(5, min(100, atoi(val.c_str())));
        else if (key == "capture_screen")        cfg.capture_screen = toBool(val);
        else if (key == "capture_camera")        cfg.capture_camera = toBool(val);
        else if (key == "location")              cfg.location = val;
        else if (key == "log")                   cfg.log = toBool(val);
    }
    return true;
}
