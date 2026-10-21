cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(camera_board_test)
# exFATのカードをそのまま使うためにfatfsをプロジェクト内に置いている
# 縞除去でUXGAのJPEGを展開・再圧縮するので速度優先でビルド
foreach(component espressif__esp32-camera espressif__esp_jpeg)
    idf_component_get_property(library ${component} COMPONENT_LIB)
    target_compile_options(${library} PRIVATE -O2)
endforeach()
