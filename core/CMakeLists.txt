add_library(malgrind_core src/profiler.cpp)
target_include_directories(malgrind_core PUBLIC include)
target_compile_features(malgrind_core PUBLIC cxx_std_20)

malgrind_add_test(core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE malgrind_core)
