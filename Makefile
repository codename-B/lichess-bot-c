CC ?= cc
BIN_DIR ?= .
OBJ_DIR ?= build/obj
TEST_DIR ?= build
YAML_BUILD_DIR ?= build/yaml
TB_BUILD_DIR ?= build/vendor
TARGET ?= $(BIN_DIR)/lichess-bot-c
CFLAGS ?= -O3 -DNDEBUG
CURL_CFLAGS ?=
CURL_LIBS ?= -lcurl
DL_LIBS ?= -ldl
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror

.DEFAULT_GOAL := all
include vendor/tablebases.mk
CPPFLAGS += -Isrc -Iinclude -Ivendor/libyaml/include $(TB_CPPFLAGS) $(CURL_CFLAGS)
LIBS = $(CURL_LIBS) $(DL_LIBS) $(TABLEBASE_LIBS)
YAML_NAMES = api reader scanner parser loader
YAML_OBJECTS = $(addprefix $(YAML_BUILD_DIR)/,$(addsuffix .o,$(YAML_NAMES)))
YAML_DEFINES = -D_POSIX_C_SOURCE=200809L -DYAML_VERSION_MAJOR=0 -DYAML_VERSION_MINOR=2 \
 -DYAML_VERSION_PATCH=5 -DYAML_VERSION_STRING=\"0.2.5\"
MAIN_NAMES = main config settings engine json logging position book tablebase variant_syzygy \
 util evaluation policy providers archive hooks
MAIN_OBJECTS = $(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(MAIN_NAMES)))
XBOARD_OBJECTS = $(addprefix $(OBJ_DIR)/,xboard_main.o xboard.o engine.o position.o json.o)
HOMEMADE_OBJECTS = $(addprefix $(OBJ_DIR)/,homemade_main.o homemade.o position.o)
UNIT_TESTS = $(addprefix $(TEST_DIR)/,test-json test-engine test-settings test-position \
 test-book test-tablebase test-archive test-hooks test-features test-logging)
.PHONY: all native sanitize sanitize-test test test-unit test-build clean install

all: $(TARGET) $(BIN_DIR)/lbc-xboard $(BIN_DIR)/lbc-homemade

$(OBJ_DIR)/%.o: src/%.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -MMD -MP -c $< -o $@

$(YAML_BUILD_DIR)/%.o: vendor/libyaml/src/%.c vendor/libyaml/src/yaml_private.h vendor/libyaml/include/yaml.h
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -std=c11 -w -Ivendor/libyaml/include $(YAML_DEFINES) -c $< -o $@

$(TARGET): $(MAIN_OBJECTS) $(YAML_OBJECTS) $(TABLEBASE_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@

$(BIN_DIR)/lbc-xboard: $(XBOARD_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BIN_DIR)/lbc-homemade: $(HOMEMADE_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(DL_LIBS) -lpthread -o $@

$(TEST_DIR)/test-json: tests/test_json.c $(OBJ_DIR)/json.o
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) -o $@

$(TEST_DIR)/test-engine: tests/test_engine.c $(OBJ_DIR)/engine.o
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) -o $@

$(TEST_DIR)/test-settings: tests/test_settings.c $(OBJ_DIR)/settings.o $(OBJ_DIR)/config.o $(YAML_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) $(CURL_LIBS) -lm -o $@

$(TEST_DIR)/test-position: tests/test_position.c $(OBJ_DIR)/position.o
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) -o $@

$(TEST_DIR)/test-book: tests/test_book.c $(OBJ_DIR)/book.o $(OBJ_DIR)/position.o
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) -o $@

$(TEST_DIR)/test-tablebase: tests/test_tablebase.c $(OBJ_DIR)/tablebase.o $(OBJ_DIR)/variant_syzygy.o $(OBJ_DIR)/position.o $(TABLEBASE_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) $(TABLEBASE_LIBS) -o $@

$(TEST_DIR)/test-archive: tests/test_archive.c $(OBJ_DIR)/archive.o $(OBJ_DIR)/position.o $(OBJ_DIR)/settings.o $(OBJ_DIR)/util.o $(OBJ_DIR)/json.o $(YAML_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) -lpthread -lm -o $@

$(TEST_DIR)/test-hooks: tests/test_hooks.c $(OBJ_DIR)/hooks.o
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) $(DL_LIBS) -o $@

$(TEST_DIR)/test-features: tests/test_features.c $(addprefix $(OBJ_DIR)/,policy.o providers.o evaluation.o util.o config.o settings.o position.o json.o) $(YAML_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) $(CURL_LIBS) -lm -o $@

$(TEST_DIR)/test-logging: tests/test_logging.c $(addprefix $(OBJ_DIR)/,logging.o settings.o util.o json.o) $(YAML_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) -lm -o $@

$(TEST_DIR)/custom-handlers.so: examples/custom_handlers.c include/lbc_plugin.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -fPIC -shared $< $(LDFLAGS) -o $@

test-build: all $(UNIT_TESTS) $(TEST_DIR)/custom-handlers.so

test-unit: $(UNIT_TESTS) $(TEST_DIR)/custom-handlers.so
	$(TEST_DIR)/test-json
	$(TEST_DIR)/test-engine "$(abspath $(TEST_DIR)/test-engine)"
	$(TEST_DIR)/test-settings config.yml.default
	$(TEST_DIR)/test-position
	$(TEST_DIR)/test-book
	$(TEST_DIR)/test-tablebase
	$(TEST_DIR)/test-archive
	$(TEST_DIR)/test-hooks "$(abspath $(TEST_DIR)/custom-handlers.so)"
	$(TEST_DIR)/test-features
	$(TEST_DIR)/test-logging

test: all test-unit

# Build for the current CPU in a separate directory.
native:
	$(MAKE) TARGET=build/native/lichess-bot-c BIN_DIR=build/native OBJ_DIR=build/native/obj TEST_DIR=build/native/tests YAML_BUILD_DIR=build/native/yaml TB_BUILD_DIR=build/native/vendor CFLAGS="-O3 -DNDEBUG -march=native -flto" LDFLAGS="-flto" all

sanitize sanitize-test:
	$(MAKE) TARGET=build/sanitize/lichess-bot-c BIN_DIR=build/sanitize OBJ_DIR=build/sanitize/obj TEST_DIR=build/sanitize/tests YAML_BUILD_DIR=build/sanitize/yaml TB_BUILD_DIR=build/sanitize/vendor CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" $(if $(filter sanitize-test,$@),test,test-build)

PREFIX ?= /usr/local
install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/libexec/lichess-bot-c $(DESTDIR)$(PREFIX)/include/lichess-bot-c
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/lichess-bot-c
	install -m 755 $(BIN_DIR)/lbc-xboard $(BIN_DIR)/lbc-homemade $(DESTDIR)$(PREFIX)/libexec/lichess-bot-c/
	install -m 644 include/lbc_plugin.h $(DESTDIR)$(PREFIX)/include/lichess-bot-c/

clean:
	rm -rf build
	rm -f lichess-bot-c lbc-xboard lbc-homemade

-include $(wildcard $(OBJ_DIR)/*.d)
