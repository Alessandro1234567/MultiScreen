# MultiScreen host Linux
PROTO_KDE ?= $(shell pkg-config --variable=pkgdatadir plasma-wayland-protocols 2>/dev/null || echo /usr/share/plasma-wayland-protocols)
PROTO_WL  ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null || echo /usr/share/wayland-protocols)
SCANNER   ?= wayland-scanner
PKGS = glib-2.0 gio-2.0 gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 wayland-client
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(shell pkg-config --cflags $(PKGS)) -Ibuild
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm

XMLS = $(PROTO_KDE)/zkde-screencast-unstable-v1.xml $(PROTO_KDE)/fake-input.xml $(PROTO_WL)/unstable/xdg-output/xdg-output-unstable-v1.xml
GEN_H = build/zkde-screencast-unstable-v1-client-protocol.h build/fake-input-client-protocol.h build/xdg-output-unstable-v1-client-protocol.h
GEN_C = build/zkde-screencast-unstable-v1-protocol.c build/fake-input-protocol.c build/xdg-output-unstable-v1-protocol.c
SRC = src/main.c src/session.c src/kde.c src/video.c src/adb.c src/util.c src/clip.c src/files.c

all: build/multiscreen-host

build:
	mkdir -p build

build/%-client-protocol.h: | build
	$(SCANNER) client-header $(filter %/$*.xml,$(XMLS)) $@
build/%-protocol.c: | build
	$(SCANNER) private-code $(filter %/$*.xml,$(XMLS)) $@

build/multiscreen-host: $(SRC) src/*.h $(GEN_H) $(GEN_C)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(GEN_C) $(LDLIBS)

clean:
	rm -rf build
.PHONY: all clean
