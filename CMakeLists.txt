cmake_minimum_required(VERSION 3.24)

project( webmud )

set( SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src )

file( GLOB SOURCES ${SOURCE_DIR}/*.cpp ${SOURCE_DIR}/*.c )

add_executable( webmud ${SOURCES} )

target_link_libraries( webmud m ssl crypto pthread m z dl crankshaft )
