#pragma once

#include "messages.pb.h"

namespace mesh {
    namespace envelope {
        Envelope MakeCustomText(const std::string &from,
                                const std::string &to,
                                const std::string &payload);
    }
}

