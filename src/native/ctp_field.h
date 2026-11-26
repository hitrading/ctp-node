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

} // namespace ctp

#endif /* CTP_NATIVE_CTP_FIELD_H */
