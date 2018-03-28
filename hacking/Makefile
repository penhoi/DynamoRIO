#!/bin/make

include ../buildenv.mk


######## final target is libdynamorio ########

ENCLAVE_NAME 	:= libdynamorio.so
APP_NAME 	:= libapp.so

.PHONY: all checkdirs clean

all: checkdirs $(ENCLAVE_NAME) $(APP_NAME)


######## Dynamorio Settings ########
BLD_DIR		?= build
INC_DIR		:= core/arch/x86 core/unix core/arch core/lib extra extra/include/annotations
INCLUDES	:= $(addprefix -I./,$(INC_DIR))
MODULES 	:= core core/unix core/lib core/arch core/arch/x86 third_party/libgcc
BUILD_DIR   := $(addprefix $(BLD_DIR)/,$(MODULES))

ifeq ($(SGX_ARCH), x86)
	BASIC_AS_FLAGS = -mmnemonic=intel -msyntax=intel -mnaked-reg --32 --noexecstack -g
else
	BASIC_AS_FLAGS = -mmnemonic=intel -msyntax=intel -mnaked-reg --64 --noexecstack -g
endif

BASIC_CC_FLAGS	:= -fno-strict-aliasing -fno-stack-protector -fvisibility=internal  -std=gnu99 -fno-omit-frame-pointer -fno-builtin-strcmp -Wall -Werror -Wwrite-strings -Wno-unused-but-set-variable

DYNAMORIO_AS_FLAGS 	:= $(BASIC_AS_FLAGS)

DYNAMORIO_CC_FLAGS	:= $(BASIC_CC_FLAGS) -fPIC -Ddynamorio_EXPORTS $(INCLUDES)
DYNAMORIO_LDSCRIPT	:= core/dynamorio.ldscript
DYNAMORIO_LD_FLAGS	:= $(BASIC_CC_FLAGS) -fPIC -Xlinker -z -Xlinker now -Xlinker -Bsymbolic -nostdlib -Wl,--entry,_start -Wl,-dT,$(DYNAMORIO_LDSCRIPT) -Wl,--hash-style=both -shared -Wl,-soname,libdynamorio.so


######## Dynamorio Objects ########
DYNAMORIO_AS_SRC	:= core/arch/asm_shared.asm core/arch/x86/x86_shared.asm core/arch/x86/x86.asm
DYNAMORIO_AS_OBJ	:= $(patsubst %,$(BLD_DIR)/%.o,$(DYNAMORIO_AS_SRC))


DYNAMORIO_SRC_DIR	:= $(MODULES)
DYNAMORIO_CC_SRC	:= $(foreach sdir,$(DYNAMORIO_SRC_DIR),$(wildcard $(sdir)/*.c))
DYNAMORIO_CC_OBJ	:= $(patsubst %.c,$(BLD_DIR)/%.o,$(DYNAMORIO_CC_SRC))


$(DYNAMORIO_AS_OBJ): $(BLD_DIR)/%.asm.o: %.asm
	${CPP} -g -fPIC $(INCLUDES)  -DCPP2ASM -E $< -o $(BLD_DIR)/$<.s
	${CMAKE} -Dfile=$(BLD_DIR)/$<.s -P "make/CMake_asm.cmake"
	$(AS) $(DYNAMORIO_AS_FLAGS) -c $(BLD_DIR)/$<.s -o $@


${DYNAMORIO_LDSCRIPT}: make/ldscript.cmake
	${CMAKE} -D outfile=$@ -DCMAKE_LINKER=/usr/bin/ld -DCMAKE_COMPILER_IS_GNUCC=TRUE -DLD_FLAGS=-melf_x86_64 -Dset_preferred=1 -Dreplace_maxpagesize= -Dpreferred_base=0x71000000 -Dadd_bounds_vars=ON -P $<


vpath %.c $(DYNAMORIO_SRC_DIR)


define make-goal
$1/%.o: %.c
	$(CC) $(PLATFORM_DEBUG_FLAGS) $(DYNAMORIO_CC_FLAGS)  -c $$< -o $$@
endef

$(foreach bdir,$(BUILD_DIR),$(eval $(call make-goal,$(bdir))))


######## building libdynamorio ########
$(ENCLAVE_NAME): $(DYNAMORIO_AS_OBJ) $(DYNAMORIO_CC_OBJ) ${DYNAMORIO_LDSCRIPT}
	$(LD) $(PLATFORM_DEBUG_FLAGS) $(DYNAMORIO_LD_FLAGS) -o $@ $(DYNAMORIO_AS_OBJ) $(DYNAMORIO_CC_OBJ)
	#$(CP) $@ $@.full
	#$(OBJCOPY) --only-keep-debug $@ $@.debug
	#$(OBJCOPY) --add-gnu-debuglink=$@.debug $@
	#$(STRIP) -g -x $@


######## building libapp ########
$(APP_NAME): App.c
	$(CC) $(PLATFORM_DEBUG_FLAGS) -o $@ $<  -ldl


checkdirs: $(BUILD_DIR)


$(BUILD_DIR):
	@mkdir -p $@


clean:
	@rm -f .config_* $(ENCLAVE_NAME) $(APP_NAME)
	@rm	-rf $(BUILD_DIR)
	@rm	-rf $(BLD_DIR)
