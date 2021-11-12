DEFAULT_VENV = ./venv
# Defaults if not set by environment
DEVICE := $(if $(DEVICE),$(DEVICE),d1_mini)
PORT := $(if $(PORT),$(PORT),/dev/ttyUSB0)
BAUD := $(if $(BAUD),$(BAUD),115200)
VENV := $(if $(VENV),$(VENV),$(DEFAULT_VENV))
PIO := $(if $(PIO),$(PIO),$(VENV)/bin/pio)

BUILD_DIR = build
BIN = $(BUILD_DIR)/$(DEVICE)/firmware.bin


all: $(BIN)

build: $(BIN)

$(BIN):src/main.cpp src/led.h src/led.cpp platformio.ini
	@echo Building: $(BIN)
	$(PIO) run -e $(DEVICE) -j8
	@# Force update of timestamp of BIN as it may not be if source changes don't require it
	@test -f $(BIN)  && touch $(BIN)

clean:
	$(PIO) run -e $(DEVICE) -t clean

cleanall:
	$(PIO) run -t cleanall
	rm -r $(BUILD_DIR)
	rm -r $(DEFAULT_VENV)

init: $(VENV)

$(VENV):
	@echo Initialising PlatformIO into $(VENV)
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install -U pip wheel
	$(VENV)/bin/pip install -U platformio

upload: $(BIN)
	@echo Flashing...
	$(PIO) run -e $(DEVICE) -t upload --upload-port $(PORT)

tail:
	stty -F $(PORT) $(BAUD)
	tail -f $(PORT)


