target_sources(pT_dx7 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/audio_render_core.c
    ${CMAKE_CURRENT_LIST_DIR}/audio_output.c
    ${CMAKE_CURRENT_LIST_DIR}/fm_synth.c
    ${CMAKE_CURRENT_LIST_DIR}/i2c_reg_io.c
    ${CMAKE_CURRENT_LIST_DIR}/sine_wave_table.c
    ${CMAKE_CURRENT_LIST_DIR}/reverb.c
)

target_include_directories(pT_dx7 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_generate_pio_header(pT_dx7 ${CMAKE_CURRENT_LIST_DIR}/mclk.pio)
pico_generate_pio_header(pT_dx7 ${CMAKE_CURRENT_LIST_DIR}/audio_i2s_pcm5102.pio)
