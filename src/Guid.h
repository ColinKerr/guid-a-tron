/*---------------------------------------------------------------------------------------------
 * guid-a-tron — a 16-byte guid, laid out and spelled exactly as iTwin lays out and spells one.
 *
 * This is the value that goes in `bis_Element.FederationGuid`, so it has to agree with `BeGuid`
 * (`BeSQLite/BeSQLite.h:202`) on three things, all of which are checked in the unit tests against
 * guids written by iTwin:
 *
 *   - **the bytes**: a guid is a 16-byte blob in the column, and nothing swaps field order on the
 *     way in or out, so byte 0 of the blob is the first hex pair of the string. (`BeGuid` reaches
 *     its bytes through a union over `_GUID`, but `ToString` and `FromString` both walk `b[0..15]`
 *     in order, so the layout is endian-free in practice.)
 *   - **the string**: lowercase 8-4-4-4-12.
 *   - **the version and variant bits**: `b[6]`'s high nibble and `b[8]`'s top two bits, set where
 *     a guid is generated rather than copied.
 *
 * `Create()` draws from `sqlite3_randomness`, which is the same source `BeGuid::Create` draws from.
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include "Types.h"

#include <string>

namespace guidatron {

struct Guid {
    Byte b[16];

    //! A zero guid, which means "none" — or a fresh random one.
    explicit Guid(bool createValue = false) {
        if (createValue)
            Create();
        else
            Invalidate();
    }

    void Invalidate();
    //! A random (version 4) guid. Draws from SQLite's PRNG, as iTwin's own does.
    void Create();

    /**
     * Zero means "no guid", which is what `BeGuid::IsValid` reports — and it reports it by testing
     * that **both** 64-bit halves are non-zero, not that the 16 bytes are not all zero. Reproduced
     * rather than corrected: a guid this tool wrote and one iTwin wrote have to be judged the same
     * way, and every guid here is a SHA-1 digest or 16 random bytes, so a zero half does not arise.
     */
    bool IsValid() const;

    bool operator==(Guid const& rhs) const;
    bool operator!=(Guid const& rhs) const { return !(*this == rhs); }
    bool operator<(Guid const& rhs) const;

    //! Lowercase 8-4-4-4-12.
    std::string ToString() const;
    //! Parse 8-4-4-4-12. False — and the guid invalidated — if `text` is not one.
    bool FromString(char const* text);
};

using GuidCR = Guid const&;

} // namespace guidatron
