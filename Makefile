CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
TARGET = tzt_patcher
SRC = $(wildcard src/*.cpp)
BUILDDIR = build
BINTARGET = $(BUILDDIR)/$(TARGET)
FIX = $(BUILDDIR)/fix_ptrace.sh

all: $(BUILDDIR) $(BINTARGET) $(FIX)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BINTARGET): $(SRC) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $(BINTARGET) $(SRC)
	@echo ""
	@echo "✅ Compilation successful!"
	@echo ""
#	@echo "Before running, fix ptrace_scope:"
#	@echo "  ./$(FIX)"
#	@echo ""
	@echo "Then run:"
	@echo "  sudo ./$(BINTARGET)"

$(FIX): | $(BUILDDIR)
	@echo '#!/bin/bash' > $(FIX)
	@echo 'echo "Checking ptrace_scope..."' >> $(FIX)
	@echo 'CURRENT=$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null)' >> $(FIX)
	@echo 'echo "Current value: $CURRENT"' >> $(FIX)
	@echo 'if [ "$CURRENT" != "0" ]; then' >> $(FIX)
	@echo '  echo "Setting ptrace_scope to 0..."' >> $(FIX)
	@echo '  sudo sysctl -w kernel.yama.ptrace_scope=0' >> $(FIX)
	@echo '  echo "✅ Done! Now run: sudo ./$(BINTARGET)"' >> $(FIX)
	@echo 'else' >> $(FIX)
	@echo '  echo "✅ Already set to 0"' >> $(FIX)
	@echo 'fi' >> $(FIX)
	@chmod +x $(FIX)

clean:
	rm -rf $(BUILDDIR)

install: $(BINTARGET)
	sudo cp $(BINTARGET) /usr/local/bin/

run: $(BINTARGET)
	@echo "Checking ptrace_scope..."
	@bash -c 'if [ "$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null)" != "0" ]; then echo "⚠️  Run ./$(FIX) first!"; exit 1; fi'
	sudo ./$(BINTARGET)

.PHONY: all clean install run
