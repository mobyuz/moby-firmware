// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_PINS_H
#define MOBY_PINS_H

#include "driver/gpio.h"

#define MOBY_PIN_TALK GPIO_NUM_7
#define MOBY_PIN_MUTE GPIO_NUM_10
#define MOBY_PIN_RED_LED GPIO_NUM_21

// INMP441 on I2S0: SCK/BCLK, WS and SD (data in, through the hardware mute switch).
#define MOBY_PIN_I2S_BCLK GPIO_NUM_4
#define MOBY_PIN_I2S_WS GPIO_NUM_5
#define MOBY_PIN_I2S_DIN GPIO_NUM_6

#endif
