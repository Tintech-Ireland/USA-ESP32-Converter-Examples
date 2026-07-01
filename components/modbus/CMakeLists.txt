# Header-only component: Modbus RTU master over RS485 (modbus_rtu.h).
idf_component_register(INCLUDE_DIRS "include"
                       REQUIRES driver esp_timer)   # UART + esp_timer_get_time()
