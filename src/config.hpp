// config.hpp - ini + command line parsing
#pragma once

#include <string>

namespace vftstream {

struct Config {
    int port = 8097;
    std::string device = "";        // usbfs node; empty => autodetect
    int quality = 85;               // JPEG quality
    int fps = 60;                   // requested sensor fps (max 60)
    bool enableLut = true;          // Babble tone LUT on/off
    bool blur = true;               // 5x5 gaussian on/off
    int irLevel = 0xff;             // IR illuminator 0..255; lower to reduce
                                    // VBUS draw on weak hosts
    bool holdXu = false;            // diagnostic: hold the XU session open
                                    // (stream stays off until /debug/resume)

    static Config fromIni(const std::string& path);
    void applyArgs(int argc, char** argv);
    void print() const;
    void writeTemplate(const std::string& path) const;
};

} // namespace vftstream
