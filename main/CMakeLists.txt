set(COMPONENT_SRCS "ota_bootloader.c")
set(COMPONENT_ADD_INCLUDEDIRS ".")

# Embed the server root certificate into the final binary
set(COMPONENT_EMBED_TXTFILES ${IDF_PROJECT_PATH}/bootloader_bin/bootloader.bin)

register_component()
