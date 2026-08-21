netconfmodule_LTLIBRARIES = libietf-hardware-state.la

libietf_hardware_state_la_SOURCES = \
    ietf-hardware-state.c

# Depends on installed libyuma-dev
libietf_hardware_state_la_CPPFLAGS = -I${includedir}/yuma/agt -I${includedir}/yuma/ncx -I${includedir}/yuma/platform $(XML_CPPFLAGS)
libietf_hardware_state_la_LDFLAGS = -module -lyumaagt -lyumancx

# The zigpy daemon, the helper called by the module and the get symlink
# (ietf-hardware-state-get -> ietf-hardware-state-zigbee, kept as on the other
# branches so the helper can be replaced by a different implementation).
bin_SCRIPTS = ietf-hardware-state-zigbeed ietf-hardware-state-zigbee ietf-hardware-state-get

# The YANG modules implemented by this netconfd module. netconfd loads them
# from libyuma-base (/usr/share/yuma/modules/ietf); copies are kept here for reference.
EXTRA_DIST = ietf-hardware-state@2018-03-13.yang iana-hardware@2018-03-13.yang \
    test/mock-zigbeed example/ietf-hardware-state-zigbeed.service
