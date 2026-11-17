#pragma once
// R36S gamepad -> guest input, PlayStation 2 layout (by position: Cross = bottom, Circle = right,
// Square = left, Triangle = top; on the R36S those are the buttons labelled B, A, Y, X).
//
// Buttons always reach the game as its PC keyboard keys (the game's own default PC key bindings), never
// as joystick buttons: the game's joystick bindings do not include pause/reset, and sending both made
// every press arrive twice. The joystick only carries the analog steering (left stick X) while driving,
// so steering stays proportional. Keys are recomputed from the whole pad state on every event, so a
// button and the d-pad that both mean "left" never release each other.
//
// Driving (NFSU2 PS2 defaults): Cross accelerate, Square brake/reverse, Circle e-brake, Triangle camera,
//   R1 nitrous, L1 look back, R2/L2 shift up/down (manual gearbox), Start pause, Select reset car,
//   left stick steer. D-pad as on the consoles: Up engage event / enter shop (Enter), Down career status (1),
//   Left world map (M), Right SMS (Tab).
// Menus: d-pad / left stick move, Cross or Start accept, Circle or Triangle back, Square = Space.
#include <SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>

class SdlGamepad {
public:
    enum Button : unsigned {A,B,X,Y,L1,R1,L2,R2,Start,Select,ThumbL,ThumbR,Up,Down,Left,Right,ButtonCount};

    static int buttonFor(uint8_t b) {
        switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return A;            // south (R36S: physical B)
        case SDL_CONTROLLER_BUTTON_B: return B;            // east  (R36S: physical A)
        case SDL_CONTROLLER_BUTTON_X: return X;            // west  (R36S: physical Y)
        case SDL_CONTROLLER_BUTTON_Y: return Y;            // north (R36S: physical X)
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return L1;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return R1;
        case SDL_CONTROLLER_BUTTON_START: return Start;
        case SDL_CONTROLLER_BUTTON_BACK: return Select;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return ThumbL;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return ThumbR;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Down;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Right;
        default: return -1;
        }
    }
    template<class SetKey> bool button(uint8_t sdlButton, bool down, bool driving, SetKey&& setKey) {
        const int b = buttonFor(sdlButton);
        if (b < 0) return false;
        buttons_[b] = down;
        apply(driving, setKey);
        return true;
    }
    template<class SetKey> void axis(uint8_t sdlAxis, int16_t raw, bool driving, SetKey&& setKey) {
        const float v = raw < 0 ? raw / 32768.0f : raw / 32767.0f;
        switch (sdlAxis) {
        case SDL_CONTROLLER_AXIS_LEFTX: stickX_ = dead(v); break;
        case SDL_CONTROLLER_AXIS_LEFTY: stickY_ = dead(v); break;
        case SDL_CONTROLLER_AXIS_RIGHTX: rightX_ = dead(v); break;
        case SDL_CONTROLLER_AXIS_RIGHTY: rightY_ = dead(v); break;
        // R36S L2/R2 are digital buttons that SDL reports as a trigger axis (0 or 32767).
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: leftTrigger_ = v > 0.05f ? v : 0.0f; break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: rightTrigger_ = v > 0.05f ? v : 0.0f; break;
        default: return;
        }
        apply(driving, setKey);
    }
    // Returns true when the driving/menu mode changed (the joystick state must be republished).
    template<class SetKey> bool refresh(bool driving, SetKey&& setKey) {
        if (driving == driving_) return false;
        apply(driving, setKey);
        return true;
    }
    template<class SetKey> void releaseAll(SetKey&& setKey) {
        buttons_.fill(false);
        stickX_ = stickY_ = rightX_ = rightY_ = leftTrigger_ = rightTrigger_ = dpadX_ = 0;
        apply(driving_, setKey);
    }
    bool held(Button b) const { return buttons_[b]; }

    // The d-pad's X value moves toward full lock in FO2_DPAD_RAMP_MS (150 by default; 0: at once) and back to the centre
    // twice as fast. Called every main-loop pass; true when the value changed (the pad state must be republished).
    bool tickDpad() {
        static const float rampMs = [] { const char* e = std::getenv("FO2_DPAD_RAMP_MS"); return e ? float(std::fmax(std::atof(e), 0.0)) : 150.0f; }();
        const auto now = std::chrono::steady_clock::now();
        const float ms = dpadLast_.time_since_epoch().count() ? std::chrono::duration<float, std::milli>(now - dpadLast_).count() : 0.0f;
        dpadLast_ = now;
        const float target = (buttons_[Right] ? 1.0f : 0.0f) - (buttons_[Left] ? 1.0f : 0.0f);
        if (dpadX_ == target) return false;
        if (rampMs <= 0.0f) { dpadX_ = target; return true; }
        const bool outward = target != 0.0f && dpadX_ * target >= 0.0f;   // pushing further, not releasing or reversing
        const float step = std::fmin(ms, 50.0f) / (outward ? rampMs : rampMs * 0.5f);
        dpadX_ = dpadX_ < target ? std::fmin(dpadX_ + step, target) : std::fmax(dpadX_ - step, target);
        return true;
    }

    // The DirectInput joystick, laid out as the Xbox 360 controller the runtime announces (045E:028E): X/Y
    // left stick, Z both triggers (left +, right -), X/Y rotation right stick, hat = d-pad, buttons 0-9 =
    // A B X Y LB RB Back Start LS RS. Only seen by the game with FO2_DIRECTINPUT_PAD=1 (see apply).
    void padState(float axes[6], uint32_t& buttons, int& hatX, int& hatY) const {
        // The d-pad also moves the left stick when the stick rests: FlatOut 2's steering is bound to its X axis in pad
        // mode (Savegame/am<guid>.dat), and consoles without a stick (GKD Pixel II) steer with the d-pad. Its X value
        // ramps (tickDpad) as a thumb would push a stick, as the game's own keyboard steering did.
        // In races only: in the menus the d-pad already sends arrows.
        const float dpadY = driving_ ? (buttons_[Down] ? 1.0f : 0.0f) - (buttons_[Up] ? 1.0f : 0.0f) : 0.0f;
        axes[0] = std::fabs(stickX_) > 0.05f ? stickX_ : (driving_ ? dpadX_ : 0.0f);
        axes[1] = std::fabs(stickY_) > 0.05f ? stickY_ : dpadY;
        axes[2] = std::max(leftTrigger_, buttons_[L2] ? 1.0f : 0.0f) - std::max(rightTrigger_, buttons_[R2] ? 1.0f : 0.0f);
        axes[3] = rightX_; axes[4] = rightY_; axes[5] = 0;
        static const Button order[10] = {A, B, X, Y, L1, R1, Select, Start, ThumbL, ThumbR};
        buttons = 0;
        for (unsigned i = 0; i < 10; ++i) if (buttons_[order[i]]) buttons |= 1u << i;
        hatX = (buttons_[Right] ? 1 : 0) - (buttons_[Left] ? 1 : 0);
        hatY = (buttons_[Down] ? 1 : 0) - (buttons_[Up] ? 1 : 0);
    }

