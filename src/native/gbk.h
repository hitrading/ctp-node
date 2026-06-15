/*
 * gbk.h - encode UTF-8 -> GB18030 (CTP's on-the-wire charset) for the rare
 * non-ASCII request field (e.g. bank-transfer customer names). Decoding is done
 * in JS via TextDecoder('gb18030'); this is the outbound counterpart.
 */

#ifndef CTP_NATIVE_GBK_H
#define CTP_NATIVE_GBK_H

#include <cstddef>
#include <string>

namespace ctp {
std::string gbkEncode(const char *utf8, size_t len);
}

#endif /* CTP_NATIVE_GBK_H */
