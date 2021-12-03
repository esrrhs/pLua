project(plua)
aux_source_directory(./ plua_src)
add_library(plua SHARED ${plua_src})
TARGET_LINK_LIBRARIES(plua)