private:
    std::array<bool, ButtonCount> buttons_{};
    float stickX_ = 0, stickY_ = 0, rightX_ = 0, rightY_ = 0, leftTrigger_ = 0, rightTrigger_ = 0;
    float dpadX_ = 0;
    std::chrono::steady_clock::time_point dpadLast_{};
    std::array<bool, 256> sent_{};
    bool driving_ = false;

    // Stick dead zone, rescaled so the output starts at 0 past it (no jump). Cheap handheld sticks rest off-centre and
    // rarely reach their edge: FO2_STICK_DEADZONE (percent, 15 by default) at the centre, full output from 95 %.
    static float dead(float v) {
        static const float zone = [] { const char* e = std::getenv("FO2_STICK_DEADZONE"); const float p = e ? float(std::atof(e)) : 15.0f; return std::fmin(std::fmax(p, 0.0f), 80.0f) / 100.0f; }();
        const float a = std::fabs(v);
        if (a <= zone) return 0.0f;
        const float out = std::fmin((a - zone) / (0.95f - zone), 1.0f);
        return v < 0 ? -out : out;
    }

    // FlatOut 2 is played on its PC keyboard controls (the game is not offered the pad, see guest_input.inc),
    // laid out like an Xbox pad. The buttons are named by position (SDL): on the RG34XX-SP the bottom one (A
    // here) is labelled B, the right one (B here) A, the left one (X) Y and the top one (Y) X. `driving` is
    // fo2InRace() (the physics ran in the last 300 ms; a race's pause menu counts as a menu).
    //   menus: d-pad / left stick = arrows, bottom = Enter (select), right = Escape (back), Start = Escape
    //   race:  bottom = Up (accelerate) + Enter, left = Down (brake / reverse), right = Space (handbrake),
    //          d-pad / stick left-right = steer, R1 = Left Ctrl (nitro), top = R (reset car),
    //          Select = C (camera), Start = Escape (pause), R2 / L2 = A / Z (manual gearbox up / down)
    // NFSU2's mapping further down is no longer used.
    // FO2_PAD_KEYS=0: no keys at all (the game then only has the pad itself, see FO2_DIRECTINPUT_PAD).
    static bool padOffered() { static const bool on = [] { const char* e = std::getenv("FO2_DIRECTINPUT_PAD"); return !(e && e[0] == '0'); }(); return on; }
    static bool fo2Keys() { static const bool on = [] { const char* e = std::getenv("FO2_PAD_KEYS"); return !(e && e[0] == '0'); }(); return on; }
    template<class SetKey> void apply(bool driving, SetKey& setKey) {
        std::array<bool, 256> want{};
        if (!fo2Keys()) { driving_ = driving; return; }
        {
            auto past = [&](float value, unsigned scan) { return value > (sent_[scan] ? .25f : .4f); };
            // With the pad offered to the game (FO2_DIRECTINPUT_PAD=1, the game's Controller set to it), the left stick
            // steers through the joystick's X axis, proportionally: in a race it no longer sends the arrows, which steer
            // at full lock and took over. The d-pad keeps them, for consoles without a stick.
            const bool padSteer = driving && padOffered();
            const float steerX = padSteer ? 0.0f : stickX_;
            want[0xcb] = (!padSteer && buttons_[Left]) || past(-steerX, 0xcb);
            want[0xcd] = (!padSteer && buttons_[Right]) || past(steerX, 0xcd);
            want[0x01] = buttons_[Start];
            if (driving) {
                // The physics keeps running on the results screen after the finish (the cars drive on), so
                // that screen looks like a race here: bottom also sends Enter (nothing while driving) and the
                // d-pad's up / down the arrows, so it can be validated and browsed.
                want[0xc8] = buttons_[A] || buttons_[Up];
                want[0xd0] = buttons_[X] || buttons_[Down];
                want[0x1c] = buttons_[A];
                want[0x39] = buttons_[B];
                want[0x1d] = buttons_[R1];
                want[0x13] = buttons_[Y];
                want[0x2e] = buttons_[Select];
                want[0x1e] = buttons_[R2] || past(rightTrigger_, 0x1e);
                want[0x2c] = buttons_[L2] || past(leftTrigger_, 0x2c);
            } else {
                want[0xc8] = buttons_[Up] || past(-stickY_, 0xc8);
                want[0xd0] = buttons_[Down] || past(stickY_, 0xd0);
                want[0x1c] = buttons_[A];
                want[0x01] = want[0x01] || buttons_[B];
            }
            driving_ = driving;
            for (unsigned scan = 0; scan < 256; ++scan)
                if (want[scan] != sent_[scan]) { sent_[scan] = want[scan]; setKey(scan, want[scan]); }
            return;
        }
        // Hysteresis keeps a stick resting near the threshold from chattering.
        auto past = [&](float value, unsigned scan) { return value > (sent_[scan] ? .25f : .4f); };
        if (driving) {
            // PC default keys: Up/Down throttle/brake, Space e-brake, Left Alt nitrous, C camera,
            // B look back, Left Shift / Left Ctrl shift up/down, Escape pause, R reset; free-roam actions
            // Enter engage event / shop, 1 career status, M world map, Tab SMS (on the d-pad, as on the consoles).
            // The stick steers through the joystick (proportional).
            want[0xc8] = buttons_[A];
            want[0xd0] = buttons_[X];
            want[0x1c] = buttons_[Up];
            want[0x02] = buttons_[Down];
            want[0x32] = buttons_[Left];
            want[0x0f] = buttons_[Right];
            want[0x39] = buttons_[B];
            want[0x2e] = buttons_[Y];
            want[0x38] = buttons_[R1];
            want[0x30] = buttons_[L1];
            want[0x2a] = buttons_[R2] || past(rightTrigger_, 0x2a);
            want[0x1d] = buttons_[L2] || past(leftTrigger_, 0x1d);
            want[0x01] = buttons_[Start];
            want[0x13] = buttons_[Select];
        } else {
            want[0xcb] = buttons_[Left] || past(-stickX_, 0xcb); want[0xcd] = buttons_[Right] || past(stickX_, 0xcd);
            want[0xc8] = buttons_[Up] || past(-stickY_, 0xc8); want[0xd0] = buttons_[Down] || past(stickY_, 0xd0);
            want[0x1c] = buttons_[A] || buttons_[Start];
            want[0x01] = buttons_[B] || buttons_[Y];
            want[0x39] = buttons_[X];
        }
        driving_ = driving;
        for (unsigned scan = 0; scan < 256; ++scan)
            if (want[scan] != sent_[scan]) { sent_[scan] = want[scan]; setKey(scan, want[scan]); }
    }
};
