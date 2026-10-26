/*---------------------------------------------------------------------------------------------
 * guid-a-tron — the two spellings every file here shares.
 *
 * `Byte` because the changeset format is defined in bytes and `unsigned char` reads badly in that
 * context; `Format` because three places need a printf into a std::string and C++20 has no
 * std::format in every toolchain this builds on.
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

namespace guidatron {

using Byte = uint8_t;

//! printf into a std::string. Truncates at 4 KB, which every caller here is far inside.
inline std::string Format(char const* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return written < 0 ? std::string() : std::string(buffer);
}

//! A string's length including its nul terminator, which is what a changeset's prefix length is
//! measured in (`ChangesetFile.cpp:WritePrefix` writes `Utf8String::SizeInBytes`).
inline uint32_t SizeWithNul(std::string const& text) { return (uint32_t)text.size() + 1; }

} // namespace guidatron
