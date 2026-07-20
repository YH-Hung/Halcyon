add_executable(halcyon_example_oo oo_usage.cpp)
target_link_libraries(halcyon_example_oo PRIVATE halcyon::halcyon)

add_executable(halcyon_example_functional functional_usage.cpp)
target_link_libraries(halcyon_example_functional PRIVATE halcyon::halcyon)

if(HALCYON_HAS_CXX20_COROUTINES)
    add_executable(halcyon_example_coro_orders coro_orders.cpp)
    target_link_libraries(halcyon_example_coro_orders PRIVATE halcyon::halcyon)
    target_compile_features(halcyon_example_coro_orders PRIVATE cxx_std_20)
else()
    message(STATUS "Halcyon: skipping coro_orders example (no C++20 coroutine support)")
endif()
