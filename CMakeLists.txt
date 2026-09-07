cmake_minimum_required(VERSION 3.20)
project(cross_exchange_arbitrage_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(arbitrage_core src/core/fixed_point.cpp src/core/order_book.cpp)
target_include_directories(arbitrage_core PUBLIC include)

add_library(raw_recording
    src/recording/raw_event_recorder.cpp
    src/recording/raw_event_reader.cpp)
target_link_libraries(raw_recording PUBLIC arbitrage_core)
target_include_directories(raw_recording SYSTEM PRIVATE third_party)

add_library(coinbase_adapter
    src/adapters/coinbase/coinbase_l2_parser.cpp
    src/adapters/coinbase/coinbase_symbol_mapper.cpp)
target_link_libraries(coinbase_adapter PUBLIC arbitrage_core)
target_include_directories(coinbase_adapter SYSTEM PRIVATE third_party)

add_executable(arbitrage_engine src/main.cpp)
target_link_libraries(arbitrage_engine PRIVATE arbitrage_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(coinbase_l2_parser_test tests/unit/coinbase_l2_parser_test.cpp)
    target_include_directories(coinbase_l2_parser_test SYSTEM PRIVATE tests/vendor third_party)
    target_compile_definitions(coinbase_l2_parser_test PRIVATE
        FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/coinbase")
    target_link_libraries(coinbase_l2_parser_test PRIVATE coinbase_adapter)
    add_test(NAME coinbase_l2_parser_test COMMAND coinbase_l2_parser_test)
    add_executable(fixed_point_test tests/unit/fixed_point_test.cpp)
    target_include_directories(fixed_point_test SYSTEM PRIVATE tests/vendor)
    target_link_libraries(fixed_point_test PRIVATE arbitrage_core)
    add_test(NAME fixed_point_test COMMAND fixed_point_test)
    add_executable(market_event_test tests/unit/market_event_test.cpp)
    target_include_directories(market_event_test SYSTEM PRIVATE tests/vendor)
    target_link_libraries(market_event_test PRIVATE arbitrage_core)
    add_test(NAME market_event_test COMMAND market_event_test)
    add_executable(order_book_test tests/unit/order_book_test.cpp)
    target_include_directories(order_book_test SYSTEM PRIVATE tests/vendor)
    target_link_libraries(order_book_test PRIVATE arbitrage_core)
    add_test(NAME order_book_test COMMAND order_book_test)
    add_executable(raw_event_recorder_test tests/unit/raw_event_recorder_test.cpp)
    target_include_directories(raw_event_recorder_test SYSTEM PRIVATE tests/vendor third_party)
    target_compile_definitions(raw_event_recorder_test PRIVATE
        FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/coinbase")
    target_link_libraries(raw_event_recorder_test PRIVATE raw_recording coinbase_adapter)
    add_test(NAME raw_event_recorder_test COMMAND raw_event_recorder_test)
endif()
