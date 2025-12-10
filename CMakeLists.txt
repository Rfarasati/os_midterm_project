cmake_minimum_required(VERSION 4.1)
project(os_project_ C)

set(CMAKE_C_STANDARD 23)

# Add pthread and crypto libraries
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(c_engine c_engine.c
        c_engine.c
        test_basic.c
        test_basic.c)

target_link_libraries(c_engine
        Threads::Threads
        OpenSSL::Crypto
)