#include "CryptoHelpers.h"

#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/evp.h>


std::string CryptoHelpers::generate_aes_key() {
    unsigned char key[AES_KEY_SIZE];
    RAND_bytes(key, AES_KEY_SIZE);
    return std::string((char *) key, AES_KEY_SIZE);
}

std::string CryptoHelpers::aes_encrypt(const std::string &key, const std::string &plaintext) {
    if (key.size() != AES_KEY_SIZE)
        throw std::runtime_error("Invalid AES key size");

    // Generate the IV (Initialization Vector)
    // The IV makes sure that if we encrypt the same message twice with the same
    // key, the output looks different
    unsigned char iv[AES_IV_SIZE];
    if (RAND_bytes(iv, AES_IV_SIZE) != 1)
        throw std::runtime_error("Failed to generate IV");

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        throw std::runtime_error("Failed to create EVP context");

    // Select the aes_256_cbc algorithm
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL,
                           (unsigned char *) key.data(), iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptInit failed");
    }

    // Actual encryption happens here. AES encrypts in chunks of 16 bytes
    std::vector<unsigned char> ciphertext(plaintext.size() + AES_BLOCK_SIZE);
    int len = 0;
    int ciphertext_len = 0;

    // If the data isn't a multiple of 16, add padding to fill in the last block
    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                          (unsigned char *) plaintext.data(), plaintext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptUpdate failed");
    }
    ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptFinal failed");
    }
    ciphertext_len += len;
    EVP_CIPHER_CTX_free(ctx);

    // Prepend the IV with the ciphertext. The decryptor will need to use the IV.
    // NOTE: the iv is not secret, it's just unique
    std::string result;
    result.reserve(AES_IV_SIZE + ciphertext_len);
    result.append((char *) iv, AES_IV_SIZE);
    result.append((char *) ciphertext.data(), ciphertext_len);

    return result;
}

std::string CryptoHelpers::aes_decrypt(const std::string &key, const std::string &encrypted_blob) {
    if (key.size() != AES_KEY_SIZE)
        throw std::runtime_error("Invalid AES key size");

    if (encrypted_blob.size() < AES_IV_SIZE)
        throw std::runtime_error("Data too short");

    // Encrypted blob format: [IV + Ciphertext]
    unsigned char iv[AES_IV_SIZE];
    std::memcpy(iv, encrypted_blob.data(), AES_IV_SIZE);

    // Extract the IV from the beginning of the data
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("Failed to create EVP context");

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, (unsigned char *) key.data(), iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptInit failed");
    }

    // Skip over the IV portion
    const unsigned char *ciphertext_ptr = (unsigned char *) encrypted_blob.data() + AES_IV_SIZE;
    int ciphertext_len = encrypted_blob.size() - AES_IV_SIZE;

    std::vector<unsigned char> plaintext(ciphertext_len + AES_BLOCK_SIZE);
    int len = 0;
    int plaintext_len = 0;

    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext_ptr, ciphertext_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptUpdate failed");
    }
    plaintext_len = len;

    // Checks the last padding bytes. If it looks random, it throws an error
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptFinal failed (Bad Key or Padding?)");
    }
    plaintext_len += len;
    EVP_CIPHER_CTX_free(ctx);

    return std::string((char *) plaintext.data(), plaintext_len);
}


