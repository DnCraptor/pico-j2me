/* The class library (build/classlib.jar), linked into flash. */
    .section .rodata.classlib, "a"
    .balign 4
    .global classlib_bin
    .global classlib_bin_end
classlib_bin:
    .incbin CLASSLIB_JAR
classlib_bin_end:
    .byte 0
