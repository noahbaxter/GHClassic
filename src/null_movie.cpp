// GH2's full-screen movie player with nothing shown: every movie ends the
// moment it starts.

#include "null_movie.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace gh2
{
    namespace
    {
        // PlayMovie(name, ...), retail 0x21bb60. Retail opens the PSS stream,
        // sets up libmpeg, IPU and a libsdr audio channel, and loops decoding
        // until sceMpegIsEnd or a button press, then returns 1 (0x21bebc).
        // Its one caller, MetaPanel::OnPlayMovie (0x134c08), only passes that
        // result back. Nothing outlives the call, so skipping it leaves the
        // game where a finished movie would.
        void playMovie(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, 2, 1u);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void installNullMovie(PS2Runtime &runtime, const Addresses &addresses)
    {
        runtime.replaceFunction(addresses.playMovie, playMovie);
    }
}
