#include "host/profiles.h"

#include <initializer_list>

namespace gh2::input
{
    namespace
    {
        Source key(SDL_Scancode code)
        {
            return {Source::kKey, code};
        }

        Source button(int index)
        {
            return {Source::kButton, index};
        }

        Source axis(int index, float rest, float full)
        {
            return {Source::kAxis, index, 0, rest, full};
        }

        Source padButton(SDL_GamepadButton b)
        {
            return {Source::kPadButton, b};
        }

        Source padAxis(SDL_GamepadAxis a, float rest, float full)
        {
            return {Source::kPadAxis, a, 0, rest, full};
        }

        Profile make(std::initializer_list<std::pair<Action, std::vector<Source>>> bindings)
        {
            Profile p;
            for (const auto &[action, sources] : bindings)
                p[action] = sources;
            return p;
        }

        // A Wii guitar through a raphnet WUSBMote v2.2, as bound on real
        // hardware for ghpc. The adapter has no tilt sensor.
        Profile raphnetWiiGuitar()
        {
            return make({
                {kGreen, {button(0)}},
                {kRed, {button(1)}},
                {kYellow, {button(2)}},
                {kBlue, {button(3)}},
                {kOrange, {button(4)}},
                {kStrumUp, {button(8)}},
                {kStrumDown, {button(5)}},
                {kStart, {button(6)}},
                {kStarPower, {button(7)}},
                {kWhammy, {axis(2, -0.11f, 0.70f)}},
            });
        }

