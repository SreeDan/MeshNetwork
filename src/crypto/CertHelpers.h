#pragma once

#include <string>

class CertHelpers {
public:
    static bool verify_certificate(const std::string &cert_pem, const std::string &ca_root_pem);

    // extract the public key (in PEM format) from a certificate
    static std::string extract_pubkey_from_cert(const std::string &cert_pem);

    static std::string extract_common_name(const std::string &cert_pem);
};

