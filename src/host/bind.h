#pragma once

namespace gh2
{
    // ghrecomp --bind [keyboard | <n>]: walk each guitar action on one device
    // and write its section of input.ini. With no device, lists them.
    int runBind(int argc, char *argv[]);
}
