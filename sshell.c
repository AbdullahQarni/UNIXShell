#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CMDLINE_MAX 512
#define TKN_MAX 32
#define ARGS_MAX 16
#define PIPED_CMD_MAX 4

// EXECUTION
struct Command
{
	// Arguments are to be used for exec().
	// arguments[0] is the command name.
	char arguments[ARGS_MAX + 1][TKN_MAX];
	int num_args;
	// The file name and FD to which output goes.
	char output_name[TKN_MAX];
	int output_dest;
	// Does the command write stdout to a file?
	int output_to_file;
	// Does the command write stderr to a file/pipe?
	int err_to_file;
	int err_to_pipe;
	// Reported to stderr at the end of execution.
	int exit_status;
};

// Data set of Command objects.
struct CommandSet
{
	struct Command commands[PIPED_CMD_MAX];
	int num_cmd;
};

// Data set to keep track of pipelines.
struct PipeEnv
{
	int pipes[PIPED_CMD_MAX - 1][2];
	int num_pipes;
};

// Create pipelines based on the number of pipes the command has.
void OpenPipes(struct PipeEnv *pipeSet)
{
	for (int i = 0; i < pipeSet->num_pipes; i++)
	{
		pipe(pipeSet->pipes[i]);
	}
}

// Cleanup function to close all pipe FDs.
void ClosePipes(struct PipeEnv *pipeSet)
{
	for (int i = 0; i < pipeSet->num_pipes; i++)
	{
		close(pipeSet->pipes[i][0]);
		close(pipeSet->pipes[i][1]);
	}
}

