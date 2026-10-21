#ifndef DEBOUNCED_BUTTON_H
#define DEBOUNCED_BUTTON_H

#include <stdint.h>

class DebouncedButton
{
public:
    explicit DebouncedButton(uint32_t debounce_ms) : debounce_ms(debounce_ms) {}

    // 起動・復旧時に押されたままなのは新しい押下ではない
    void reset(bool pressed, uint32_t now)
    {
        raw = stable = pressed;
        changed_at = now;
        eligible = false;
    }

    bool update(bool pressed, uint32_t now, bool accepts_press)
    {
        if (pressed != raw)
        {
            raw = pressed;
            changed_at = now;
            if (pressed) eligible = accepts_press;
        }
        if (pressed == stable || uint32_t(now - changed_at) < debounce_ms) return false;
        stable = pressed;
        return stable && eligible;
    }

private:
    const uint32_t debounce_ms;
    bool raw = false;
    bool stable = false;
    bool eligible = false;
    uint32_t changed_at = 0;
};

#endif
