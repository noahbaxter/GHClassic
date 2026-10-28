#include "host/owner.h"

#include "host/input.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <unistd.h>
#endif

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace gh2
{
    namespace
    {
        // Whether this instance reads the controllers, from the owner file,
        // looked at every half second. The host thread's own.
        bool s_owner = true;
        uint64_t s_ownerCheckedMs = 0;
        bool s_leave = false; // a scripted run's

        // One file for the whole machine, whatever --data each run was
        // given: the controllers are the machine's.
        std::filesystem::path ownerPath()
        {
            std::error_code error;
            const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
            return (error ? std::filesystem::path(".") : temp) / "GHClassic.controllers";
        }

        unsigned long processId()
        {
#ifdef _WIN32
            return GetCurrentProcessId();
#else
            return static_cast<unsigned long>(getpid());
#endif
        }

        bool running(unsigned long pid)
        {
#ifdef _WIN32
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
            if (!process)
                return false;
            const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            CloseHandle(process);
            return alive;
#else
            return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
        }

        // Another running GHClassic that owns the controllers. None when
        // there is no file or its owner has quit, which leaves them to every
        // window until one takes them.
        bool otherOwner()
        {
            std::ifstream file(ownerPath());
            unsigned long pid = 0;
            return (file >> pid) && pid != processId() && running(pid);
        }

        void setOwner(bool owner)
        {
            if (owner != s_owner)
                std::cerr << "[input] controllers: " << (owner ? "this window" : "another window has them") << std::endl;
            s_owner = owner;
            s_ownerCheckedMs = SDL_GetTicks();
        }
    }

    void leaveControllers()
    {
        s_leave = true;
    }

    void takeControllers()
    {
        if (s_leave)
            return;
        std::ofstream file(ownerPath(), std::ios::trunc);
        file << processId() << '\n';
        if (!file)
            std::cerr << "[input] could not write " << ownerPath().string() << std::endl;
        setOwner(true);
    }

    namespace input
    {
        void claimControllers()
        {
            if (otherOwner())
                setOwner(false);
            else
                takeControllers();
        }

        bool ownsControllers()
        {
            if (SDL_GetTicks() - s_ownerCheckedMs >= 500u)
                setOwner(!otherOwner());
            return s_owner;
        }
    }
}
