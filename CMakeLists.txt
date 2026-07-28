cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(udc_stm32_zlp_race_poc)

target_sources(app PRIVATE src/main.c)
