idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
