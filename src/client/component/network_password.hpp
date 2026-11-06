#pragma once

#include <cstdint>
#include <string>

namespace network_password {
uint64_t hash_password(const std::string &password);
std::string get_password_hash_string();
bool is_password_set();
// Xerces: net_password itself, for the host's password check and the
// joiner's answer (xerces/join_password.hpp). Never logged or sent.
std::string get_password();
} // namespace network_password
