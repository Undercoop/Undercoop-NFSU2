# Copyright (C) 2026 batuhan.
# GNU GPL version 3 only, with LICENSE-7ZIP-EXCEPTION.txt.
cmake_minimum_required(VERSION 3.21)
project(NFSU2Coop LANGUAGES C CXX RC)

add_library(UndercoopTransport STATIC src/CoopTransportWin.cpp
    src/third_party/enet/callbacks.c src/third_party/enet/compress.c
    src/third_party/enet/host.c src/third_party/enet/list.c
    src/third_party/enet/packet.c src/third_party/enet/peer.c
    src/third_party/enet/protocol.c src/third_party/enet/win32.c)
target_include_directories(UndercoopTransport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/enet/include")
set_target_properties(UndercoopTransport PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
target_link_libraries(UndercoopTransport PUBLIC ws2_32 winmm)

add_library(NFSU2Coop SHARED src/NFSU2Coop.cpp src/NFSU2Coop.rc)
target_include_directories(NFSU2Coop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(NFSU2Coop PRIVATE UNDERCOOP_CMAKE_TRANSPORT)
target_link_libraries(NFSU2Coop PRIVATE UndercoopTransport)
if(MSVC)
    target_compile_options(NFSU2Coop PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/arch:IA32>)
endif()
set_target_properties(NFSU2Coop PROPERTIES
    OUTPUT_NAME "NFSU2Coop"
    SUFFIX ".asi"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES)
