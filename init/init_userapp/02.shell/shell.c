#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main()
{
	char command[255];
	char **environ, **argv;
	for (;;) {
		write(1, "# ", 2);
		int count = read(0, command, 255);
		// /bin/ls\n -> /bin/ls\0
		command[count - 1] = '\0';
		pid_t fork_result = fork();
		if (fork_result == 0) {
			execve(command, argv, environ);
			break;
		} else {
			siginfo_t info;
			waitid(P_ALL, 0, &info, WEXITED);
		}
	}
	_exit(0);
}

