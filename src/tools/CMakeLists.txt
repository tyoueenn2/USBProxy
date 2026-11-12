#
# This file is part of USBProxy.
#

set(TOOL usb-mitm)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" USBPROXY_VERSION LIMIT_COUNT 1)
configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

include_directories(${libusbproxy_SOURCE_DIR})
LIST(APPEND TOOLS_LINK_LIBS USBProxy -pthread zmq)

add_executable(${TOOL} ${TOOL}.cpp)
set_source_files_properties(${TOOL}.cpp PROPERTIES COMPILE_FLAGS  "-std=c++11 -pthread -Wall -I.")
target_link_libraries(${TOOL} ${TOOLS_LINK_LIBS})
install(TARGETS ${TOOL} RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})
