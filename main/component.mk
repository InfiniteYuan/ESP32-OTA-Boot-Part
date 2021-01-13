#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)

COMPONENT_EMBED_TXTFILES := ${PROJECT_PATH}/boot_part/bootloader.bin
COMPONENT_EMBED_TXTFILES += ${PROJECT_PATH}/boot_part/partition-table.bin