std::string CryptoHelpers::rsa_encrypt_key(const std::string &pub_key_pem, const std::string &aes_key) {
    // Load the public key from the PEM string
    BIO *keybio = BIO_new_mem_buf(pub_key_pem.c_str(), -1);
    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(keybio, NULL, NULL, NULL);
    BIO_free(keybio);
    if (!pkey)
        throw std::runtime_error("Failed to load Public Key");

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
    if (!ctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to create PKEY context");
    }

    if (EVP_PKEY_encrypt_init(ctx) <= 0)
        throw std::runtime_error("Encrypt init failed");

    // Set the padding for RSA using PKCS1_OAEP
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0)
        throw std::runtime_error("Failed to set OAEP padding");

    size_t outlen;
    // First call determines buffer size
    if (EVP_PKEY_encrypt(ctx, NULL, &outlen, (unsigned char *) aes_key.data(), aes_key.size()) <= 0)
        throw std::runtime_error("Failed to determine encrypt buffer size");

    std::vector<unsigned char> out(outlen);
    // Second call does the encryption
    if (EVP_PKEY_encrypt(ctx, out.data(), &outlen, (unsigned char *) aes_key.data(), aes_key.size()) <= 0)
        throw std::runtime_error("PKEY encrypt failed");

    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    return std::string((char *) out.data(), outlen);
}

std::string CryptoHelpers::rsa_decrypt_key(const std::string &priv_key_pem, const std::string &encrypted_key) {
    BIO *keybio = BIO_new_mem_buf(priv_key_pem.c_str(), -1);
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(keybio, NULL, NULL, NULL);
    BIO_free(keybio);
    if (!pkey)
        throw std::runtime_error("Failed to load Private Key");

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
    if (!ctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to create PKEY context");
    }

    if (EVP_PKEY_decrypt_init(ctx) <= 0)
        throw std::runtime_error("Decrypt init failed");

    // Set the padding, should match the encryption padding
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0)
        throw std::runtime_error("Failed to set OAEP padding");

    size_t outlen;
    if (EVP_PKEY_decrypt(ctx, NULL, &outlen, (unsigned char *) encrypted_key.data(), encrypted_key.size()) <= 0)
        throw std::runtime_error("Failed to determine decrypt buffer size");

    std::vector<unsigned char> out(outlen);
    if (EVP_PKEY_decrypt(ctx, out.data(), &outlen, (unsigned char *) encrypted_key.data(), encrypted_key.size()) <= 0)
        throw std::runtime_error("PKEY decrypt failed");

    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    return std::string((char *) out.data(), outlen);
}

std::string CryptoHelpers::sign_message(const std::string &priv_key_pem, const std::string &plain_text) {
    BIO *keybio = BIO_new_mem_buf(priv_key_pem.c_str(), -1);
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(keybio, NULL, NULL, NULL);
    BIO_free(keybio);
    if (!pkey) throw std::runtime_error("Failed to load private key for signing");

    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to create MD context");
    }

    // Initialize signing with SHA-256
    if (EVP_DigestSignInit(md_ctx, NULL, EVP_sha256(), NULL, pkey) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("DigestSignInit failed");
    }

    if (EVP_DigestSignUpdate(md_ctx, plain_text.data(), plain_text.size()) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("DigestSignUpdate failed");
    }

    size_t sig_len;
    if (EVP_DigestSignFinal(md_ctx, NULL, &sig_len) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("DigestSignFinal (size) failed");
    }

    std::vector<unsigned char> sig(sig_len);
    if (EVP_DigestSignFinal(md_ctx, sig.data(), &sig_len) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("DigestSignFinal failed");
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return std::string((char *) sig.data(), sig_len);
}


bool CryptoHelpers::verify_signature(const std::string &pub_key_pem, const std::string &plain_text,
                                     const std::string &signature) {
    BIO *keybio = BIO_new_mem_buf(pub_key_pem.c_str(), -1);
    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(keybio, NULL, NULL, NULL);
    BIO_free(keybio);
    if (!pkey) return false;

    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        return false;
    }

    // Initially verify with SHA-256
    if (EVP_DigestVerifyInit(md_ctx, NULL, EVP_sha256(), NULL, pkey) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return false;
    }

    // Process the plain text to calculate what the hash should be
    if (EVP_DigestVerifyUpdate(md_ctx, plain_text.data(), plain_text.size()) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return false;
    }

    // Compare calculated hash against the decrypted signature
    int ret = EVP_DigestVerifyFinal(md_ctx, (unsigned char *) signature.data(), signature.size());

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return (ret == 1);
}
