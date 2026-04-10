#pragma once

#include <memory>
#include <string>
#include <boost/asio/ssl/context.hpp>

class CertHelpers {
public:
    static std::shared_ptr<boost::asio::ssl::context> make_ssl_context(
        const std::string &cert_file,
        const std::string &key_file,
        const std::string &ca_file
    );

    static bool verify_certificate(const std::string &cert_pem, const std::string &ca_root_pem);

    // extract the public key (in PEM format) from a certificate
    static std::string extract_pubkey_from_cert(const std::string &cert_pem);

    static std::string extract_common_name(const std::string &cert_pem);
};

