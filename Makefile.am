netconfmodule_LTLIBRARIES = liblsi-ivi-load.la

liblsi_ivi_load_la_SOURCES = \
lsi-ivi-load.c

# Depends on installed libyuma-dev
liblsi_ivi_load_la_CPPFLAGS = -I${includedir}/yuma/agt -I${includedir}/yuma/ncx -I${includedir}/yuma/platform $(XML_CPPFLAGS)
liblsi_ivi_load_la_LDFLAGS = -module -lyumaagt -lyumancx

yang_DATA = lsi-ivi-load.yang

bin_SCRIPTS = lsi-ivi-load-set lsi-ivi-load-get

