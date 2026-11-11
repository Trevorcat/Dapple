#pragma once

#include <expected>
#include <string>

namespace dapple {

struct Error {
  std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(std::string message) {
  return std::unexpected<Error>(Error{std::move(message)});
}

}  // namespace dapple
