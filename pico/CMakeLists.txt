cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
 
project(prog C CXX ASM)
 
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
 
pico_sdk_init()

add_executable(${PROJECT_NAME}
    main.c
    avrprog.c
    firmware_image.c
    firmware_storage.c
    usb_descriptors.c
)
pico_add_extra_outputs(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR})

set_source_files_properties(
    main.c
    avrprog.c
    firmware_image.c
    firmware_storage.c
    usb_descriptors.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror"
)

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    pico_flash
    pico_unique_id
    hardware_flash
    hardware_spi
    hardware_watchdog
    tinyusb_device
)

pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 1)
