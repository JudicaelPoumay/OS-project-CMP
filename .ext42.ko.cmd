cmd_/usr/src/ext42/ext42.ko := ld -r -m elf_i386 -T ./scripts/module-common.lds --build-id  -o /usr/src/ext42/ext42.ko /usr/src/ext42/ext42.o /usr/src/ext42/ext42.mod.o
