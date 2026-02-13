
CXX      := g++
TARGET   := luma
SRC      := main.cpp

PKGFLAGS := $(shell pkg-config --cflags --libs jack lilv-0 x11)

CXXFLAGS := -std=c++17 -Wall -Wextra -O2
LDFLAGS  := -ldl

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(PKGFLAGS) $(LDFLAGS)

debug: CXXFLAGS := -std=c++17 -Wall -Wextra -g -O0
debug: clean all

clean:
	rm -f $(TARGET)

.PHONY: all clean debug
