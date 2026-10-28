# Nano Java desktop build (gcc and a JDK 8 javac): the class library and
# the headless host VM used by the tests. The RP2350 firmware is built with
# CMake (CMakeLists.txt, build_all.sh / build_all.bat).
#
#   make classlib   build/classlib.jar
#   make host       build/host/nanojava (desktop build, used by tests)
#   make test       run the test suite on the host build
#   make prebuilt   refresh classlib/classlib.jar (used by CMake without JDK 8)
#   make clean

BUILD       := build
HOST_CC     ?= gcc
HOST_CFLAGS := -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter \
               -Wno-missing-field-initializers -D_POSIX_C_SOURCE=200809L
HOST_LIBS   := -lm

CORE_SRC := $(wildcard src/vm/*.c src/util/*.c src/midp/*.c src/audio/*.c)
HOST_SRC := $(CORE_SRC) $(wildcard platform/host/*.c)
HOST_OBJ := $(patsubst %.c,$(BUILD)/host/obj/%.o,$(HOST_SRC))
HEADERS  := $(wildcard src/*/*.h platform/host/*.h)

JAVA_SRC := $(shell find classlib/src -name '*.java')
JAVAC    := javac -nowarn -Xlint:-options -encoding UTF-8 -source 1.3 -target 1.3

.PHONY: all classlib host test prebuilt clean

all: classlib host

classlib: $(BUILD)/classlib.jar

$(BUILD)/classlib.jar: $(JAVA_SRC)
	@rm -rf $(BUILD)/classlib && mkdir -p $(BUILD)/classlib
	@echo "javac classlib ($(words $(JAVA_SRC)) files)"
	@$(JAVAC) -bootclasspath $(BUILD)/classlib -extdirs "" -d $(BUILD)/classlib $(JAVA_SRC)
	@cd $(BUILD)/classlib && jar cfM ../classlib.jar .

host: $(BUILD)/host/nanojava

$(BUILD)/host/nanojava: $(HOST_OBJ)
	@echo "link $@"
	@$(HOST_CC) -o $@ $(HOST_OBJ) $(HOST_LIBS)

$(BUILD)/host/obj/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	@echo "cc $<"
	@$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

test: classlib host
	@sh tests/run.sh

prebuilt: classlib
	@cp $(BUILD)/classlib.jar classlib/classlib.jar
	@echo "updated classlib/classlib.jar"

clean:
	rm -rf $(BUILD)
