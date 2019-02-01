PROGRAM = mini-player2
CC      = gcc
ECHO    = printf "$(1)\t%s\n" $(2)
BRIEF   = CC
MSG     = $@
CFLAG   = -s
LIB     = -lavdevice -lavcodec -lavfilter -lavformat -lavutil -lswresample -lSDL2 -lole32 -lShell32 -lKernel32

$(foreach VAR,$(BRIEF), \
	$(eval override $(VAR) = @$$(call ECHO,$(VAR),$$(MSG)); $($(VAR))))

define COMPILE
	$($(1)) -c -o $@ $<
endef

define COMPILE_EXECUTE
	$($(1)) $(CFLAG) -o $@ $< $(LIB)
endef

COMPILE_C = $(call COMPILE,CC)
COMPILE_EXE = $(call COMPILE_EXECUTE,CC)

$(PROGRAM) : $(PROGRAM).o
	$(COMPILE_EXE)
	cp $(PROGRAM) ../runtime

%.o : %.c
	$(COMPILE_C)

.PHONY: clean
clean:
	rm -rf *.o $(PROGRAM) ../runtime/$(PROGRAM)