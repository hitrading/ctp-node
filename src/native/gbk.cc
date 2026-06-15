/*
 * gbk.cc - see gbk.h
 */

#include "gbk.h"

#ifdef _WIN32
#include <windows.h>

namespace ctp {
std::string gbkEncode(const char *utf8, size_t len) {
  if (len == 0)
    return std::string();
  const int wlen =
      MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(len), nullptr, 0);
  if (wlen <= 0)
    return std::string(utf8, len);
  std::wstring wide(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(len), &wide[0], wlen);
  // 54936 = GB18030.
  const int blen = WideCharToMultiByte(54936, 0, wide.data(), wlen, nullptr, 0,
                                       nullptr, nullptr);
  if (blen <= 0)
    return std::string(utf8, len);
  std::string out(static_cast<size_t>(blen), '\0');
  WideCharToMultiByte(54936, 0, wide.data(), wlen, &out[0], blen, nullptr,
                      nullptr);
  return out;
}
} // namespace ctp

#else
#include <iconv.h>

namespace ctp {
std::string gbkEncode(const char *utf8, size_t len) {
  if (len == 0)
    return std::string();
  iconv_t cd = iconv_open("GB18030//TRANSLIT", "UTF-8");
  if (cd == reinterpret_cast<iconv_t>(-1))
    return std::string(utf8, len);
  size_t inleft = len;
  const size_t cap = len * 2 + 4;
  std::string out(cap, '\0');
  char *inbuf = const_cast<char *>(utf8);
  char *outbuf = &out[0];
  size_t outleft = cap;
  iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
  iconv_close(cd);
  out.resize(cap - outleft);
  return out;
}
} // namespace ctp
#endif
