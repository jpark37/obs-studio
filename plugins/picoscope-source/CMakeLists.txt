project(picoscope-source)

add_library(picoscope-source MODULE)
add_library(OBS::picoscope-source ALIAS picoscope-source)

target_sources(picoscope-source PRIVATE picoscope-source.c)

target_link_libraries(picoscope-source PRIVATE OBS::libobs)

set_target_properties(picoscope-source PROPERTIES FOLDER "plugins" PREFIX "")

setup_plugin_target(picoscope-source)
