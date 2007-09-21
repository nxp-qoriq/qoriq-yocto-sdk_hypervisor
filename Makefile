#
# Copyright � 2007 Freescale Semiconductor, Inc
# Copyright � 1999 Paul D. Smith
#
#
# The env var $CROSS_COMPILE should be set to powerpc-unknown-linux-gnu-
#

CC=$(CROSS_COMPILE)gcc
#CC_OPTS=-m32 -nostdinc -Wa,-me500
CC_OPTS=-m32 -Wa,-me500 -Iinclude
CC_OPTS_C= -Wall \
  -Wundef \
  -Wstrict-prototypes \
  -Wno-trigraphs \
  -fno-strict-aliasing \
  -fno-common \
  -O2 \
  -msoft-float \
  -pipe \
  -ffixed-r2 \
  -mmultiple \
  -mno-altivec \
  -funit-at-a-time \
  -mno-string \
  -fomit-frame-pointer \
  -Wdeclaration-after-statement
#CC_OPTS_ASM=
LD=$(CROSS_COMPILE)ld
LD_OPTS=-m elf32ppc -Bstatic
GENASSYM=tools/genassym.sh

OBJS = src/head.o src/exceptions.o src/interrupts.o

SRCS_C = src/genassym.c src/interrupts.c
SRCS_S = src/head.S src/exceptions.S


uv.uImage: uv.bin
	mkimage -A ppc -O linux -T kernel -C none -a 00000000 -e 00000000 -d $< $@

#	mkimage -A ppc -O linux -T standalone -C gzip -a 00000000 -e 00000000 -d $< $@
#	mkimage -A ppc -O linux -T kernel -C gzip -a 00000000 -e 00000000 -d $< $@
#	mkimage -A ppc -O linux -C none -T standalone -a 00000000 -e 00000000 -d $< $@


uv.bin.gz: uv.bin
	gzip -f $<

uv.bin: uv
	$(CROSS_COMPILE)objcopy -O binary $< $@

uv: $(OBJS)
	$(LD) $(LD_OPTS) -o $@ $(OBJS)
#	$(LD) $(LD_OPTS) -T uv.lds -o $@ $(OBJS)

# compile and gen dependecy file
%.o : %.c
	$(CC) -MD $(CC_OPTS) $(CC_OPTS_C) -c -o $@ $<
	@cp $*.d $*.P; \
	sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
		-e '/^$$/ d' -e 's/$$/ :/' < $*.d >> $*.P; \
	rm -f $*.d

# assemble and gen dependecy file
%.o : %.S src/assym.s
	$(CC) -MD $(CC_OPTS) $(CC_OPTS_ASM) -c -o $@ $<
	@cp $*.d $*.P; \
	sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
		-e '/^$$/ d' -e 's/$$/ :/' < $*.d >> $*.P; \
	rm -f $*.d

src/genassym.o: src/genassym.c
	$(CC) -MD $(CC_OPTS) -c -o $@ $<
	@cp $*.d $*.P; \
	sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
		-e '/^$$/ d' -e 's/$$/ :/' < $*.d >> $*.P; \
	rm -f $*.d

src/assym.s: src/genassym.o
	$(GENASSYM) -o $@ $<

# include the dependecy files
-include $(SRCS_C:.c=.P)
-include $(SRCS_S:.S=.P)
 
clean:
	rm -f uv src/*.o src/*.P src/*.d *.bin *.uImage *.gz src/assym.s
