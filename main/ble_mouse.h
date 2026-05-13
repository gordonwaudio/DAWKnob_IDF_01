#pragma once

#include <stdbool.h>
#include <stdint.h>

// Button bitmasks (HID standard: bit0=left, bit1=right, bit2=middle)
#define BLE_MOUSE_LEFT   0x01
#define BLE_MOUSE_RIGHT  0x02
#define BLE_MOUSE_MIDDLE 0x04

void ble_mouse_init(void);
void ble_mouse_deinit(void);
bool ble_mouse_is_connected(void);
void ble_mouse_press(uint8_t buttons);
void ble_mouse_release(void);
void ble_mouse_move(int8_t x, int8_t y);