        // GH2's own joypad layout (config/beatmatch_controller.dta, joypad):
        // frets L2 L1 R1 R2 X, star power on Select, and whammy on the left
        // stick pushed up, which is where GuitarController::GetWhammyBar
        // (0x236718) reads it with whammy_bar_style ps2.
        Profile joypadLayout()
        {
            return make({
                {kGreen, {padAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0f, 1.0f)}},
                {kRed, {padButton(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)}},
                {kYellow, {padButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)}},
                {kBlue, {padAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.0f, 1.0f)}},
                {kOrange, {padButton(SDL_GAMEPAD_BUTTON_SOUTH)}},
                {kStrumUp, {padButton(SDL_GAMEPAD_BUTTON_DPAD_UP)}},
                {kStrumDown, {padButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN)}},
                {kStart, {padButton(SDL_GAMEPAD_BUTTON_START)}},
                {kStarPower, {padButton(SDL_GAMEPAD_BUTTON_BACK)}},
                {kWhammy, {padAxis(SDL_GAMEPAD_AXIS_LEFTY, 0.0f, -1.0f)}},
            });
        }

        // The layout SDL gives most guitars it drives as gamepads: Xbox 360
        // (XInput), PS3 and Wii Rock Band, PS4/PS5 and Xbox One RB4 (SDL
        // 3.4.16's hidapi drivers; PlasticBand's report structs). Solo frets
        // also set the main fret, so they play as it. RIGHT_SHOULDER is the
        // RB tilt or the GH star power pedal, and XInput guitars tilt on RIGHTY,
        // which SDL inverts: neck up is negative. Whammy rests at one end of
        // RIGHTX, or near the middle on a few families.
        Profile sdlGuitar(float whammyRest, bool yellowBlueSwapped)
        {
            return make({
                {kGreen, {padButton(SDL_GAMEPAD_BUTTON_SOUTH)}},
                {kRed, {padButton(SDL_GAMEPAD_BUTTON_EAST)}},
                {kYellow, {padButton(yellowBlueSwapped ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH)}},
                {kBlue, {padButton(yellowBlueSwapped ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST)}},
                {kOrange, {padButton(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)}},
                {kStrumUp, {padButton(SDL_GAMEPAD_BUTTON_DPAD_UP)}},
                {kStrumDown, {padButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN)}},
                {kStart, {padButton(SDL_GAMEPAD_BUTTON_START)}},
                {kStarPower, {padButton(SDL_GAMEPAD_BUTTON_BACK)}},
                {kTilt,
                 {padButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), padAxis(SDL_GAMEPAD_AXIS_RIGHTY, 0.0f, -1.0f)}},
                {kWhammy, {padAxis(SDL_GAMEPAD_AXIS_RIGHTX, whammyRest, 1.0f)}},
            });
        }

        // The PC/Mac Guitar Hero World Tour guitar, which SDL reads as a plain
        // HID joystick (PlasticBand's PS3 descriptor, in the order macOS sorts
        // HID usages). The tilt sensor is on a vendor page SDL drops.
        Profile pcWorldTourGuitar()
        {
            return make({
                {kGreen, {button(1)}},
                {kRed, {button(2)}},
                {kYellow, {button(0)}},
                {kBlue, {button(3)}},
                {kOrange, {button(4)}},
                {kStrumUp, {Source{Source::kHat, 0, SDL_HAT_UP}}},
                {kStrumDown, {Source{Source::kHat, 0, SDL_HAT_DOWN}}},
                {kStart, {button(9)}},
                {kStarPower, {button(8), button(5)}},
                {kWhammy, {axis(2, 0.0f, 1.0f)}},
            });
        }

        struct Known
        {
            uint16_t vendor;
            uint16_t product;
            const char *label;
            Profile (*profile)();
        };

        Profile ps3GuitarHero() { return sdlGuitar(0.0f, true); }
        Profile rockBandDrifting() { return sdlGuitar(0.0f, false); } // PS3/Wii RB whammy settles mid-range when idle
        Profile sdlGuitarDefault() { return sdlGuitar(-1.0f, false); }

        const Known kKnown[] = {
            {0x12ba, 0x0100, "PS3 Guitar Hero guitar", ps3GuitarHero},
            {0x12ba, 0x0200, "PS3 Rock Band guitar", rockBandDrifting},
            {0x1bad, 0x0004, "Wii Rock Band guitar", rockBandDrifting},
            {0x1bad, 0x3010, "Wii Rock Band 2 guitar", rockBandDrifting},
            {0x1430, 0x474c, "PC/Mac Guitar Hero World Tour guitar", pcWorldTourGuitar},
            {0x289b, 0x0028, "Wii guitar (raphnet)", raphnetWiiGuitar},
            {0x289b, 0x002b, "Wii guitar (raphnet)", raphnetWiiGuitar},
            {0x289b, 0x0080, "Wii guitar (raphnet)", raphnetWiiGuitar},
            // Xbox 360 guitars. macOS needs an SDL built with libusb to see them.
            {0x1430, 0x4748, "Xbox 360 Guitar Hero guitar", sdlGuitarDefault},
            {0x1430, 0x0705, "Xbox 360 Guitar Hero guitar", sdlGuitarDefault},
            {0x1430, 0x4734, "Xbox 360 Guitar Hero guitar", sdlGuitarDefault},
            {0x1430, 0x0706, "Xbox 360 Guitar Hero guitar", sdlGuitarDefault},
            {0x1bad, 0x0002, "Xbox 360 Rock Band guitar", sdlGuitarDefault},
            {0x0738, 0x9806, "Xbox 360 Rock Band guitar", sdlGuitarDefault},
            // Rock Band 4 guitars (Xbox One wired ones are Windows/Linux only).
            {0x0738, 0x4161, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x0e6f, 0x0170, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x0e6f, 0x0248, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x3651, 0x4161, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x3958, 0x4161, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x0738, 0x8261, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x0e6f, 0x0173, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x0e6f, 0x024a, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x3651, 0x1500, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x3651, 0x5500, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x3958, 0x5500, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x0e6f, 0x0249, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x3651, 0x1600, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x3651, 0x5600, "Rock Band 4 guitar", sdlGuitarDefault},
            {0x3958, 0x5600, "Rock Band 4 guitar", sdlGuitarDefault},
        };

        // Six-fret Guitar Hero Live guitars have no five-fret layout to give.
        bool isGuitarHeroLive(uint16_t vendor, uint16_t product)
        {
            return (vendor == 0x12ba && product == 0x074b) || (vendor == 0x1430 && product == 0x070b) ||
                   (vendor == 0x1430 && product == 0x07bb) || (vendor == 0x1430 && product == 0x079b);
        }
    }

    Profile keyboardProfile()
    {
        return make({
            {kGreen, {key(SDL_SCANCODE_1)}},
            {kRed, {key(SDL_SCANCODE_2)}},
            {kYellow, {key(SDL_SCANCODE_3)}},
            {kBlue, {key(SDL_SCANCODE_4)}},
            {kOrange, {key(SDL_SCANCODE_5)}},
            {kStrumUp, {key(SDL_SCANCODE_UP)}},
            {kStrumDown, {key(SDL_SCANCODE_DOWN)}},
            {kStart, {key(SDL_SCANCODE_RETURN)}},
            {kStarPower, {key(SDL_SCANCODE_RSHIFT)}},
        });
    }

    std::optional<BuiltinProfile> builtinProfile(SDL_Joystick *joystick, SDL_Gamepad *gamepad)
    {
        const uint16_t vendor = SDL_GetJoystickVendor(joystick);
        const uint16_t product = SDL_GetJoystickProduct(joystick);
        for (const Known &k : kKnown)
            if (k.vendor == vendor && k.product == product)
                return BuiltinProfile{k.label, k.profile()};
        if (isGuitarHeroLive(vendor, product))
            return std::nullopt;
        // A guitar SDL recognises that is not listed above, Santroller builds
        // in a console mode among them.
        if (SDL_GetJoystickType(joystick) == SDL_JOYSTICK_TYPE_GUITAR)
            return BuiltinProfile{"guitar", sdlGuitarDefault()};
        if (gamepad)
            return BuiltinProfile{"gamepad, GH2's joypad layout", joypadLayout()};
        return std::nullopt;
    }
}
