#pragma once
#include <Arduino.h>

void ui_init();
void ui_frame();                                   // redraws only when something moved
void ui_key(uint8_t keycode, uint8_t modifier);    // HID usage + modifier byte
void ui_press(int x, int y);
void ui_drag(int x, int y);
void ui_release(int x, int y);
void ui_pinch(int phase, int x, int y, int dist, uint32_t ms);   // phase 0 start, 1 move, 2 end, 3 lift (panel coords)
void ui_dirty();
void ui_screenshot(Stream &out);                   // raw RGB565 frame, for checking layouts over serial
extern uint32_t ui_drawMs, ui_pushMs;   // last frame, for the serial log
bool kb_present();   // main.cpp: the keyboard on ExtPort1 answers
