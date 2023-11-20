cmake_minimum_required(VERSION 3.20)

set(ESPEAK_OUT "espeak")

project(espeak LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

#add_executable(${ESPEAK_OUT} "")
add_library(${ESPEAK_OUT} SHARED "")

file(GLOB ESPEAK_SOURCE *.c *.h)
if (${CMAKE_C_COMPILER_ID} STREQUAL GNU)
    file(GLOB ESPEAK_SOURCE_GNU gcc/*.c gcc/*.h)
    list(APPEND ESPEAK_SOURCE ${ESPEAK_SOURCE_GNU})
    target_include_directories(${ESPEAK_OUT} PRIVATE gcc)
    if(${CMAKE_HOST_SYSTEM_NAME} MATCHES Windows)
        set(CMAKE_IMPORT_LIBRARY_PREFIX  "")
        set(CMAKE_IMPORT_LIBRARY_SUFFIX ".a")
        set(CMAKE_SHARED_LIBRARY_PREFIX  "")
        set(CMAKE_SHARED_LIBRARY_SUFFIX   ".dll")
        list(REMOVE_ITEM ESPEAK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/gcc/mbrowrap.h")
        list(REMOVE_ITEM ESPEAK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/gcc/mbrowrap.c")
    endif ()
elseif (${CMAKE_C_COMPILER_ID} STREQUAL MSVC)
    file(GLOB ESPEAK_SOURCE_MSVC msvc/*.c msvc/*.h)
    list(APPEND ESPEAK_SOURCE ${ESPEAK_SOURCE_MSVC})
    target_include_directories(${ESPEAK_OUT} PRIVATE msvc)
else ()
    message(FATAL_ERROR "Unknown Compiler:" ${CMAKE_C_COMPILER_ID})
endif ()

list(REMOVE_ITEM ESPEAK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/espeak_libtest.c")

target_sources(${ESPEAK_OUT} PRIVATE
        ${ESPEAK_SOURCE})

target_link_directories(${ESPEAK_OUT} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/portaudio)

target_link_libraries(${ESPEAK_OUT} PRIVATE
        portaudio)

target_include_directories( ${ESPEAK_OUT} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions( ${ESPEAK_OUT} PRIVATE
        USE_PORTAUDIO USE_ASYNC DEBUG_ENABLED)
