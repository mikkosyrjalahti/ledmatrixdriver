cmake_minimum_required(VERSION 3.12)

## Pico2 W ..
#set(PICO_PLATFORM rp2350)
#set(PICO_BOARD pico2_w)

## Pico W
set(PICO_PLATFORM rp2040)
set(PICO_BOARD pico_w)

set(CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -O3 -Wall -Werror -Wno-unused -Wno-unused-function -fstack-usage")

include(pico_sdk_import.cmake)
project(pico-dust)
pico_sdk_init()

aux_source_directory(src SRCS)
add_executable(main ${SRCS})

add_compile_definitions(NDEBUG)
add_compile_definitions(CYW43_HOST_NAME="ledpanel")

# this allows us to randomize every boot
#add_compile_definitions(CYW43_USE_OTP_MAC=0)

# this gets us a callback on WiFi activity?
add_compile_definitions(LWIP_ARP_FILTER_NETIF=1)
add_compile_definitions(LWIP_HOOK_FILENAME="dust-lwip.h")

# send DHCP replies without broadcast
# doesn't seem to really help anything
add_compile_definitions(ETHARP_SUPPORT_STATIC_ENTRIES=1)

# reserve a bit more than the default stack and heap
# we want to be safe with ~1kB HTTP client blocks
# and we want to be able to construct those on stack
add_compile_definitions(PICO_STACK_SIZE=0x1000) # 4kB
add_compile_definitions(PICO_HEAP_SIZE=0x6000) # 24kB

include_directories(src)

pico_enable_stdio_usb(main 0)
pico_enable_stdio_uart(main 0)

pico_add_extra_outputs(main)

target_link_libraries(main pico_stdlib pico_rand
    hardware_pio pico_flash pico_multicore
    pico_cyw43_arch_lwip_threadsafe_background)
