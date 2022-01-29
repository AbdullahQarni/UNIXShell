# Project 1: Simple Shell
## Overview
The goal of this project was to implement a shell that responds to unix
commands using syscalls and forking logic. The logic of the process breaks down
into 4 main steps.
1. Parse the command line and break or sort it into representations of a
singular "command", while also checking for errors in the input.
2. Handle "command" communications and the medium to which output is displayed.
3. Properly execute each "command" in a concurrent fashion.
4. Record the results of the execution and relay it back to the user.

Individual "commands" are represented as `struct Command`, which store an array
of arguments, integers representing exit status and output file descriptors,
and flags that determine output redirection. A `struct CommandSet` handles an
array of `Command` objects. Each `Command` of a `CommandSet` is individually
built and initialized through the parsing process.

## Parsing
There were two main options to choose from when parsing the command line.
1. A top-down approach that divides the command line using `strtok`.
2. A bottom-up approach that reads the command line character by character.

Initial builds utilized the `strtok` method. Splitting the command line in
order of `|`, `>`, then whitespace had an identifiable logic that could be
easily translated into code. Later builds abandoned this due to a lack of error
logic. Detecting errors in leftmost order was not feasible. Edge cases such as
`||` and `>>`, missing file cases, and detecting `>&` and `|&` required 
significant redirection of control flow to account for every case. The build was
also not efficient due to requiring separate loops for each meta-character.

Later builds instead employed the single character parser, as it granted
greater control over the process. `ParseCmd` utilizes a single loop over the
entire command line to build `Command` objects. Individual characters are added
to a temporary token. Then, upon encountering any of the meta-characters or the
first character of a new token, the current token is copied to a `target` that
points to a certain element of a `Command`. This `target` is determined by the
meta-character that was encountered. For example, `|` means the next `target`
should be changed to the first argument of the next `Command` within the
`Commandset`. `>` changes the `target` to the output file name of the current
`Command`. The parser also flips flags on the `Command` indicating output needs
to be redirected before execution. If the `target` is an argument of a
`Command` and the parser encounters the first character of a new token, it
copies the current token to the argument, makes the new target the next
argument of the `Command`, and then reads the new token. A `read_mode` value
keeps track of whether an argument or a file name is being built.

Error reporting was freer and could be done in a leftmost-first order. For
example, if the parser tries to copy an empty token to the target, there
is clear logic that something is missing and a `MISSING_TOKEN` error can be
thrown. In contrary, if the token was not empty and is a file name, the parser
can then verify that the file can be opened or throw `BAD_FILE`. Then, if the
parser encounters `|`, it had previously turned on the flag for the current
`Command`'s output redirection and would raise `MISLOCATED_REDIRECT`. On the
other hand, if the token is the 17th argument of the current command,
`ARGS_OVERFLOW` can be raised instead.

Ultimately this implementation provides greater freedom and only needing a
single loop. However it comes at the expense of demanding code not easily
translated from logic. The code is less readable as a result.

## Execution
Execution is a step-by-step process simplified as fork, set output, exec, then
record exit values.

A loop is used to create forks for every `Command`. The fact a child's own fork
`pid_t` value appears as 0 to itself allows for the establishment of conditions
for the children to prematurely break from the loop while the parent does not.
Concurrent forks are achieved as the parent shell does not start waiting until
the loop is completed and has forked for every `Command` in the `CommandSet`. 

Each fork is given a `cmd_order` value equal to the index of the loop it was in
before it broke out. This `cmd_order` acts as an identifier that the child uses
to get information on which `Command` it is handling within the `CommandSet`.
The identifier also determines which pipes the child will connect to. For
example, in a 3-pipe, 4-command situation, the fork with `cmd_order` 2 will
connect its `stdout` to the write fd of the pipe handling `cmd_order` 2 and
`cmd_order` 3 and its `stdin` to the read fd of the pipe handling `cmd_order` 1
and `cmd_order` 2.

After the pipes are connected, the child redirects any output to files if the
parser changed flags on the `Command` object it handles. Then, the child calls
`exec` on its `Command` and dies. The waiting parent shell saves the exit value
of each child to the `Command` object it was ordered to handle (for use in
generating the 'Completed' statement), finishing the execution process.

## Built In
Given a `CommandSet`, hard-coded built in commands are detected from the first
`Command` in the array. If detected, a separate block of code is run instead of
going through the execution process. In this case, the `exit_status` of the
`Command` object has to be manually set instead of through `waitpid`. 

From the man pages of functions like `chdir` and `opendir`, it was discovered
that some of these functions return specific values if an error occurs. This
allowed for more precise error checking, and was also used in the parsing
phase for tests like `open`. In other cases, helper functions returned `int`
instead of being `void` to track successful operations.

## Testing
Functionality was tested thoroughly for every feature before moving on to the
next phase. After the code was "fully functionable," it was passed through the
provided testing script. It was then subjected to random and varied test cases
to detect any undesired output. In most cases this was done by comparing the
output to the reference shell using file output or separate terminals.

## Limitations
Due to the nature of hard-coded builtin commands, there is potential unexpected
behavior if they are used in tandem with pipes or output redirection.

Issues with `strcat` and `strcpy` led to using a manual indexing approach to 
parse and copy individual characters, meaning potential wasted run time.

Most arrays have hard-coded sizes that agree with specification limits. A 
byproduct of this implementation is a hard limit of 3 pipes and 4 commands in 
one command line. An additional error `PIPE_OVERFLOW` was added to catch this
and print a message to `stderr` accordingly. `malloc` and `free` were avoided
due to concerns with inconsistencies when forking or with parsing/launch
errors.

The parser, like the reference, tries to open or create files immediately
after reading file names even if the file never gets used due to errors.
This is part of error checking with leftmost priority. The file is closed
after verification, and is not opened again until the actual command is forked.
We compensated by not truncating the file for the initial error checking.

### References
fork-exec-wait.c, dup2.c, dir-scan.c, stat.c. "Syscalls" slides 32-3, 37. 

Project 1 discussion: "project1.pdf" slide 34.

[strtok](https://www.cplusplus.com/reference/cstring/strtok/).
[Using memset to initialize or flush a character array.](https://stackoverflow.com/questions/1735919/whats-the-best-way-to-reset-a-char-in-c)

Piazza @63. Various man pages.
