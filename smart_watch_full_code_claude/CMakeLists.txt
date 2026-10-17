SET(SOURCES ui_Screen1.c
    ui_Screen2.c
    ui_Screen4.c
    ui_Screen5.c
    ui_Screen6.c
    ui_Screen3.c
    ui_Screen7.c
    ui_Screen8.c
    ui_Screen9.c
    ui_Screen10.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    ui_img_208041617.c
    ui_img_bp_icon_png.c
    ui_img_humidity_icon_png.c
    ui_img_temp_icon_png.c
    ui_img_temp_humidity_icon_png.c
    ui_img_wifi_icon_png.c
    ui_img_spo2_icon_png.c)

add_library(ui ${SOURCES})
