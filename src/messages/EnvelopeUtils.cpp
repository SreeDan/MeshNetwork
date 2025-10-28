#include "EnvelopeUtils.h"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace mesh {
    static boost::uuids::uuid gen_uuid() {
        static boost::uuids::random_generator generator;
        return generator();
    }

    Envelope MakeCustomText(const std::string &from,
                            const std::string &to,
                            const std::string &payload) {
        Envelope e;
        e.set_from(from);
        e.set_to(to);
        e.set_msg_id(gen_uuid().data, 16);
        e.set_payload(payload);
        e.set_type(mesh::CUSTOM_TEXT);
        e.set_expect_response(false);
        return e;
    }
}
