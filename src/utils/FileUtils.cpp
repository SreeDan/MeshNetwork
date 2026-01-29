#include "FileUtils.h"

#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>

std::string read_file_to_string(const std::string &filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("IdentityManager: Failed to open file: " + filepath);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}
