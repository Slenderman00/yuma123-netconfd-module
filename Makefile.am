netconfmodule_LTLIBRARIES = liblsi-thermometers.la

liblsi_thermometers_la_SOURCES = \
    lsi-thermometers.c \
    sensirion_i2c_hal.c \
    sensirion_common.c \
    sts3x_i2c.c

# Depends on installed libyuma-dev
liblsi_thermometers_la_CPPFLAGS = -I${includedir}/yuma/agt -I${includedir}/yuma/ncx -I${includedir}/yuma/platform $(XML_CPPFLAGS) -I.
liblsi_thermometers_la_LDFLAGS = -module -lyumaagt -lyumancx

yang_DATA = lsi-thermometers.yang

bin_SCRIPTS = thermometers-get