// Executes the information in a Command object.
// REF: fork-exec-wait.c, dup2.c
void RunCommand(struct Command *cmd)
{
	// Set up arguments for exec function.
	char *args[1 + cmd->num_args];
	for (int i = 0; i < cmd->num_args + 1; i++)
	{
		if (i == cmd->num_args) {
			args[i] = NULL;
		} else {
			args[i] = cmd->arguments[i];
		}
	}	
	
	if (cmd->output_to_file) 
	{
		cmd->output_dest = open(cmd->output_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(cmd->output_dest, STDOUT_FILENO);
		// >&, connect STDERR as well.
		if (cmd->err_to_file) dup2(cmd->output_dest, STDERR_FILENO);
		close(cmd->output_dest);
	}

	// Actual execution of command. Fork ends here.
	execvp(cmd->arguments[0], args);
	// If still here, exec failed due to invalid command name.
	fprintf(stderr, "Error: command not found\n");
	exit(1);
}

// Runs all Commands in a CommandSet after setting up pipes.
// REF: fork-exec-wait.c, "Process pipeline example" (Syscalls p. 37)
void RunAllCmd(struct CommandSet *allCmd, struct PipeEnv *pipeSet)
{
	// Identifier for forks to realize what number command they are.
	int cmd_order;
	// Array of fork ids, forkId[x] = 0 for command x's fork.
	pid_t forkIds[4];

	OpenPipes(pipeSet);

	// Create a child for every command.
	for (int i = 0; i < allCmd->num_cmd; i++)
	{
		cmd_order = i;
		forkIds[i] = fork();
		// Prevents children from forking.
		if (forkIds[i] == 0) break;
	}

	if (forkIds[cmd_order] == 0)
	{
		// Child: connect the requisite pipes then run command.
		// Set write pipe FD except for last command.
		if (allCmd->num_cmd - 1 > cmd_order)
		{
			dup2(pipeSet->pipes[cmd_order][1], STDOUT_FILENO);
		}

		// Set read pipe FD except for first command.
		if (cmd_order > 0)
		{
			dup2(pipeSet->pipes[cmd_order - 1][0], STDIN_FILENO);
		}

		// Set stderr to write pipe FD if requested.
		if (allCmd->commands[cmd_order].err_to_pipe)
		{
			dup2(pipeSet->pipes[cmd_order][1], STDERR_FILENO);
		}
		
		// Close misc pipe FDs and pipe FDs no longer in use.
		ClosePipes(pipeSet);
		RunCommand(&allCmd->commands[cmd_order]);
	} else {
		// Parent: Wait for every child in order of FIFO.
		// Then save exit status to all Command Objects.
		// REF: Piazza @63 (thanks professor!)
		ClosePipes(pipeSet);
		for (int j = 0; j < allCmd->num_cmd; j++)
		{
			int status;
			waitpid(forkIds[j], &status, 0);
			allCmd->commands[j].exit_status = WEXITSTATUS(status);	
		}
	}
}

// PARSING
// ParseModes determine behavior of parser when encountering certain symbols.
enum ParseModes {
	// Building the argument of a command.
	SEARCH_COMMAND,
	// Building an output filename.
	SEARCH_FILENAME
};

enum ParsingErrors
{
	MISSING_TOKEN, // Missing output file or command
	BAD_FILE, // Can't open file
	ARG_OVERFLOW, // Too many arguments
	PIPE_OVERFLOW, // Too many pipes
	MISLOCATED_REDIRECT // Mislocated output
};

// Parsing error reporting system.
// REF: "project1.pdf" slide 34.
void ParsingError(int error_code, int read_mode) 
{
	switch(error_code)
	{
		case MISSING_TOKEN:
			if (read_mode == SEARCH_COMMAND) fprintf(stderr, "Error: missing command\n");
			if (read_mode == SEARCH_FILENAME) fprintf(stderr, "Error: no output file\n");
			break;
		case BAD_FILE:
			fprintf(stderr, "Error: cannot open output file\n");
			break;
		case ARG_OVERFLOW:
			fprintf(stderr, "Error: too many process arguments\n");
			break;
		case PIPE_OVERFLOW:
			fprintf(stderr, "Error: too many pipes\n");
			break;
		case MISLOCATED_REDIRECT:
			fprintf(stderr, "Error: mislocated output redirection\n");
			break;
		default:
			break;
	}
}

// Checks that a particular file can be opened.
int VerifyFile(char *filename)
{
	int open_file = 0;
	open_file = open(filename, O_WRONLY | O_CREAT, 0644);
	if (open_file == -1)
	{
		ParsingError(BAD_FILE, 0);
		return 0;
	}
	close(open_file);
	return 1;
}

// Copies a token string to a target string location.
int CopyToken(char *segment, char *target, int *length, int read_mode)
{
	// If no token was read, raise a missing command/file error.
	if (*length == 0)
	{
		ParsingError(MISSING_TOKEN, read_mode);
		return 0;
	}

	// If token is a file name, verify that the file can be opened.
	if (read_mode == SEARCH_FILENAME)
		if (!VerifyFile(segment)) return 0;

	// Copy to target then flush the token.
	strcpy(target, segment);
	// REF: https://stackoverflow.com/questions/1735919/whats-the-best-way-to-reset-a-char-in-c
	memset(segment, '\0', TKN_MAX);
	*length = 0;
	return 1;
}

// Initialize Command Object values.
void InitCommand(struct Command *cmd)
{
	cmd->output_dest = STDOUT_FILENO;
	strcpy(cmd->output_name, " ");
	cmd->output_to_file = 0;
	cmd->err_to_pipe = 0;
	cmd->err_to_file = 0;
	cmd->num_args = 0;
	cmd->exit_status = 0;
}

// Runs through the command line character by character, splitting it into Command Objects.
// Works by building a read string then copying it to a piece of a Command Object.
int ParseCmd(struct CommandSet *allCmd, struct PipeEnv *pipeSet, char *cmd)
{
	// Flushable token to be copied to a target location.
	char segment[TKN_MAX];
	char *target;
	int length = 0;

	// "Reading mode" to determine where fully read tokens go.
	int read_mode = SEARCH_COMMAND;

	// If the parser runs through whitespace while looking for a command.
	int encounter_whitespace = 0;
	int init_skip = 1;

	// Initialize Command Set and Pipe Environment variables.
	// Initial target is the first argument of the first Command.
	allCmd->num_cmd = 0;
	pipeSet->num_pipes = 0;
	InitCommand(&allCmd->commands[allCmd->num_cmd]);
	target = allCmd->commands[allCmd->num_cmd].arguments[0];

	// Initialize the parsing token.
	// REF: https://stackoverflow.com/questions/1735919/whats-the-best-way-to-reset-a-char-in-c
	memset(segment, '\0', TKN_MAX);

	for (int i = 0; i < (int) strlen(cmd); i++)
	{
		char read_char = cmd[i];
		switch(read_char)
		{
			case '|':
				// Attempt to copy token to target location.
				if (!CopyToken(segment, target, &length, read_mode)) return 1;
				if (read_mode == SEARCH_COMMAND) allCmd->commands[allCmd->num_cmd].num_args++;

				// Make sure within 16 argument threshold.
				if (allCmd->commands[allCmd->num_cmd].num_args > ARGS_MAX)
				{
					ParsingError(ARG_OVERFLOW, 0);
					return 1;
				}

				// If current command has a file output and now trying to pipe, raise mislocation error.
				if (allCmd->commands[allCmd->num_cmd].output_to_file)
				{
					ParsingError(MISLOCATED_REDIRECT, 0);
					return 1;
				}

				// Make sure within number of pipes given by project specs.
				pipeSet->num_pipes++;
				if (pipeSet->num_pipes > PIPED_CMD_MAX - 1)
				{
					ParsingError(PIPE_OVERFLOW, 0);
					return 1;
				}

				// If the symbol was actually "|&" indicate that stderr needs to be piped.
				if (i < (int) strlen(cmd) - 1)
				{
					if (cmd[i + 1] == '&')
					{
						allCmd->commands[allCmd->num_cmd].err_to_pipe = 1;
						i++;
					}
				} 				

				// Define and set up new target. 
				// First argument of the next Command object in the CommandSet.
				allCmd->num_cmd++;
				InitCommand(&allCmd->commands[allCmd->num_cmd]);
				target = allCmd->commands[allCmd->num_cmd].arguments[
					allCmd->commands[allCmd->num_cmd].num_args];

				// Start looking for the first argument of a new Command.
				encounter_whitespace = 0;
				init_skip = 1;
				read_mode = SEARCH_COMMAND;
				break;
			case '>':
				// Attempt to copy token to target location.
				if (!CopyToken(segment, target, &length, read_mode)) return 1;
				if (read_mode == SEARCH_COMMAND) allCmd->commands[allCmd->num_cmd].num_args++;

				// Make sure within 16 argument threshold.
				if (allCmd->commands[allCmd->num_cmd].num_args > ARGS_MAX)
				{
					ParsingError(ARG_OVERFLOW, 0);
					return 1;
				}

				// Set new target as the output filename of current Command.
				target = allCmd->commands[allCmd->num_cmd].output_name;
				allCmd->commands[allCmd->num_cmd].output_to_file = 1;

				// If the symbol is actually ">&", indicate to output stderr to a file.
				if (i < (int) strlen(cmd) - 1)
				{
					if (cmd[i + 1] == '&')
					{
						allCmd->commands[allCmd->num_cmd].err_to_file = 1;
						i++;
					}
				} 

				// Start looking for a file name.
				encounter_whitespace = 0;
				read_mode = SEARCH_FILENAME;
				break;
			case ' ':
				if (read_mode == SEARCH_COMMAND) encounter_whitespace = 1; 
				// Only count whitespace for filename search if build process has started.
				if (read_mode == SEARCH_FILENAME && length > 0) {
					encounter_whitespace = 1;
				}
				break;
			default:
				// Encountered the first character of a new token (token___token).
				// init_skip prevents command argument copies for cases like ____(token).
				if (encounter_whitespace && !init_skip)
				{
					// Attempt to copy token to target location.
					if (!CopyToken(segment, target, &length, read_mode)) return 1;
					if (read_mode == SEARCH_COMMAND) allCmd->commands[allCmd->num_cmd].num_args++;

					// Make sure within 16 argument threshold.
					if (allCmd->commands[allCmd->num_cmd].num_args > ARGS_MAX)
					{
						ParsingError(ARG_OVERFLOW, 0);
						return 1;
					}
					// Assign new target to next argument of the Command.
					target = allCmd->commands[allCmd->num_cmd].arguments[
						allCmd->commands[allCmd->num_cmd].num_args];
					read_mode = SEARCH_COMMAND;
				}
				init_skip = 0;
				encounter_whitespace = 0;
				// Add the character to the token and keep reading.
				segment[length] = read_char;
				length++;
				break;
		}			
	}

	// End of parsing, handle any hanging text.
	// The command has a hanging meta-character, or the user entered nothing.
	if (length == 0) {
		if (allCmd->commands[0].num_args > 0) ParsingError(MISSING_TOKEN, read_mode);
		return 1;
	}

	// Otherwise, copy the hanging text to the target and update final parameters.
	if (!CopyToken(segment, target, &length, read_mode)) return 1;
	if (read_mode == SEARCH_COMMAND) allCmd->commands[allCmd->num_cmd].num_args++;
	// Make sure within 16 argument threshold.
	if (allCmd->commands[allCmd->num_cmd].num_args > ARGS_MAX)
	{
		ParsingError(ARG_OVERFLOW, 0);
		return 1;
	}
	allCmd->num_cmd++;
	return 0;
}

// MISC BUILTIN
// Special ls that prints all filenames and byte size of current directory.
// REF: dir_scan.c, stat.c, "Syscalls" slides 32-33.
int sls()
{
	DIR *current_dir;
	struct dirent *directory_file;
	struct stat file_info;

	// Open the current directory and get its first file.
	current_dir = opendir(".");
	// Account for directories that lack permissions.
	if (current_dir == NULL)
	{
		fprintf(stderr, "Error: cannot open directory\n");
		return 1;
	}
	directory_file = readdir(current_dir);
	while (directory_file != NULL)
	{
		// Skip ".", "..", and all hidden files and folders.
		if (directory_file->d_name[0] != '.')
		{
			// Get the file's information in a readable form.
			stat(directory_file->d_name, &file_info);
			// Print filename then size in bytes.
			printf("%s (%d bytes)\n", directory_file->d_name, (int) file_info.st_size);
		}
		// Get the next file until all files have been read.
		directory_file = readdir(current_dir);
	}
	return 0;
}

int main(void)
{
	char cmd[CMDLINE_MAX];
	char current_dir[CMDLINE_MAX];
	struct CommandSet CommandCenter;
	struct PipeEnv PipeManager;

	while (1) {
		char *nl;

		// Print prompt
		printf("sshell@ucd$ ");
		fflush(stdout);

		// Get command line
		fgets(cmd, CMDLINE_MAX, stdin);

		// Print command line if stdin is not provided by terminal.
		if (!isatty(STDIN_FILENO)) {
			printf("%s", cmd);
			fflush(stdout);
		}

		// Remove trailing newline from command line.
		nl = strchr(cmd, '\n');
		if (nl)
			*nl = '\0';
	
		// Begin parsing of the command line.
		int parse_failure = ParseCmd(&CommandCenter, &PipeManager, cmd);
		// If no parsing errors, result is a set of Commands to execute.
		if (!parse_failure)
		{
			// Check the first command to detect builtin commands.
			struct Command *FirstCommand = &CommandCenter.commands[0];
			char *first_command_name = FirstCommand->arguments[0];
			// Builtin commands
			if (!strcmp(first_command_name, "exit")) {
				fprintf(stderr, "Bye...\n");
				break;
			} else if (!strcmp(first_command_name, "cd")) {
				FirstCommand->exit_status = chdir(FirstCommand->arguments[1]);
				if (FirstCommand->exit_status) 
				{
					fprintf(stderr, "Error: cannot cd into directory\n");
					FirstCommand->exit_status = 1;
				}
			} else if (!strcmp(first_command_name, "pwd")) {
				getcwd(current_dir, sizeof(current_dir));
				printf("%s\n", current_dir);
				FirstCommand->exit_status = 0;
			} else if (!strcmp(first_command_name, "sls")) {
				FirstCommand->exit_status = sls();
			} else {
				// Execute regular commands.
				RunAllCmd(&CommandCenter, &PipeManager);
			}
			// Completed message.
			fprintf(stderr, "+ completed '%s' ", cmd);
			for (int i = 0; i < CommandCenter.num_cmd; i++) fprintf(stderr, "[%d]", 
				CommandCenter.commands[i].exit_status);
			fprintf(stderr, "\n");
		}	
	}
	fprintf(stderr, "+ completed '%s' [%d]\n", cmd, 0);
	return EXIT_SUCCESS;
}
