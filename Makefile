# Bare-bones makefile that compiles with just sshell.c and removes the executable with clean

# Make
sshell: sshell.c
	gcc -Wall -Werror -Wextra sshell.c -o sshell

# Remove executable
clean:
	rm -f sshell
