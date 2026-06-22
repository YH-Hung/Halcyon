cmake_minimum_required(VERSION 3.20)
project(halcyon_orders_sample CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Consumes Halcyon as an installed/exported package. Configure with:
#   -DCMAKE_PREFIX_PATH=<halcyon build-or-install tree>
#   -DDB2_CLIDRIVER_ROOT=<halcyon repo>/third_party/clidriver
# (the vendored-driver default root is relative to THIS project, so point it at
#  Halcyon's driver explicitly.)
find_package(Halcyon REQUIRED)

add_executable(orders_oo         src/orders_oo.cpp)
add_executable(orders_functional src/orders_functional.cpp)
target_link_libraries(orders_oo         PRIVATE halcyon::halcyon)
target_link_libraries(orders_functional PRIVATE halcyon::halcyon)
