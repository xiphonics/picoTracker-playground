# ---------------------------------------------------------------
# common_pico — shared platform code for VGA (original) targets
# ---------------------------------------------------------------
add_library(common_pico INTERFACE)
target_sources(common_pico INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/i_glob.c
        ${CMAKE_CURRENT_LIST_DIR}/i_input.c
        ${CMAKE_CURRENT_LIST_DIR}/i_system.c
        ${CMAKE_CURRENT_LIST_DIR}/piconet.c
        ${CMAKE_CURRENT_LIST_DIR}/i_timer.c
        ${CMAKE_CURRENT_LIST_DIR}/i_video.c
        ${CMAKE_CURRENT_LIST_DIR}/stubs.c
        ${CMAKE_CURRENT_LIST_DIR}/i_picosound.c
)
if (PICO_ON_DEVICE)
    target_sources(common_pico INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/blit.S
            ${CMAKE_CURRENT_LIST_DIR}/picoflash.c
            )
    pico_wrap_function(common_pico malloc)
    pico_wrap_function(common_pico calloc)
    pico_wrap_function(common_pico free)
    target_compile_definitions(common_pico INTERFACE
            PICO_HEAP_SIZE=0
            USE_ZONE_FOR_MALLOC=1
    )
    target_link_libraries(common_pico INTERFACE hardware_i2c)
endif()
target_include_directories(common_pico INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/..
        ${CMAKE_CURRENT_LIST_DIR}/../../textscreen
        )
target_compile_definitions(common_pico INTERFACE
        NO_USE_MOUSE=1
        PICO_AUDIO_I2S_PIO=1
        PICO_AUDIO_I2S_DMA_IRQ=1
        PICO_USE_SW_SPIN_LOCKS=0
        PICO_USE_STACK_GUARDS=0
)
pico_generate_pio_header(common_pico ${CMAKE_CURRENT_LIST_DIR}/video_doom.pio)
target_link_libraries(common_pico INTERFACE pico_stdlib pico_multicore pico_scanvideo_dpi)

# ---------------------------------------------------------------
# common_pico_picotracker — ST7789 SPI display, GPIO buttons
# ---------------------------------------------------------------
add_library(common_pico_picotracker INTERFACE)
target_sources(common_pico_picotracker INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/i_glob.c
        ${CMAKE_CURRENT_LIST_DIR}/i_input.c
        ${CMAKE_CURRENT_LIST_DIR}/i_system.c
        ${CMAKE_CURRENT_LIST_DIR}/i_timer.c
        ${CMAKE_CURRENT_LIST_DIR}/i_video.c
        ${CMAKE_CURRENT_LIST_DIR}/stubs.c
        ${CMAKE_CURRENT_LIST_DIR}/i_picosound.c
)
# piconet / I2C networking dropped — I2C pins clash with I2S on picoTracker
if (PICO_ON_DEVICE)
    target_sources(common_pico_picotracker INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/blit.S
            ${CMAKE_CURRENT_LIST_DIR}/picoflash.c
            )
    pico_wrap_function(common_pico_picotracker malloc)
    pico_wrap_function(common_pico_picotracker calloc)
    pico_wrap_function(common_pico_picotracker free)
    target_compile_definitions(common_pico_picotracker INTERFACE
            PICO_HEAP_SIZE=0
            USE_ZONE_FOR_MALLOC=1
    )
endif()
target_include_directories(common_pico_picotracker INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/..
        ${CMAKE_CURRENT_LIST_DIR}/../../textscreen
        )
target_compile_definitions(common_pico_picotracker INTERFACE
        PICOTRACKER=1
        NO_USE_MOUSE=1
        NO_USE_UART=1
        NO_USE_NET=1
        PICO_AUDIO_I2S_PIO=1
        PICO_AUDIO_I2S_DMA_IRQ=1
        PICO_AUDIO_I2S_DATA_PIN=17
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=18
        PICO_USE_SW_SPIN_LOCKS=0
        PICO_USE_STACK_GUARDS=0
        SUPPORT_TEXT=0
)
target_link_libraries(common_pico_picotracker INTERFACE
        pico_stdlib pico_multicore
        hardware_spi hardware_dma hardware_pwm hardware_clocks
        hardware_interp
)

add_library(pico_cd INTERFACE)
if (TARGET tinyusb_host)
    target_link_libraries(pico_cd INTERFACE tinyusb_host)
endif()
