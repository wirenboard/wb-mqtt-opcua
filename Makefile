PREFIX = /usr

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

ifeq ($(DEBUG),)
	BUILD_DIR ?= build/release
	CMAKE_BUILD_TYPE=Release
else
	BUILD_DIR ?= build/debug
	CMAKE_BUILD_TYPE=Debug
endif

COMMON_SRCS := $(shell find $(SRC_DIRS) \( -name "*.cpp" -or -name "*.c" \) -and -not -name main.cpp)
COMMON_OBJS := $(COMMON_SRCS:%=$(BUILD_DIR)/%.o)

LDFLAGS = -lwbmqtt1 -lpthread -lopen62541
CXXFLAGS = -std=c++20 -Wall -Werror -I$(SRC_DIRS) -DWBMQTT_COMMIT="$(GIT_REVISION)" -DWBMQTT_VERSION="$(DEB_VERSION)"
CFLAGS = -Wall -I$(SRC_DIR)

ifeq ($(DEBUG),)
	CXXFLAGS += -O2
else
	CXXFLAGS += -g -O0 --coverage
	LDFLAGS += --coverage
endif

TEST_DIR = test
TEST_SRCS := $(shell find $(TEST_DIR) \( -name "*.cpp" -or -name "*.c" \) -and -not -name main.cpp)
TEST_OBJS := $(TEST_SRCS:%=$(BUILD_DIR)/%.o)
TEST_TARGET = test-app
TEST_LDFLAGS = -lgtest -lwbmqtt_test_utils

VALGRIND_FLAGS = --error-exitcode=180 -q

COV_REPORT ?= $(BUILD_DIR)/cov
GCOVR_FLAGS := -s --html $(COV_REPORT).html -x $(COV_REPORT).xml
ifneq ($(COV_FAIL_UNDER),)
	GCOVR_FLAGS += --fail-under-line $(COV_FAIL_UNDER)
endif

all:
	$(MAKE) $(TARGET)

$(TARGET): $(COMMON_OBJS) $(BUILD_DIR)/src/main.cpp.o
	${CXX} -o $(BUILD_DIR)/$@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.c.o: %.c
	${CC} -c $< -o $@ ${CFLAGS}

$(BUILD_DIR)/%.cpp.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $^

test: $(TEST_DIR)/$(TEST_TARGET)
	rm -f $(TEST_DIR)/*.dat.out
	if [ "$(shell arch)" != "armv7l" ] && [ "$(CROSS_COMPILE)" = "" ] || [ "$(CROSS_COMPILE)" = "x86_64-linux-gnu-" ]; then \
		valgrind $(VALGRIND_FLAGS) $(TEST_DIR)/$(TEST_TARGET) $(TEST_ARGS) || \
		if [ $$? = 180 ]; then \
			echo "*** VALGRIND DETECTED ERRORS ***" 1>& 2; \
			exit 1; \
		else $(TEST_DIR)/abt.sh show; exit 1; fi; \
    else \
        $(TEST_DIR)/$(TEST_TARGET) $(TEST_ARGS) || { $(TEST_DIR)/abt.sh show; exit 1; } \
	fi
ifneq ($(DEBUG),)
	gcovr $(GCOVR_FLAGS) $(BUILD_DIR)/$(SRC_DIR) $(BUILD_DIR)/$(TEST_DIR)
endif

$(TEST_DIR)/$(TEST_TARGET): $(TEST_OBJS) $(COMMON_OBJS) $(BUILD_DIR)/test/main.cpp.o
	$(CXX) -o $@ $^ $(LDFLAGS) $(TEST_LDFLAGS) -fno-lto

clean:
	-rm -rf $(BUILD_DIR)
	-rm -rf $(TEST_DIR)/$(TEST_TARGET)

install:
	install -Dm0755 $(BUILD_DIR)/$(TARGET) -t $(DESTDIR)$(PREFIX)/bin
	install -Dm0644 wb-mqtt-opcua.schema.json -t $(DESTDIR)$(PREFIX)/share/wb-mqtt-confed/schemas
	install -Dm0644 wb-mqtt-opcua.sample.conf -t $(DESTDIR)$(PREFIX)/share/wb-mqtt-opcua
	install -Dm0644 wb-mqtt-opcua.wbconfigs $(DESTDIR)/etc/wb-configs.d/18wb-mqtt-opcua

.PHONY: all test clean
