# The sb target is a RAM image: with the dev-boot pins the boot ROM waits for a
# debugger, so the image is loaded over SWD and USB1/CN18 stays free for the
# PoC device itself.

BOARD     ?= stm32n6570_dk/stm32n657xx/sb
BUILD_DIR ?= build
GDB_PORT  ?= 61234

.PHONY: all
all: build

.PHONY: build
build:
	west build -b $(BOARD) --build-dir $(BUILD_DIR) .

.PHONY: flash-swd
flash-swd: build
	@west debugserver --build-dir $(BUILD_DIR) --port-number $(GDB_PORT) \
		> $(BUILD_DIR)/gdbserver.log 2>&1 & \
	server=$$!; \
	trap 'kill $$server 2>/dev/null' EXIT; \
	sleep 3; \
	gdb=$$(sed -n 's/^  gdb: //p' $(BUILD_DIR)/zephyr/runners.yaml); \
	"$$gdb" -q -batch $(BUILD_DIR)/zephyr/zephyr.elf \
		-ex "target remote :$(GDB_PORT)" -ex load -ex detach \
	|| { echo "load failed, gdb server log:"; cat $(BUILD_DIR)/gdbserver.log; exit 1; }

HOST_STAMP := $(BUILD_DIR)/.host-deps

$(HOST_STAMP): host/requirements.txt
	@mkdir -p $(@D)
	pip install -q -r $<
	@touch $@

.PHONY: read
read: $(HOST_STAMP)
	@for i in $$(seq 20); do lsusb -d 2fe3:f00d >/dev/null 2>&1 && break; sleep 0.5; done
	python host/read_bulk.py

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
