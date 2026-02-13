
/*
 * main.cpp
 *
 * SPDX-License-Identifier:  BSD-3-Clause
 *
 * Copyright (C) 2026 brummer <brummer@web.de>
 */

/****************************************************************
        main.cpp - the luma main interface

****************************************************************/

#include <iostream>
#include <string>
#include <limits>

#include "LV2JackX11Host.hpp"

int main(int argc, char *argv[]) {

    if (0 == XInitThreads())
        std::cerr << "Warning: XInitThreads() failed\n";

    if (argc < 2) {
        std::cout << "Minimal LV2 X11 host\n";
        std::cout << "Usage:\n";
        std::cout << "  " << argv[0] << " plugin_uri [preset_number]\n";
        return 0;
    }

    std::string uri = argv[1];
    std::string preset_uri;
    std::string preset_label;

    LV2X11JackHost host(uri.c_str());

    if (!host.init()) return 1;

    auto presets = host.get_presets(uri.c_str());

    if (!presets.empty()) {
        std::cout << "     ╦  ╦ ╦ ╔╦╗ ╔═╗\n";
        std::cout << "     ║  ║ ║ ║║║ ╠═╣\n";
        std::cout << "     ╩═╝╚═╝═╩╝╚═╝ ╩\n";
        std::cout << "  Found presets:\n";
        for (size_t i = 0; i < presets.size(); ++i) {
            std::cout << "    [" << i << "] "
                      << presets[i].label << "\n";
        }

        int choice = -1;
        if (argc >= 3) {
            try {
                choice = std::stoi(argv[2]);
            } catch (...) {
                choice = -1;
            }
        } else {
            std::cout << "\nSelect preset (ENTER = default): ";
            std::string line;
            std::getline(std::cin, line);
            if (!line.empty()) {
                try {
                    choice = std::stoi(line);
                } catch (...) {
                    choice = -1;
                }
            }
        }

        if (choice >= 0 && choice < static_cast<int>(presets.size())) {
            preset_uri = presets[choice].uri;
            preset_label = presets[choice].label;
            std::cout << "\nLoading preset: "
                      << presets[choice].label
                      << "\n";
        }
    } else {
        std::cout << "No presets found.\n";
    }

    if (!preset_uri.empty()) host.apply_preset(preset_uri, preset_label);
    if (!host.initUi()) return 1;

    host.run_ui_loop();

    return 0;
}
