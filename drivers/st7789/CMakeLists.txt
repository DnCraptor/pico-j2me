# The PIO program of the SPI TFT driver (ST7789 / ILI9341), from pico-spec.
# The driver itself is platform/rp2350/video_tft.c.
add_library(st7789 INTERFACE)
target_link_libraries(st7789 INTERFACE hardware_pio hardware_clocks hardware_dma)
target_include_directories(st7789 INTERFACE ${CMAKE_CURRENT_LIST_DIR})
pico_generate_pio_header(st7789 ${CMAKE_CURRENT_LIST_DIR}/st7789.pio)
