#include "message_utils.h"

#include <string>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>

std::string generate_uuid_bytes(int size) {
    static thread_local boost::uuids::random_generator generator;
    boost::uuids::uuid u = generator();
    if (size > u.size()) {
        size = u.size();
    }
    return std::string(reinterpret_cast<const char *>(u.begin()), size);
}

void write_uuid_bytes(std::string &out_buffer, int size) {
    static thread_local boost::uuids::random_generator generator;
    boost::uuids::uuid u = generator();

    out_buffer.resize(u.size());
    std::copy(u.begin(), u.end(), out_buffer.begin());
}
