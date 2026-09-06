cmake_minimum_required(VERSION 3.20)
project(cross_exchange_arbitrage_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(arbitrage_core src/core/fixed_point.cpp)
target_include_directories(arbitrage_core PUBLIC include)

add_executable(arbitrage_engine src/main.cpp)
target_link_libraries(arbitrage_engine PRIVATE arbitrage_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(fixed_point_test tests/unit/fixed_point_test.cpp)
    target_include_directories(fixed_point_test SYSTEM PRIVATE tests/vendor)
    target_link_libraries(fixed_point_test PRIVATE arbitrage_core)
    add_test(NAME fixed_point_test COMMAND fixed_point_test)
endif()
