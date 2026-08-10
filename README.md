# Unix-Like Operating System

This project is a small Unix-like operating system built for the 64-bit RISC-V ISA. It provides an interactive shell for launching user programs and issuing commands. The kernel handles requests such as file access through system calls, while preemptive scheduling allows multiple processes to coexist. Each process runs independently, keeping user code isolated from privileged kernel execution.

The kernel is written primarily in C modules that manage higher-level behavior like device driver operations and process execution. RISC-V assembly is leveraged to manage processor state during low-level operations such as trap handling or transitions between user and supervisor mode.

## Demo

https://github.com/user-attachments/assets/9c96a547-1c42-4f29-a086-7240fca30e08

## Demo Walkthrough

The demo begins by entering the operating system directory, and running the `usr` Makefile, which compiles the user program files and provided Star Trek game. A KTFS filesystem image is then created containing the newly compiled `usr/games/*` and `usr/progs/*` files. The `sys` Makefile is then run, which launches the kernel with the `uart0` device as its console. A second terminal attaches to the kernel as an input/output device connected to `uart0`.

`ls` is then used to show the current paths available in the filesystem, including available devices, commands, and programs. `hello` is run to demonstrate basic program functionality, followed by `date`, which retrieves the current date and time from the `rtc0` device driver. `echo` is then used to create a file and write a message into it before `exit` shuts down the kernel. After restarting and reattaching to the kernel, the previously created file is shown to have persisted in KTFS through the shutdown and reboot.

Running `wc` counts the lines, words, and bytes in the file, and rerunning the operation using `cat` and `|` demonstrates pipe functionality. The pipe connects the output of `cat` directly to the input of `wc`, allowing data to pass between the two processes without using an intermediate file. The provided `trek` game is then run and exited cleanly, showcasing the kernel’s ability to handle a more complex program that utilizes the UART, RTC, and VirtIO RNG device drivers simultaneously.

The filesystem is then stress tested by adding and removing three files at once while also demonstrating `xargs` and additional pipe functionality. Finally, an invalid argument is tested to show that the kernel can cleanly handle incorrect inputs.

## Overview

The system runs in QEMU, which emulates a computer implementing the 64-bit RISC-V ISA. This allows the operating system to boot and run in a virtual environment without requiring a physical machine with a RISC-V processor. In the demo, programs such as `hello` and `trek` are loaded from the filesystem and executed as user processes, with control returning to the shell after each program finishes. The shell also demonstrates piping and redirection, showing that program output can be routed directly into another process or stored in a file instead of simply being printed. These commands make the interaction between user processes and kernel services visible rather than only showing that individual programs can launch.

See [Architecture](docs/architecture.md) for a deeper explanation of the kernel design.

## Supported Shell Commands

| Command | Description                                                                        |
| ------- | ---------------------------------------------------------------------------------- |
| `echo`  | Writes command-line arguments to standard output                                   |
| `cat`   | Reads a file and writes its contents to standard output                            |
| `ls`    | Lists filesystem entries, devices, or mounted filesystems                          |
| `wc`    | Counts lines, words, and bytes from a file or standard input                       |
| `date`  | Displays the current date and time after opening the real time clock device driver |
| `touch` | Creates one or more empty files                                                    |
| `rm`    | Removes files from the filesystem                                                  |
| `xargs` | Reads arguments from standard input and supplies them to another program           |
| `hello` | Runs the hello user program                                                        |
| `trek`  | Runs the trek user program                                                         |
| `<`     | Redirects a program's standard input from a file                                   |
| `>`     | Redirects a program's standard output to a file                                    |
| `\|`    | Connects the standard output of one process to the standard input of another       |

Pipes and redirection use the same file-descriptor and uniform I/O interface as the rest of the kernel rather than being implemented as special cases in the shell.

## Verification

Development combined targeted component tests with full system testing under QEMU, while GNU GDB was used to trace failures between C and assembly code. Many of the hardest bugs appeared only after otherwise functional subsystems were integrated, including a forked child losing its process identity and additonal issues involving pathname parsing and piped user input. See [Verification](docs/verification.md) for more.

## Repository Structure

| Directory | Contents                                                      |
| --------- | ------------------------------------------------------------- |
| `src/`    | Provided and modified source code |
| `docs/`   | Architecture and verification documentation                   |
| `images/` | Diagrams and screenshots                                      |
| `media/`  | Demo video                                                    |

## Scope

This project was developed as part of a university course using a provided kernel framework. The `src/` directory distinguishes provided files from those implemented or modified for the project. This repository is intended for portfolio review and should not be reused in any context that could violate academic integrity.
