#pragma once

#include <string>

namespace otts::auth {

bool is_authorized(
    const std::string& action,
    const std::string& stream_key,
    const std::string& supplied_token,
    const std::string& expires,
    const std::string& signature);

std::string sign_stream(
    const std::string& action,
    const std::string& stream_key,
    const std::string& expires,
    const std::string& secret);

}  // namespace otts::auth
