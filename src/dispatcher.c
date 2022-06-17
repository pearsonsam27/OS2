#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dispatcher.h"
#include "shell_builtins.h"
#include "parser.h"
#include <sys/wait.h>
#include <fcntl.h>

/**
 * dispatch_external_command() - run a pipeline of commands
 *
 * @pipeline:   A "struct command" pointer representing one or more
 *              commands chained together in a pipeline.  See the
 *              documentation in parser.h for the layout of this data
 *              structure.  It is also recommended that you use the
 *              "parseview" demo program included in this project to
 *              observe the layout of this structure for a variety of
 *              inputs.
 *
 * Note: this function should not return until all commands in the
 * pipeline have completed their execution.
 *
 * Return: The return status of the last command executed in the
 * pipeline.
 */

static int pipelineHandler(struct command *pipeline)
{
	int status;
	int prevPipe = STDIN_FILENO;
	int pfds[2];

	pipe(pfds);
	pid_t pidPipe = fork();

	if (pidPipe < 0) {
		fprintf(stderr, "fork error\n");
		exit(-1);
	}

	if (pidPipe == 0) { // I am a child

		if (pipeline->input_filename) {
			prevPipe =
				open(pipeline->input_filename, O_RDONLY, 0644);

			if (prevPipe == -1) {
				fprintf(stderr,
					"error: cannot open file/bad permission '%s'\n",
					pipeline->input_filename);
				exit(1);
			}
		}

		if (prevPipe != STDIN_FILENO) {
			dup2(prevPipe, STDIN_FILENO);
			close(prevPipe);
		}

		if (pipeline->pipe_to->output_filename) {
			if (pipeline->pipe_to->output_type ==
			    COMMAND_OUTPUT_FILE_APPEND) { //Append
				pfds[1] = open(
					pipeline->pipe_to->output_filename,
					O_WRONLY | O_CREAT | O_APPEND, 0644);

				if (pfds[1] == -1) {
					fprintf(stderr,
						"error: cannot open file/bad permission '%s'\n",
						pipeline->pipe_to
							->output_filename);
					exit(1);
				}
			}
			if (pipeline->pipe_to->output_type ==
			    COMMAND_OUTPUT_FILE_TRUNCATE) { //Overwrite

				pfds[1] = open(
					pipeline->pipe_to->output_filename,
					O_WRONLY | O_CREAT | O_TRUNC, 0644);

				if (pfds[1] == -1) {
					fprintf(stderr,
						"error: cannot open file/bad permission '%s'\n",
						pipeline->pipe_to
							->output_filename);
					exit(1);
				}
			}
		}
		dup2(pfds[1], STDOUT_FILENO);
		close(pfds[1]);
		execvp(pipeline->argv[0], pipeline->argv);
	}

	if (wait(&status) != pidPipe) { //Unknown cmd
		exit(status);
		return (status);
	};

	if (status != 0) { //child exited ok, but not return 0
		return (status);
	}

	close(prevPipe);
	close(pfds[1]);
	prevPipe = pfds[0];

	if (prevPipe != STDIN_FILENO) {
		dup2(prevPipe, STDIN_FILENO);
		close(prevPipe);
	}

	int check = execvp(pipeline->pipe_to->argv[0], pipeline->pipe_to->argv);
	if (check != 0) {
		fprintf(stderr, "2nd cmd failed: %d\n", WEXITSTATUS(status));
		exit(1);
	}
	return -1;
}

static int dispatch_external_command(struct command *pipeline)
{
	pid_t pid = fork();
	int status;

	if (pid < 0) { //I am a child
		fprintf(stderr, "fork error\n");
		exit(-1);
	} else if (pid == 0) {
		if (pipeline->output_type == COMMAND_OUTPUT_PIPE) {
			int pipeStatus = pipelineHandler(pipeline);

			if (pipeStatus != 0) { //pipe fail
				fprintf(stderr, "1st cmd failed or bad file\n");
				return WEXITSTATUS(pipeStatus);
			};
		} else {
			if (pipeline->input_filename) {
				int input = open(pipeline->input_filename,
						 O_RDONLY, 0644);

				if (input == -1) {
					fprintf(stderr,
						"error: cannot open file/bad permission '%s'\n",
						pipeline->input_filename);
					return 1;
				}
				dup2(input, STDIN_FILENO);
			}

			if (pipeline->output_filename) {
				if (pipeline->output_type ==
				    COMMAND_OUTPUT_FILE_APPEND) { //Append
					int outputAppend = open(
						pipeline->output_filename,
						O_WRONLY | O_CREAT | O_APPEND,
						0644);

					if (outputAppend == -1) {
						fprintf(stderr,
							"error: cannot open file/bad permission '%s'\n",
							pipeline->output_filename);
						return 1;
					}
					dup2(outputAppend, STDOUT_FILENO);
				}
				if (pipeline->output_type ==
				    COMMAND_OUTPUT_FILE_TRUNCATE) { //Overwrite

					int outputTrunc = open(
						pipeline->output_filename,
						O_WRONLY | O_CREAT | O_TRUNC,
						0644);

					if (outputTrunc == -1) {
						fprintf(stderr,
							"error: cannot open file/bad permission '%s'\n",
							pipeline->output_filename);
						return 1;
					}
					dup2(outputTrunc, STDOUT_FILENO);
				}
			}

			execvp(pipeline->argv[0], pipeline->argv);
		}
	}

	if (wait(&status) != pid) { //Unknown cmd
		fprintf(stderr, "Unknown Command: %d\n", WEXITSTATUS(status));
		return WEXITSTATUS(status);
	};

	if (status != 0) { //child exited ok, but not return 0
		fprintf(stderr, "child function failed, status: %d\n",
			WEXITSTATUS(status));
		return WEXITSTATUS(status);
	}

	return WEXITSTATUS(status);
}

/**
 * dispatch_parsed_command() - run a command after it has been parsed
 *
 * @cmd:                The parsed command.
 * @last_rv:            The return code of the previously executed
 *                      command.
 * @shell_should_exit:  Output parameter which is set to true when the
 *                      shell is intended to exit.
 *
 * Return: the return status of the command.
 */
static int dispatch_parsed_command(struct command *cmd, int last_rv,
				   bool *shell_should_exit)
{
	/* First, try to see if it's a builtin. */
	for (size_t i = 0; builtin_commands[i].name; i++) {
		if (!strcmp(builtin_commands[i].name, cmd->argv[0])) {
			/* We found a match!  Run it. */
			return builtin_commands[i].handler(
				(const char *const *)cmd->argv, last_rv,
				shell_should_exit);
		}
	}

	/* Otherwise, it's an external command. */
	return dispatch_external_command(cmd);
}

int shell_command_dispatcher(const char *input, int last_rv,
			     bool *shell_should_exit)
{
	int rv;
	struct command *parse_result;
	enum parse_error parse_error = parse_input(input, &parse_result);

	if (parse_error) {
		fprintf(stderr, "Input parse error: %s\n",
			parse_error_str[parse_error]);
		return -1;
	}

	/* Empty line */
	if (!parse_result)
		return last_rv;

	rv = dispatch_parsed_command(parse_result, last_rv, shell_should_exit);
	free_parse_result(parse_result);
	return rv;
}
