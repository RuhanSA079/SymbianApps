// ns_random.cpp: arc4random_buf for expat's hash salt (and anything else in
// NetSurf that wants secure random bytes), from the system random server.
#include <e32base.h>
#include <e32math.h>
#include <random.h>
#include "ns_symbian.h"

extern "C" void arc4random_buf(void *buf, size_t count)
{
    TPtr8 out(static_cast<TUint8 *>(buf), 0, count);
    out.SetLength(count);
    TRAPD(err, TRandom::SecureRandomL(out));
    if (err != KErrNone) {
        // Should not happen; fall back to the (weak) kernel PRNG rather
        // than leave the buffer unset.
        TUint8 *p = static_cast<TUint8 *>(buf);
        for (size_t i = 0; i < count; i++)
            p[i] = static_cast<TUint8>(Math::Random());
    }
}
