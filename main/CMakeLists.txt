set(COMPONENT_SRCS "ota_boot_part.c")
set(COMPONENT_ADD_INCLUDEDIRS ".")

# Embed the server root certificate into the final binary
set(COMPONENT_EMBED_TXTFILES ${IDF_PROJECT_PATH}/boot_part/bootloader.bin ${IDF_PROJECT_PATH}/boot_part/partition-table.bin)

register_component()
