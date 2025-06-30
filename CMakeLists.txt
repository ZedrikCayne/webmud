cmake_minimum_required(VERSION 3.24)

project( webmud )

find_package(OpenSSL)

set( SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src )

set(CMAKE_BUILD_TYPE Debug)

file( GLOB SOURCES ${SOURCE_DIR}/*.cpp ${SOURCE_DIR}/*.c )

add_executable( webmud ${SOURCES} )

target_include_directories( webmud PUBLIC crankshaft/include )

target_link_directories( webmud PUBLIC crankshaft/lib )

if( OPENSSL_FOUND ) 
    target_include_directories( webmud PUBLIC ${OPENSSL_INCLUDE_DIR} )
    target_link_libraries( webmud ${OPENSSL_LIBRARIES} )
endif()

target_link_libraries( webmud -lcrankshaft )

#target_link_libraries( webmud m ssl crypto pthread m z dl crankshaft )

