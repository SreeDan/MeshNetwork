#include "CertHelpers.h"

#include <string>
#include <memory>
#include <iostream>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/pem.h>
#include <openssl/err.h>

bool CertHelpers::verify_certificate(const std::string &cert_pem, const std::string &ca_root_pem) {
    BIO *ca_bio = BIO_new_mem_buf(ca_root_pem.c_str(), -1);
    X509 *ca_cert = PEM_read_bio_X509(ca_bio, nullptr, nullptr, nullptr);
    BIO_free(ca_bio);
    if (!ca_cert) return false;

    // load the Node Certificate
    BIO *cert_bio = BIO_new_mem_buf(cert_pem.c_str(), -1);
    X509 *target_cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
    BIO_free(cert_bio);
    if (!target_cert) {
        X509_free(ca_cert);
        return false;
    }

    // create a Store and add the CA
    X509_STORE *store = X509_STORE_new();
    X509_STORE_add_cert(store, ca_cert);

    // create a Context to verify the target
    X509_STORE_CTX *ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, target_cert, nullptr);

    int ret = X509_verify_cert(ctx);

    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    X509_free(target_cert);
    X509_free(ca_cert);

    return (ret == 1);
}

std::string CertHelpers::extract_pubkey_from_cert(const std::string &cert_pem) {
    BIO *cert_bio = BIO_new_mem_buf(cert_pem.c_str(), -1);
    X509 *cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
    BIO_free(cert_bio);
    if (!cert) return "";

    EVP_PKEY *pkey = X509_get_pubkey(cert);
    if (!pkey) {
        X509_free(cert);
        return "";
    }

    // write public key to PEM string
    BIO *key_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(key_bio, pkey);

    char *data = nullptr;
    long len = BIO_get_mem_data(key_bio, &data);
    std::string result(data, len);

    BIO_free(key_bio);
    EVP_PKEY_free(pkey);
    X509_free(cert);
    return result;
}

std::string CertHelpers::extract_common_name(const std::string &cert_pem) {
    BIO *cert_bio = BIO_new_mem_buf(cert_pem.c_str(), -1);
    X509 *cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
    BIO_free(cert_bio);
    if (!cert) return "";

    char buffer[256];
    X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, buffer, 256);

    X509_free(cert);
    return std::string(buffer);
}
