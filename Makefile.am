netconfmodule_LTLIBRARIES = libhs-sensor-shield.la

libhs_sensor_shield_la_SOURCES = \
    sensirion_config.h \
    sensirion_common.h \
    sensirion_common.c \
    sensirion_i2c.h \
    sensirion_i2c.c \
    sensirion_i2c_hal.h \
    sensirion_i2c_hal.c \
    sensirion_gas_index_algorithm.h \
    sensirion_gas_index_algorithm.c \
    sht4x.h \
    sht4x.c \
    scd4x.h \
    scd4x.c \
    sgp41.h \
    sgp41.c \
    lps22hb.h \
    lps22hb.c \
    bh1750.h \
    bh1750.c \
    pms7003.h \
    pms7003.c \
    hs-sensor-shield.c

# Depends on installed libyuma-dev
libhs_sensor_shield_la_CPPFLAGS = -I${includedir}/yuma/agt -I${includedir}/yuma/ncx -I${includedir}/yuma/platform $(XML_CPPFLAGS) -I. -pthread
libhs_sensor_shield_la_LDFLAGS = -module -lyumaagt -lyumancx -pthread

# The YANG modules implemented by this netconfd module. netconfd loads them
# from libyuma-base (/usr/share/yuma/modules/ietf); copies are kept here for reference.
EXTRA_DIST = ietf-hardware-state@2018-03-13.yang iana-hardware@2018-03-13.yang
