.POSIX:
.SUFFIXES: .o .c

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

COMMOBJ  = main.o util.o parse.o abi.o cfg.o mem.o ssa.o alias.o load.o \
           copy.o fold.o gvn.o gcm.o simpl.o ifopt.o live.o spill.o rega.o \
           emit.o
AMD64OBJ = amd64/targ.o amd64/sysv.o amd64/isel.o amd64/emit.o amd64/winabi.o
ARM64OBJ = arm64/targ.o arm64/abi.o arm64/isel.o arm64/emit.o
RV64OBJ  = rv64/targ.o rv64/abi.o rv64/isel.o rv64/emit.o
WASMOBJ  = wasm/targ.o wasm/abi.o wasm/isel.o wasm/emit.o
MOSOBJ   = mos/targ.o mos/abi.o mos/isel.o mos/emit.o
OBJ      = $(COMMOBJ) $(AMD64OBJ) $(ARM64OBJ) $(RV64OBJ) $(WASMOBJ) $(MOSOBJ)

SRCALL   = $(OBJ:.o=.c)

CC       = cc
CFLAGS   = -std=c99 -g -Wall -Wextra -Wpedantic

qbe: $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ): all.h ops.h
$(AMD64OBJ): amd64/all.h
$(ARM64OBJ): arm64/all.h
$(RV64OBJ): rv64/all.h
$(WASMOBJ): wasm/all.h
$(MOSOBJ): mos/all.h
main.o: config.h

config.h:
	@case `uname` in                               \
	*Darwin*)                                      \
		case `uname -m` in                     \
		*arm64*)                               \
			echo "#define Deftgt T_arm64_apple";\
			;;                             \
		*)                                     \
			echo "#define Deftgt T_amd64_apple";\
			;;                             \
		esac                                   \
		;;                                     \
	*)                                             \
		case `uname -m` in                     \
		*aarch64*|*arm64*)                     \
			echo "#define Deftgt T_arm64"; \
			;;                             \
		*riscv64*)                             \
			echo "#define Deftgt T_rv64";  \
			;;                             \
		*)                                     \
			echo "#define Deftgt T_amd64_sysv";\
			;;                             \
		esac                                   \
		;;                                     \
	esac > $@

install: qbe
	mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m755 qbe "$(DESTDIR)$(BINDIR)/qbe"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/qbe"

clean:
	rm -f *.o */*.o qbe

clean-gen: clean
	rm -f config.h

check: qbe
	tools/test.sh all

check-x86_64: qbe
	TARGET=x86_64 tools/test.sh all

check-arm64: qbe
	TARGET=arm64 tools/test.sh all

check-rv64: qbe
	TARGET=rv64 tools/test.sh all

check-amd64_win: qbe
	TARGET=amd64_win tools/test.sh all

check-wasm: qbe
	test/wasm/run.sh

check-mos: check check-wasm
	test/mos/run.sh

check-wasm-native: qbe
	test/wasm/run_native.sh

check-cppcheck-wasm:
	cppcheck -q --std=c99 --error-exitcode=1         \
		--enable=style,portability                \
		--suppress=missingIncludeSystem            \
		--suppress=variableScope                  \
		--suppress=constVariablePointer            \
		--suppress=constParameterPointer           \
		-I. wasm/emit.c wasm/abi.c                \
		wasm/isel.c wasm/targ.c

check-scan-wasm:
	@for f in wasm/emit.c wasm/abi.c                  \
		wasm/isel.c wasm/targ.c;                  \
	do                                                \
		echo "scan-build: $$f";                   \
		scan-build --status-bugs                  \
			$(CC) $(CFLAGS) -c $$f            \
			-o /dev/null || exit 1;           \
	done

check-asan-wasm: config.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined      \
		-fno-omit-frame-pointer $(SRCALL)         \
		-o qbe.asan
	ASAN_OPTIONS=detect_leaks=0 bin=./qbe.asan        \
		test/wasm/run.sh;                         \
	rc=$$?; rm -f qbe.asan; exit $$rc

src:
	@echo $(SRCALL)

80:
	@for F in $(SRCALL);                       \
	do                                         \
		awk "{                             \
			gsub(/\\t/, \"        \"); \
			if (length(\$$0) > $@)     \
				printf(\"$$F:%d: %s\\n\", NR, \$$0); \
		}" < $$F;                          \
	done

wc:
	@wc -l $(SRCALL)

.PHONY: clean clean-gen check check-arm64 check-rv64 check-wasm \
       check-wasm-native check-cppcheck-wasm check-scan-wasm \
       check-asan-wasm check-mos src 80 wc install uninstall
