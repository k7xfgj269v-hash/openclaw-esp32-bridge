# ESP-IDF 工程入口（支持把工程放在子目录）
# 使用前先 source $IDF_PATH/export.sh（或 zsh 里 get_idf），再 idf.py set-target esp32s3
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openclaw_companion)
