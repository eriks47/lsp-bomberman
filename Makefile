CC      := gcc
CFLAGS  := -Wall -Wextra -Wpedantic -Icommon
LDFLAGS :=

BUILD   := build

CLIENT_SRCS := $(wildcard client/*.c)
SERVER_SRCS := $(wildcard server/*.c)

CLIENT_OBJS := $(patsubst %.c, $(BUILD)/%.o, $(CLIENT_SRCS))
SERVER_OBJS := $(patsubst %.c, $(BUILD)/%.o, $(SERVER_SRCS))

CLIENT_BIN  := $(BUILD)/client/client
SERVER_BIN  := $(BUILD)/server/server

# ── Default target ────────────────────────────────────────────────────────────
.PHONY: all
all: $(CLIENT_BIN) $(SERVER_BIN)

# ── Linking ───────────────────────────────────────────────────────────────────
$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ── Compilation ───────────────────────────────────────────────────────────────
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Convenience targets ───────────────────────────────────────────────────────
.PHONY: client server
client: $(CLIENT_BIN)
server: $(SERVER_BIN)

.PHONY: clean
clean:
	rm -rf $(BUILD)
