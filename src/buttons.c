#include <stdbool.h>
#include "gpio.h"
#include "buttons.h"

#define BTN_LEFT_MODE_PU  (1 << 19)
#define BTN_UP_MODE_PU    (1 << 23)
#define BTN_DOWN_MODE_PU  (1 << 27)
#define BTN_RIGHT_MODE_PU (1 << 31)

#define BTN_A_MODE_PU     (1 << 3)
#define BTN_B_MODE_PU     (1 << 7)

#define BTN_LEFT_MODE_FI  (1 << 18)
#define BTN_UP_MODE_FI    (1 << 22)
#define BTN_DOWN_MODE_FI  (1 << 26)
#define BTN_RIGHT_MODE_FI (1 << 30)

#define BTN_A_MODE_FI     (1 << 2)
#define BTN_B_MODE_FI     (1 << 6)


void buttons_init(void) {
    // Clear floating input bits (which are set to 1 at reset)
    GPIOB_CRL &= ~(BTN_LEFT_MODE_FI | BTN_UP_MODE_FI | BTN_DOWN_MODE_FI | BTN_RIGHT_MODE_FI);
    GPIOB_CRH &= ~(BTN_A_MODE_FI | BTN_B_MODE_FI);

    // Set inputs as internal pull-up
    GPIOB_CRL |= (BTN_LEFT_MODE_PU | BTN_UP_MODE_PU | BTN_DOWN_MODE_PU | BTN_RIGHT_MODE_PU);
    GPIOB_CRH |= (BTN_A_MODE_PU | BTN_B_MODE_PU);

    GPIOB_ODR |= 0x3F0; // Activate pull-up resistor
}

bool btn_pressed(enum Button btn) {
    return !(GPIOB_IDR & (1 << btn)); // Active-low logic
}

bool btn_released(enum Button btn) {
    // Keep track when each button was last pressed
    static bool btn_was_pressed[NUM_BUTTONS];

    // Figure out which switch we are dealing with for ease of use
    bool *was_pressed = &btn_was_pressed[btn - BTN_LEFT];

    /* If the switch is not currently pressed but was pressed recently,
       we know it has just been released */
    if (!btn_pressed(btn) && *was_pressed) {
        *was_pressed = false;
        return true;
    } else if (btn_pressed(btn)) {
        *was_pressed = true;
    }

    return false;
}
