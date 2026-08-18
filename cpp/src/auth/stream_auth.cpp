#include "otts/auth/stream_auth.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace otts::auth {
namespace {

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

bool constant_time_equal(const std::string& lhs, const std::string& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        diff |= static_cast<unsigned char>(lhs[i] ^ rhs[i]);
    }
    return diff == 0;
}

std::string lower_hex(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool is_not_expired(const std::string& expires) {
    try {
        const auto expires_seconds = std::stoll(expires);
        const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return expires_seconds >= now_seconds;
    } catch (...) {
        return false;
    }
}

}  // namespace

std::string sign_stream(
    const std::string& action,
    const std::string& stream_key,
    const std::string& expires,
    const std::string& secret) {
    if (secret.empty() || action.empty() || stream_key.empty() || expires.empty()) {
        return {};
    }

    const auto payload = action + "\n" + stream_key + "\n" + expires;
    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int digest_len = 0;
    HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(payload.data()),
        payload.size(),
        digest,
        &digest_len);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i) {
        out << std::setw(2) << static_cast<int>(digest[i]);
    }
    return out.str();
}

bool is_authorized(
    const std::string& action,
    const std::string& stream_key,
    const std::string& supplied_token,
    const std::string& expires,
    const std::string& signature) {
    const auto token = env_value("OTTS_STREAM_TOKEN");
    const auto secret = env_value("OTTS_AUTH_SECRET");

    if (token.empty() && secret.empty()) {
        return true;
    }
    if (!token.empty() && supplied_token == token) {
        return true;
    }
    if (secret.empty() || action.empty() || stream_key.empty() || expires.empty() || signature.empty()) {
        return false;
    }
    if (!is_not_expired(expires)) {
        return false;
    }

    const auto expected = sign_stream(action, stream_key, expires, secret);
    return !expected.empty() && constant_time_equal(expected, lower_hex(signature));
}

}  // namespace otts::auth
