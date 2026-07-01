idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES driver board)

# Pin the C++ standard to C++17 (project-wide cap).
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
