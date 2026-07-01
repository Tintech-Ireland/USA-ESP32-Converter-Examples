# Header-only component exposing the board pinout (board_pins.h).
idf_component_register(INCLUDE_DIRS "include"
                       REQUIRES driver)   # board_pins.h pulls in driver/gpio.h
