#include "string_utils.h"

#include <iomanip>
#include <ios>


std::string to_hex(const std::string &input) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char c: input) {
        ss << std::setw(2) << static_cast<int>(c);
    }
    return ss.str();
}
