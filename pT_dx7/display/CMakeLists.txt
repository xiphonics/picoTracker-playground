target_sources(pT_dx7 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/algo_gfx.c
    ${CMAKE_CURRENT_LIST_DIR}/chargfx.c
    ${CMAKE_CURRENT_LIST_DIR}/display_startup.c
    ${CMAKE_CURRENT_LIST_DIR}/ili9341.c
    ${CMAKE_CURRENT_LIST_DIR}/ui_widgets.c
)

target_compile_definitions(pT_dx7 PRIVATE
    LCD_ST7789
)

target_include_directories(pT_dx7 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
