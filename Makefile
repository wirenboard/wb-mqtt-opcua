ifneq ($(DEB_HOST_MULTIARCH),)
	CROSS_COMPILE ?= $(DEB_HOST_MULTIARCH)-
endif

ifeq ($(origin CC),default)
	CC := $(CROSS_COMPILE)gcc
endif
ifeq ($(origin CXX),default)
	CXX := $(CROSS_COMPILE)g++
endif

# extract Git revision and version number from debian/changelog
GIT_REVISION:=$(shell git rev-parse HEAD)
DEB_VERSION:=$(shell head -1 debian/changelog | awk '{ print $$2 }' | sed 's/[\(\)]//g')

TARGET = wb-mqtt-opcua
SRC_DIRS ?= src
LIB62541_DIR = $(shell pwd)/thirdparty/open62541

ifeq ($(DEBUG),)
	BUILD_DIR ?= build/release
	CMAKE_BUILD_TYPE=Release
else
	BUILD_DIR ?= build/debug
	CMAKE_BUILD_TYPE=Debug
endif

COMMON_SRCS := $(shell find $(SRC_DIRS) \( -name *.cpp -or -name *.c \) -and -not -name main.cpp)
COMMON_OBJS := $(COMMON_SRCS:%=$(BUILD_DIR)/%.o)

LIB62541_BUILD_DIR = $(BUILD_DIR)/thirdparty/open62541

# include/open62541 is created when building in devenv with version suffix
# because of a strange hack in open62541's CMakeLists.txt
LIB62541_INCLUDE = -I$(LIB62541_BUILD_DIR)/include -I$(LIB62541_BUILD_DIR)/include/open62541

LDFLAGS = -lwbmqtt1 -lpthread $(LIB62541_BUILD_DIR)/bin/libopen62541.a
CXXFLAGS = -std=c++14 -Wall -Werror $(LIB62541_INCLUDE) -I$(SRC_DIRS) -DWBMQTT_COMMIT="$(GIT_REVISION)" -DWBMQTT_VERSION="$(DEB_VERSION)"
CFLAGS = -Wall $(LIB62541_INCLUDES) -I$(SRC_DIR)

ifeq ($(DEBUG),)
	CXXFLAGS += -O2
else
	CXXFLAGS += -g -O0 -fprofile-arcs -ftest-coverage
	LDFLAGS += -lgcov
endif


TEST_DIR = test
TEST_SRCS := $(shell find $(TEST_DIR) \( -name *.cpp -or -name *.c \) -and -not -name main.cpp)
TEST_OBJS := $(TEST_SRCS:%=$(BUILD_DIR)/%.o)
TEST_TARGET = test-app
TEST_LDFLAGS = -lgtest -lwbmqtt_test_utils

all: $(TARGET)

$(TARGET): $(COMMON_OBJS) $(BUILD_DIR)/src/main.cpp.o
	${CXX} -o $(BUILD_DIR)/$@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.c.o: %.c
	${CC} -c $< -o $@ ${CFLAGS}

$(BUILD_DIR)/%.cpp.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/test/%.o: test/%.cpp
	$(CXX) -c $(CXXFLAGS) -o $@ $^

test: $(TEST_DIR)/$(TEST_TARGET)
	rm -f $(TEST_DIR)/*.dat.out
	if [ "$(shell arch)" != "armv7l" ] && [ "$(CROSS_COMPILE)" = "" ] || [ "$(CROSS_COMPILE)" = "x86_64-linux-gnu-" ]; then \
		valgrind --error-exitcode=180 -q $(TEST_DIR)/$(TEST_TARGET) $(TEST_ARGS) || \
		if [ $$? = 180 ]; then \
			echo "*** VALGRIND DETECTED ERRORS ***" 1>& 2; \
			exit 1; \
		else $(TEST_DIR)/abt.sh show; exit 1; fi; \
    else \
        $(TEST_DIR)/$(TEST_TARGET) $(TEST_ARGS) || { $(TEST_DIR)/abt.sh show; exit 1; } \
	fi

$(TEST_DIR)/$(TEST_TARGET): $(TEST_OBJS) $(COMMON_OBJS) $(BUILD_DIR)/test/main.cpp.o
	$(CXX) -o $@ $^ $(LDFLAGS) $(TEST_LDFLAGS) -fno-lto

open62541_build:
	mkdir -p $(LIB62541_BUILD_DIR)
	cd $(LIB62541_BUILD_DIR); \
	cmake -DCMAKE_INSTALL_PREFIX:PATH="" -DCMAKE_BUILD_TYPE:STRING="$(CMAKE_BUILD_TYPE)" -D CMAKE_C_COMPILER=$(CC) -D CMAKE_CXX_COMPILER=$(CXX) -D CMAKE_C_COMPILER_WORKS=1 -D CMAKE_CXX_COMPILER_WORKS=1 -D CMAKE_INTERPROCEDURAL_OPTIMIZATION=NO $(LIB62541_DIR); \
	$(MAKE) DESTDIR=./ install

clean:
	rm -rf $(BUILD_DIR)

install:
	install -D -m 0644 wb-mqtt-opcua.schema.json $(DESTDIR)/usr/share/wb-mqtt-confed/schemas/wb-mqtt-opcua.schema.json
	install -D -m 0644 wb-mqtt-opcua.sample.conf $(DESTDIR)/usr/share/wb-mqtt-opcua/wb-mqtt-opcua.sample.conf
	install -D -m 0755 $(BUILD_DIR)/$(TARGET) $(DESTDIR)/usr/bin/$(TARGET)
	install -D -m 0644  wb-mqtt-opcua.wbconfigs $(DESTDIR)/etc/wb-configs.d/18wb-mqtt-opcua

.PHONY: all test clean open62541_build
