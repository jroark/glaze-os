# Set GLAZE_SDK to the extracted SDK directory (or pass it on make's command line).
ifndef GLAZE_SDK
$(error Set GLAZE_SDK to the directory containing bin/glaze-sdk and sysroot)
endif
SOURCES := main.c
# Extra modules: SOURCES += src/model.c
# Optional media parsers: LDLIBS += -l:libmedia.so.1
include $(GLAZE_SDK)/share/app.mk
