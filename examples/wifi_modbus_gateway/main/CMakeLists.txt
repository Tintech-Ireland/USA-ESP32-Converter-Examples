idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES driver board modbus esp_wifi esp_netif esp_event nvs_flash lwip)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
