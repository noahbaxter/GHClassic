// MemInit (0x2ce508) makes a heap for each entry of the config's (mem
// (heaps ...)), which gh2.dta includes from config/mem.dta: main 19630000
// and rnd 5900000 on the disc. Each is one malloc, cut to the largest block
// libc has, and with ps2_clamp_32 (TRUE in the engine's default.dta) to one
// ending below 0x1f70000 (GetLargestFreeSysMem, 0x2ce888). The game's own keys come before default.dta's in
// the merge, so the flag set here is the one read.

#include "content/heaps.h"

#include "disc/ark.h"
#include "formats/dtb.h"

#include <iostream>

namespace gh2
{
    namespace
    {
        // libc's ceiling is the runtime arena's base, 111 MB of 128; the
        // executable and libc's other blocks need the rest. The pool is the
        // disc's.
        constexpr char kMem[] =
            "(heaps (main (size 56000000)) (rnd (size 28000000) (handle TRUE)))"
            " (pool (big_hunk 3100000) (small_hunk 50000))"
            " (ps2_clamp_32 FALSE)";
    }

    void installHeaps()
    {
        const auto mem = dtb::parse(kMem);
        if (!mem)
        {
            std::cerr << "[heaps] config did not parse" << std::endl;
            return;
        }
        ark::addFile("config/gen/mem.dtb", dtb::write(*mem));
    }
}
