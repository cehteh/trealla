#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <readline/readline.h>
#include <readline/history.h>
#include "history.h"
#include "utf8.h"

#include <termios.h>
#include <unistd.h>

int history_getch(void)
{
	struct termios oldattr, newattr;
	tcgetattr(STDIN_FILENO, &oldattr);
	newattr = oldattr;
	newattr.c_lflag &= ~(ICANON | ECHO);
	tcsetattr(STDIN_FILENO, TCSANOW, &newattr);
	int ch = fgetc_utf8(stdin);
	tcsetattr(STDIN_FILENO, TCSANOW, &oldattr);
	return ch;
}

char *history_readline_eol(const char *prompt, char eol)
{
	char *cmd = NULL;
	char *line;

LOOP:

	if ((line = readline(prompt)) == NULL)
		return NULL;

	const char *s = line;

	if (cmd) {
		size_t n = strlen(cmd) + strlen(line);
		cmd = realloc(cmd, n+1);
		strcat(cmd, line);
	} else {
		cmd = line;
	}

	while (1) {
		int ch = get_char_utf8(&s);

		if ((ch == 0) && (line[strlen(line)-1] == eol)) {
			add_history(cmd);
			break;
		}

		if (ch == 0) {
			//*dst++ = '\n';
			prompt = " |\t";
			goto LOOP;
		}
	}

	return cmd;
}

static char g_filename[1024];

void history_load(const char *filename)
{
	snprintf(g_filename, sizeof(g_filename), "%s", filename);
	using_history();
	read_history(g_filename);
}

void history_save(void)
{
	write_history(g_filename);
}
