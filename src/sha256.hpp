#ifndef DAGE_SHA256_HPP
#define DAGE_SHA256_HPP

#include <string>

namespace dage {
std::string sha256_digest(const std::string& input);
}
#endif
