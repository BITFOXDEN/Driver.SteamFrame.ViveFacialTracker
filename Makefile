# Makefile for vftstream (Steam Frame / ARM64 Linux)
#
# On the headset (or any aarch64 Linux with the dependencies):
#   make            # build ./vftstream
#   make install    # install binary + systemd unit (writable-root hosts)
#
# Cross-building from a PC is possible with an aarch64 glibc toolchain and a
# sysroot that includes libjpeg-turbo; the on-device build is the easy path.

CXX      ?= g++
# Built on the Frame, so -mcpu=native tunes for its own cores. The hot path
# is integer-only, so -ffast-math is safe here. Cross-building instead,
# override CXX and CXXFLAGS, e.g.:
#   make CXX=aarch64-linux-gnu-g++ \
#        CXXFLAGS='-O3 -march=armv8-a -ffast-math -flto -std=c++17 ...'
CXXFLAGS ?= -O3 -mcpu=native -ffast-math -flto -std=c++17 -Wall -Wextra -Wno-unused-parameter
LDFLAGS  ?= -s -flto
LDLIBS   ?= -lpthread

# libjpeg-turbo's turbojpeg.h lives in its own include dir on many distros.
TURBO_INC ?= $(shell pkg-config --cflags turbojpeg 2>/dev/null || echo -I/usr/include)
TURBO_LIB ?= $(shell pkg-config --libs turbojpeg 2>/dev/null || echo -lturbojpeg)

TARGET  := vftstream
SRCS    := src/main.cpp src/pipeline.cpp src/config.cpp src/http_server.cpp src/usb_uvc.cpp
OBJS    := $(SRCS:.cpp=.o)
DEPS    := $(SRCS:.cpp=.d)

CPPFLAGS += $(TURBO_INC)
LDLIBS += $(TURBO_LIB)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	install -Dm644 packaging/vftstream.service /usr/local/lib/systemd/system/vftstream.service
	@echo "Enable with:  sudo systemctl enable --now vftstream"

uninstall:
	rm -f /usr/local/bin/$(TARGET)
	rm -f /usr/local/lib/systemd/system/vftstream.service

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

.PHONY: all install uninstall clean
