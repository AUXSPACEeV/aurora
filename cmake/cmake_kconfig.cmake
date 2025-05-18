set(PROJECT_ROOT ${CMAKE_SOURCE_DIR})
set(KCONFIG_ROOT ${PROJECT_ROOT}/Kconfig)
set (APPLICATION_SOURCE_DIR ${PROJECT_ROOT})
set(BOARD_DIR ${PROJECT_ROOT}/configs)
set(AUTOCONF_H ${CMAKE_BINARY_DIR}/kconfig/include/generated/autoconf.h)

# Re-configure (Re-execute all CMakeLists.txt code) when autoconf.h changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${AUTOCONF_H})

include(cmake/extensions.cmake)
include(cmake/python.cmake)
include(cmake/kconfig.cmake)
