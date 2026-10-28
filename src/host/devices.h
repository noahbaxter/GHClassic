#pragma once

// What the Controllers screen (ui/controllers.cpp) reads of every device and
// changes in input.ini. The host thread fills it each poll (input.cpp); each
// call is callable from any thread.

#include "host/bindings.h"
#include "host/device_id.h"

#include <array>
#include <string>
#include <vector>

namespace gh2::input
{
    // While the screen is up no device plays the game or takes a seat:
    // every control only shows there, and the keyboard runs it.
    void setControllersScreen(bool on);

    // A connected device as the screen shows it.
    struct DeviceView
    {
        DeviceId id;
        std::string label;       // its layout's name, else SDL's
        DeviceKind kind = DeviceKind::kController;
        int seat = -1;           // the seat it holds, -1 for none
        int number = 0;          // 1, 2... among devices of one model, 0 when alone
        std::array<float, kActionCount> amount{}; // each action now, 0 to 1
        std::array<std::vector<std::string>, kActionCount> bound{}; // each one's controls as a player calls them
    };

    // Every device, controllers in the order SDL lists them, the keyboard last.
    std::vector<DeviceView> deviceViews();
    // The device last pressed while the screen is up: the one it shows. The
    // keyboard runs the screen, so it is never pressed into it.
    DeviceId touchedDevice();
    void showDevice(const DeviceId &id);
    // Keys gone down while the screen is up, SDL scancodes, oldest first.
    std::vector<int> takeScreenKeys();
    // How long red alone has been held, unbroken, on the device shown, in
    // seconds: anything else held with it starts it over.
    float redHeldSeconds();

    // The next control moved on that device, or key for the keyboard, binds
    // the action, written to its section of input.ini and played from then.
    // A controller's whammy takes only an axis; the keyboard's is a key.
    // Escape is the keyboard's way out, so it never binds.
    void startRebind(const DeviceId &id, Action action);
    void cancelRebind();
    bool rebinding();
    // The action unbound in the device's section, or the section's own
    // lines dropped so its built-in layout and detected mode are all it
    // plays.
    void clearBinding(const DeviceId &id, Action action);
    void resetBindings(const DeviceId &id);
    // A controller's mode swapped, pad for guitar or back, in its section of
    // input.ini: how a seat it sits in plays (input.cpp). Its kind shows it.
    void swapMode(const DeviceId &id);
}
