    .section .text.user
    .globl user_test_entry
user_test_entry:
    li a7, SYS_exit
    li a0, 0
    ecall
1:  j 1b
