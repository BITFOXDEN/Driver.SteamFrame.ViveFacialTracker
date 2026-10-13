// config.cpp - see config.hpp
#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

namespace vftstream {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

Config Config::fromIni(const std::string& path) {
    Config c;
    std::ifstream f(path);
    if (!f) return c;

    std::string line;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        auto toInt = [&]() -> int {
            try { return std::stoi(val); } catch (...) { return 0; }
        };

        if      (key == "port")        c.port = toInt();
        else if (key == "device")      c.device = val;
        else if (key == "quality")     c.quality = std::clamp(toInt(), 1, 100);
        else if (key == "fps")         c.fps = std::clamp(toInt(), 1, 60);
        else if (key == "lut")         c.enableLut = (val == "true" || val == "1" || val == "yes");
        else if (key == "blur")        c.blur = (val == "true" || val == "1" || val == "yes");
        else if (key == "ir_level")    c.irLevel = std::clamp(toInt(), 0, 255);
        else if (key == "hold")        c.holdXu = (val == "true" || val == "1" || val == "yes");
    }
    return c;
}

void Config::applyArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int& idx) -> std::string {
            if (idx + 1 < argc) return argv[++idx];
            return "";
        };
        if (a == "--port" || a == "-p") {
            std::string v = next(i);
            try { port = std::stoi(v); } catch (...) {}
        } else if (a == "--device" || a == "-d") {
            device = next(i);
        } else if (a == "--quality" || a == "-q") {
            try { quality = std::clamp(std::stoi(next(i)), 1, 100); } catch (...) {}
        } else if (a == "--fps") {
            try { fps = std::clamp(std::stoi(next(i)), 1, 60); } catch (...) {}
        } else if (a == "--ir-level") {
            try { irLevel = std::clamp(std::stoi(next(i)), 0, 255); } catch (...) {}
        } else if (a == "--no-lut") {
            enableLut = false;
        } else if (a == "--no-blur") {
            blur = false;
        } else if (a == "--help" || a == "-h") {
            std::cout
                << "Usage: vftstream [options]\n"
                << "  --ini PATH      read an ini config file\n"
                << "  --port N        HTTP port (default 8097)\n"
                << "  --device PATH   usbfs node (default: autodetect the tracker)\n"
                << "  --quality N     JPEG quality 1-100 (default 85)\n"
                << "  --fps N         requested sensor fps, 1..60 (default 60)\n"
                << "  --ir-level N    IR illuminator 0..255 (default 255)\n"
                << "  --no-lut        disable the Babble tone LUT\n"
                << "  --no-blur       disable the 5x5 gaussian blur\n";
            std::exit(0);
        }
    }
}

void Config::writeTemplate(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return;
    f << "# vftstream config\n"
      << "# device: empty => autodetect\n"
      << "port=" << port << "\n"
      << "device=\n"
      << "quality=" << quality << "\n"
      << "fps=" << fps << "\n"
      << "lut=" << (enableLut ? "true" : "false") << "\n"
      << "blur=" << (blur ? "true" : "false") << "\n"
      << "ir_level=" << irLevel << "\n";
}

void Config::print() const {
    std::cout << "vftstream config:"
              << " port=" << port
              << " device=" << (device.empty() ? "(autodetect)" : device)
              << " quality=" << quality
              << " fps=" << fps
              << " lut=" << (enableLut ? "on" : "off")
              << " blur=" << (blur ? "on" : "off")
              << "\n";
}

} // namespace vftstream
