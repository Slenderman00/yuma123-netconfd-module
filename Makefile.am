netconfmodule_LTLIBRARIES = libietf-hardware-state-2.la

libietf_hardware_state_2_la_SOURCES = \
    sensirion_i2c_hal.c \
    sensirion_i2c.c \
    sensirion_common.c \
    sts3x_i2c.c \
    pms7003.c \
    ietf-hardware-state-2.c

# Depends on installed libyuma-dev
libietf_hardware_state_2_la_CPPFLAGS = -I${includedir}/yuma/agt -I${includedir}/yuma/ncx -I${includedir}/yuma/platform $(XML_CPPFLAGS) -I.
libietf_hardware_state_2_la_LDFLAGS = -module -lyumaagt -lyumancx

# The YANG modules implemented by this netconfd module. netconfd loads them
# from libyuma-base (/usr/share/yuma/modules/ietf); copies are kept here for reference.
EXTRA_DIST = ietf-hardware-state@2018-03-13.yang iana-hardware@2018-03-13.yang
