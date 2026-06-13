CXX      = g++
TARGET   = tzt_patcher
BUILDDIR = build

# ── Source & object discovery ─────────────────────────────────────────────────
SRC     = $(wildcard src/*.cpp)
OBJDIR  = $(BUILDDIR)/obj
OBJS    = $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(SRC))
DEPS    = $(OBJS:.o=.d)

# ── Build profiles ────────────────────────────────────────────────────────────
# release: максимальная оптимизация
CXXFLAGS_RELEASE = -std=c++17 -O3 -march=native -flto -fomit-frame-pointer \
                   -DNDEBUG -pipe -Wall -Wextra

# debug: полная отладочная информация, без оптимизаций
CXXFLAGS_DEBUG   = -std=c++17 -Og -g3 -fsanitize=address,undefined \
                   -fno-omit-frame-pointer -Wall -Wextra -DDEBUG

# default (то что было раньше, если просто `make`)
CXXFLAGS        ?= $(CXXFLAGS_RELEASE)
LDFLAGS_DEBUG    = -fsanitize=address,undefined

BINTARGET = $(BUILDDIR)/$(TARGET)
FIX       = $(BUILDDIR)/fix_ptrace.sh

# ── Авто-подключение зависимостей (.d файлы) ─────────────────────────────────
-include $(DEPS)

# ── Цели ─────────────────────────────────────────────────────────────────────
.PHONY: all release debug clean install run

all: release

release: CXXFLAGS := $(CXXFLAGS_RELEASE)
release: $(BUILDDIR) $(OBJDIR) $(BINTARGET) $(FIX)
	@echo ""
	@echo "✅ Release build: $(BINTARGET)"
	@echo "   Flags: $(CXXFLAGS_RELEASE)"
	@echo ""
	@echo "Then run:"
	@echo "  sudo ./$(BINTARGET)"

debug: CXXFLAGS := $(CXXFLAGS_DEBUG)
debug: LDFLAGS  := $(LDFLAGS_DEBUG)
debug: $(BUILDDIR) $(OBJDIR) $(BINTARGET) $(FIX)
	@echo ""
	@echo "🐛 Debug build: $(BINTARGET)"
	@echo "   Flags: $(CXXFLAGS_DEBUG)"
	@echo ""
	@echo "Then run:"
	@echo "  sudo ./$(BINTARGET)"

# ── Каталоги ─────────────────────────────────────────────────────────────────
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

# ── Компиляция объектных файлов с генерацией зависимостей ────────────────────
# -MMD -MP создают .d файлы рядом с .o — инкрементальная пересборка работает
$(OBJDIR)/%.o: src/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# ── Линковка ─────────────────────────────────────────────────────────────────
$(BINTARGET): $(OBJS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

# ── Скрипт fix_ptrace ─────────────────────────────────────────────────────────
$(FIX): | $(BUILDDIR)
	@printf '#!/bin/bash\n' > $(FIX)
	@printf 'CURRENT=$$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null)\n' >> $(FIX)
	@printf 'echo "ptrace_scope = $$CURRENT"\n' >> $(FIX)
	@printf 'if [ "$$CURRENT" != "0" ]; then\n' >> $(FIX)
	@printf '  sudo sysctl -w kernel.yama.ptrace_scope=0\n' >> $(FIX)
	@printf '  echo "✅ Done! Now run: sudo ./$(BINTARGET)"\n' >> $(FIX)
	@printf 'else\n' >> $(FIX)
	@printf '  echo "✅ Already 0"\n' >> $(FIX)
	@printf 'fi\n' >> $(FIX)
	@chmod +x $(FIX)

# ── Утилиты ───────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILDDIR)

install: release
	sudo cp $(BINTARGET) /usr/local/bin/

run: release
	@bash -c 'if [ "$$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null)" != "0" ]; \
	  then echo "⚠️  Run ./$(FIX) first!"; exit 1; fi'
	sudo ./$(BINTARGET)
