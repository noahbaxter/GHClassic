#pragma once

// Which of the GHClassic windows open reads the controllers: a file for the
// whole machine names it (takeControllers and leaveControllers in input.h).

namespace gh2::input
{
    // At launch: the first window up has them until another is focused; a
    // later one leaves them where they are.
    void claimControllers();
    // Whether this window reads them, looked at again every half second.
    bool ownsControllers();
}
