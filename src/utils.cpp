#include "utils.h"
#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>

namespace fs = std::filesystem;

std::string pickFile(const std::string& defaultDir) {
    std::vector<std::string> files;

    if (fs::exists(defaultDir) && fs::is_directory(defaultDir)) {
        for (const auto& entry : fs::directory_iterator(defaultDir)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path().filename().string());
            }
        }
    }

    if (files.empty()) {
        std::cout << "[Error] No files found in " << defaultDir << std::endl;
        return {};
    }

    std::sort(files.begin(), files.end());

    std::cout << "Files in " << defaultDir << ":\n";
    for (size_t i = 0; i < files.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << files[i] << "\n";
    }

    std::cout << "Choose file number (or 0 to cancel): ";
    size_t choice = 0;
    std::cin >> choice;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    if (choice == 0 || choice > files.size()) {
        std::cout << "[Cancelled]\n";
        return {};
    }

    return (fs::path(defaultDir) / files[choice - 1]).string();
}
