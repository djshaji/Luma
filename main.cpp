
/*
 * main.cpp
 *
 * SPDX-License-Identifier:  BSD-3-Clause
 *
 * Copyright (C) 2026 brummer <brummer@web.de>
 */

/****************************************************************
        main.cpp - a minimal CLI interface for Luma

****************************************************************/

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>
#include <sstream>
#include <limits>

#include "LV2JackX11Host.hpp"

static int last_drawn_lines = 0;

static void clear_previous_output() {
    if (last_drawn_lines <= 0) return;
    // move cursor up
    std::cout << "\033[" << last_drawn_lines << "A";
    // clear to end of screen
    std::cout << "\033[J";
}

static int wait_enter_or_quit() {
    std::cout << "\nENTER = next Page | number = select Plugin | q = quit\n> ";
    std::cout.flush();
    std::string line;
    std::getline(std::cin, line);

    if (line.empty()) return -1;
    if (line == "q" || line == "Q") return -2;

    std::istringstream iss(line);
    int value;
    if (iss >> value) return value;

    return -1;
}

// returns selected index, or -1 if none selected
int pager_print_plugins(const std::vector<std::pair<std::string, std::string>>& matches) {

    if (matches.empty()) return -1;

    const int rows = 10;
    const int cols = 2;
    const int per_page = rows * cols;

    size_t max_len = 0;
    for (auto& m : matches)
        max_len = std::max(max_len, m.second.size());

    const int col_width = std::min<size_t>(max_len + 4, 40);
    size_t index = 0;

    while (index < matches.size()) {
        clear_previous_output();
        size_t end = std::min(index + per_page, matches.size());
        size_t count = end - index;
        size_t r = (count + cols - 1) / cols;
        int drawn = 0;

        for (size_t row = 0; row < r; ++row) {
            for (int col = 0; col < cols; ++col) {
                size_t i = index + row + col * r;
                if (i >= end) continue;
                std::cout << "[" << i << "] " << std::left
                    << std::setw(col_width) << matches[i].second;
            }
            std::cout << "\n";
            drawn++;
        }

        drawn += 2; // instruction + prompt
        last_drawn_lines = drawn;
        int result = wait_enter_or_quit();
        if (result >= 0 && result < (int)matches.size()) return result;
        if (result == -2) return -1;
        index = end;
    }

    //clear_previous_output();
    std::cout << "\nList end, select plugin number or quit: ";
    int choice = -1;
    std::string line;
    std::getline(std::cin, line);
    if (!line.empty()) {
        try {
            choice = std::stoi(line);
        } catch (...) {
            choice = -1;
        }
    }
    if (choice >= 0 && choice < (int)matches.size())
        return choice;

    return -1;
}

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

    LV2X11JackHost host;
    host.init_world();
    auto matches = host.find_plugin_matches(uri);

    if (matches.empty()) {
        std::cerr
            << "No plugin found\n";
        host.closeHost();
        return 1;
    }
    std::cout << "     ╦  ╦ ╦ ╔╦╗ ╔═╗\n";
    std::cout << "     ║  ║ ║ ║║║ ╠═╣\n";
    std::cout << "     ╩═╝╚═╝═╩╝╚═╝ ╩\n";
    std::cout << "  Find " << matches.size() << " matches:\n";

    int pchoice = 0;
    if (matches.size() > 1) pchoice = pager_print_plugins(matches);

    if (pchoice >= 0) {
        std::cout << "Selected: " << matches[pchoice].second << "\n";
        uri = matches[pchoice].first;
    } else {
        host.closeHost();
        return 0;
    }

    if (!host.init(uri.c_str())) return 1;

    auto presets = host.get_presets(uri.c_str());

    if (!presets.empty()) {
        std::cout << "\n  Found presets:\n";
        for (size_t i = 0; i < presets.size(); ++i) {
            std::cout << "    [" << i << "] " << presets[i].label << "\n";
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
    } //else {
      //  std::cout << "No presets found.\n";
    //}

    if (!preset_uri.empty()) host.apply_preset(preset_uri, preset_label);
    if (!host.initUi()) return 1;

    host.run_ui_loop();

    return 0;
}
