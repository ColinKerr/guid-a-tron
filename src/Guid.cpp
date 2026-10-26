/*---------------------------------------------------------------------------------------------
 * guid-a-tron — the guid. See Guid.h for what it has to agree with.
 *--------------------------------------------------------------------------------------------*/
#include "Guid.h"

#include "sqlite3.h"

#include <ctype.h>
#include <string.h>

namespace guidatron {

//---------------------------------------------------------------------------------------------
void Guid::Invalidate() { memset(b, 0, sizeof(b)); }

//---------------------------------------------------------------------------------------------
void Guid::Create() {
    sqlite3_randomness(sizeof(b), b);
    b[6] = (Byte)((b[6] & 0x0f) | 0x40); // version 4 (random)
    b[8] = (Byte)((b[8] & 0x3f) | 0x80); // RFC 4122 variant
}

//---------------------------------------------------------------------------------------------
bool Guid::IsValid() const {
    bool firstHalf = false, secondHalf = false;
    for (int i = 0; i < 8; ++i) {
        firstHalf = firstHalf || b[i] != 0;
        secondHalf = secondHalf || b[i + 8] != 0;
    }
    return firstHalf && secondHalf;
}

//---------------------------------------------------------------------------------------------
bool Guid::operator==(Guid const& rhs) const { return 0 == memcmp(b, rhs.b, sizeof(b)); }

//---------------------------------------------------------------------------------------------
// `BeGuid::operator<` compares the two 64-bit halves as little-endian integers on the platforms
// this runs on, which is not the same order as a byte-wise compare. Nothing here depends on the
// ordering — it exists so a Guid can be a map key — so this takes the byte-wise one, which is
// also the order the column's index uses.
//---------------------------------------------------------------------------------------------
bool Guid::operator<(Guid const& rhs) const { return memcmp(b, rhs.b, sizeof(b)) < 0; }

//---------------------------------------------------------------------------------------------
std::string Guid::ToString() const {
    char text[40];
    snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
             b[14], b[15]);
    return text;
}

//---------------------------------------------------------------------------------------------
bool Guid::FromString(char const* text) {
    Invalidate(); // so a rejected string leaves no half-parsed guid behind
    if (text == nullptr)
        return false;
    for (int i = 0; i < 36; ++i) {
        char c = text[i];
        bool hyphenHere = i == 8 || i == 13 || i == 18 || i == 23;
        if (hyphenHere ? c != '-' : 0 == isxdigit((unsigned char)c))
            return false;
    }
    if (text[36] != 0)
        return false;

    auto hexToInt = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    int at = 0;
    for (int i = 0; i < 16; ++i) {
        while (text[at] == '-')
            ++at;
        b[i] = (Byte)(16 * hexToInt(text[at]) + hexToInt(text[at + 1]));
        at += 2;
    }
    return true;
}

} // namespace guidatron
