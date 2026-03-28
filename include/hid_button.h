#pragma once

enum ButtonState {
    BUTTON_NO_CHANGE = -1,
    BUTTON_LID_CLOSED = 21,
    BUTTON_BUTTON_PRESSED = 22,
    BUTTON_LID_OPEN = 23
};

bool init_button();
int poll_button();
bool is_button_pressed_edge();
const char* button_state_name(int state);
int button_last_state();
