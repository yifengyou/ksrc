#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main()
{
	char command[255];
	for (;;) {
		write(1, "# ", 2);
		int count = read(0, command, 255);
		// /bin/ls\n -> /bin/ls\0
		command[count - 1] = '\0';
		pid_t fork_result = fork();
		if (fork_result == 0) {
			execve(command, 0, 0);
			break;
		} else {
			// wait
			// pid_t waitpid(pid_t pid, int *wstatus, int options);
			siginfo_t info;
			waitid(P_ALL, 0, &info, WEXITED);
		}
	}
	return 0;
}
