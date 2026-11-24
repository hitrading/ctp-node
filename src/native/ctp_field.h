/* Bounded conversion for fixed-width CTP char arrays received from outside. */
#ifndef CTP_NATIVE_CTP_FIELD_H
#define CTP_NATIVE_CTP_FIELD_H

#include <cstddef>
#include <string>

namespace ctp {

template <size_t N>
inline bool boundedCtpField(const char (&field)[N], std::string &out) {
  size_t length = 0;
  while (length < N && field[length] != '\0')
    ++length;
  if (length == N)
    return false;
  out.assign(field, length);
  return true;
}

// Some exchanges pad these fixed-width identifiers with ASCII spaces.
// Keep other bytes, including embedded spaces, significant.
template <std::size_t N>
bool boundedExchangeId(const char (&field)[N], std::string &out) {
  if (!boundedCtpField(field, out)) return false;
  const auto first = out.find_first_not_of(' ');
  if (first == std::string::npos) {
    out.clear();
  } else {
    out.erase(out.find_last_not_of(' ') + 1);
    out.erase(0, first);
  }
  return true;
}

} // namespace ctp

#endif /* CTP_NATIVE_CTP_FIELD_H */
