#include "stdafx.h"

#include <CoreLib/Crypto.h>
#include <iomanip>

BEGIN_SE()

// BG3Access fork: replaced Norbyte's upstream public key with our own
// secp256r1 / NIST P-256 keypair generated via UpdateSigner.exe genkey.
// Private key lives at D:\BG3Access\bg3access_signing.priv.key on the
// maintainer's machine and is NEVER committed.  If this key is ever
// compromised or lost: regenerate the pair, rebake the new public key
// below, rebuild the BG3Updater loader, ship the new loader to every
// existing user (their cached old loader would refuse signed updates
// from the new key, so cache-bust path is via the loader binary).
static uint8_t UpdaterPublicKey[2 * NUM_ECC_BYTES] = {
    0x41, 0x7e, 0x34, 0xd1, 0x77, 0x71, 0x9a, 0x43, 0xce, 0x42, 0xb2, 0x1f, 0xf6, 0xaa, 0x18, 0x71,
    0x9a, 0x59, 0xcd, 0xbf, 0x05, 0xef, 0x18, 0x02, 0xf7, 0x37, 0x5c, 0xbb, 0xcd, 0xc6, 0x4d, 0x8d,
    0x3b, 0xd4, 0x08, 0x5c, 0x92, 0x39, 0xdd, 0x5c, 0x61, 0x66, 0x2f, 0x95, 0xe6, 0x26, 0x6a, 0xf3,
    0x40, 0x23, 0xd9, 0x7c, 0x34, 0x35, 0x93, 0x19, 0xaa, 0x64, 0xe3, 0x65, 0xe2, 0xa1, 0x67, 0x3d
};

bool CryptoUtils::SHA256(uint8_t* data, size_t len, uint8_t* digest)
{
    tc_sha256_state_struct sha;
    if (tc_sha256_init(&sha) != TC_CRYPTO_SUCCESS) {
        return false;
    }

    if (tc_sha256_update(&sha, data, len) != TC_CRYPTO_SUCCESS) {
        return false;
    }

    return tc_sha256_final(digest, &sha) == TC_CRYPTO_SUCCESS;
}


bool CryptoUtils::EccVerify(uint8_t* data, size_t len, uint8_t* publicKey, uint8_t* signature)
{
    uint8_t digest[TC_SHA256_DIGEST_SIZE];
    if (!SHA256(data, len, digest)) {
        return false;
    }

    return uECC_verify(publicKey, digest, sizeof(digest), signature, uECC_secp256r1()) == TC_CRYPTO_SUCCESS;
}


bool CryptoUtils::EccSign(uint8_t* data, size_t len, uint8_t* privateKey, uint8_t* signature)
{
    uint8_t digest[TC_SHA256_DIGEST_SIZE];
    if (!SHA256(data, len, digest)) {
        return false;
    }

    return uECC_sign(privateKey, digest, sizeof(digest), signature, uECC_secp256r1()) == TC_CRYPTO_SUCCESS;
}

bool CryptoUtils::SignFile(std::wstring const& zipPath, std::wstring const& privateKeyPath)
{
    std::vector<uint8_t> contents;
    std::vector<uint8_t> key;
    if (!LoadFile(zipPath, contents)) return false;
    if (!LoadFile(privateKeyPath, key)) return false;

    if (key.size() != NUM_ECC_BYTES) return false;

    PackageSignature sig;
    memset(&sig, 0, sizeof(sig));
    sig.Magic = PackageSignature::MAGIC_V1;
    sig.Version = PackageSignature::VER_SIGNATURE_IN_MANIFEST;
    sig.SignedBytes = (uint32_t)contents.size();

    if (!EccSign(contents.data(), contents.size(), key.data(), sig.EccSignature)) {
        return false;
    }

    auto contentLength = contents.size();
    contents.resize(contentLength + sizeof(sig));
    memcpy(contents.data() + contentLength, &sig, sizeof(sig));

    return SaveFile(zipPath, contents);
}

bool CryptoUtils::GenerateKeys(std::wstring const& privateKeyPath)
{
    uint8_t privateKey[NUM_ECC_BYTES];
    uint8_t publicKey[2*NUM_ECC_BYTES];
    if (!uECC_make_key(publicKey, privateKey, uECC_secp256r1())) {
        return false;
    }

    std::ofstream of(privateKeyPath.c_str(), std::ios::out | std::ios::binary);
    if (!of.good()) {
        return false;
    }

    of.write(reinterpret_cast<char*>(privateKey), sizeof(privateKey));

    std::cout << "Public key:" << std::endl;
    for (auto i = 0; i < sizeof(publicKey); i++) {
        std::cout << "0x" << std::hex << std::setfill('0') << std::setw(2) << (unsigned)publicKey[i] << ", ";
    }

    return true;
}

bool CryptoUtils::GetFileSignature(std::wstring const& path, PackageSignature& signature)
{
    std::vector<uint8_t> contents;
    if (!LoadFile(path, contents)) return false;

    if (contents.size() < sizeof(PackageSignature)) return false;

    auto sig = reinterpret_cast<PackageSignature*>(contents.data() + contents.size() - sizeof(PackageSignature));
    if (sig->Magic != PackageSignature::MAGIC_V1) return false;

    signature = *sig;
    return true;
}

bool CryptoUtils::VerifySignedFile(std::wstring const& zipPath, std::string& reason)
{
    std::vector<uint8_t> contents;
    if (!LoadFile(zipPath, contents)) {
        reason = "Unable to open update package";
        return false;
    }

    if (contents.size() < sizeof(PackageSignature)) {
        reason = "Update package not cryptographically signed";
        return false;
    }

    auto sig = reinterpret_cast<PackageSignature*>(contents.data() + contents.size() - sizeof(PackageSignature));
    if (sig->Magic != PackageSignature::MAGIC_V1) {
        reason = "Update package not cryptographically signed";
        return false;
    }

    if (!EccVerify(contents.data(), contents.size() - sizeof(PackageSignature), UpdaterPublicKey, sig->EccSignature)) {
        reason = "Cryptographic signature on update package is incorrect";
        return false;
    }

    return true;
}

END_SE()
