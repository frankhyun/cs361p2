#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>

#define BUFSIZE 4096
#define MAX_QTEXT 2048
#define TIMEOUT_SECS 8 // super weird bug, but if you set this to 10 or 9, the server never receives second answer from client. 8 works fine for some reason?

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif /* INADDR_NONE */

int connectsock(char *host, char *service, char *protocol);

static int csock;

/**
 * write all 'len' bytes to csock
 */
static void send_all(const char *buf, int len)
{
	int sent = 0;
	while (sent < len)
	{
		int n = write(csock, buf + sent, len - sent);
		if (n <= 0)
			return;
		sent += n;
	}
}

/**
 * read exactly n bytes from csock into buf
 * return n on success, -1 on error/disconnect
 */
static int read_exact(char *buf, int n)
{
	int total = 0;
	while (total < n)
	{
		int r = read(csock, buf + total, n - total);
		if (r <= 0)
			return -1;
		total += r;
	}

	return total;
}

/**
 * read one CRLF terminated line from csock
 * strips CRLF, NUL terminates buf
 * returns chars stored, 0 on disconnect, -1 on error
 */
static int read_line_server(char *buf, int maxlen)
{
	int i = 0;
	while (i < maxlen - 1)
	{
		char c;
		int r = read(csock, &c, 1);
		if (r <= 0)
		{
			buf[i] = '\0';

			return r;
		}
		if (c == '\r')
			continue;
		if (c == '\n')
		{
			buf[i] = '\0';

			return i;
		}
		buf[i++] = c;
	}
	buf[i] = '\0';

	return i;
}

/**
 * read one line from stdin, aborting after timeout_secs
 * trims trailing newline
 * returns 1 if user typed something, 0 if timer expired OR stdin EOF
 */
static int read_stdin_timed(char *buf, int maxlen, int timeout_secs)
{
	fd_set fds;
	struct timeval tv;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	tv.tv_sec = timeout_secs;
	tv.tv_usec = 0;
	int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);

	if (r <= 0)
		return 0;
	if (fgets(buf, maxlen, stdin) == NULL)
		return 0;

	// strip trailing crlf
	int len = (int)strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	return 1;
}

/*
**	Client
*/
int main(int argc, char *argv[])
{
	char buf[BUFSIZE];
	char *service;
	char *host = "localhost";
	int cc;

	switch (argc)
	{
	case 2:
		service = argv[1];
		break;
	case 3:
		host = argv[1];
		service = argv[2];
		break;
	default:
		fprintf(stderr, "usage: client [host] port\n");
		exit(1);
	}

	/*	Create the socket to the controller  */
	if (((csock = connectsock(host, service, "tcp")) == 0))
	{
		fprintf(stderr, "client: cannot connect to %s:%s\n", host, service);
		exit(1);
	}

	// handshake

	// QS|ADMIN
	if (read_line_server(buf, sizeof(buf)) <= 0)
	{
		fprintf(stderr, "client: server disconnected during handshake\n");
		close(csock);
		exit(1);
	}
	if (strncmp(buf, "QS|ADMIN", 8) != 0)
	{
		fprintf(stderr, "client: unexpected message: %s\n", buf);
		close(csock);
		exit(1);
	}

	printf("=== welcome to da quiz ===\n");
	printf("Enter your name: ");
	fflush(stdout);

	char name[256];
	if (fgets(name, sizeof(name), stdin) == NULL)
	{
		close(csock);
		exit(1);
	}
	// trim trailing newline
	int nlen = (int)strlen(name);
	while (nlen > 0 && (name[nlen - 1] == '\n' || name[nlen - 1] == '\r'))
		name[--nlen] = '\0';

	snprintf(buf, sizeof(buf), "GROUP|%s\r\n", name);
	send_all(buf, (int)strlen(buf));
	printf("get er done, %s!\n", name);
	fflush(stdout);

	/**
	 * main message loop
	 * server streams:
	 * QUES|size|text (no CRLF)
	 * WIN|name\r\n (or WIN\r\n)
	 * RESULT|name|score\r\n
	 *
	 * dispatch on command word, reading until '|' or '\r' to identify message
	 */
	int question_num = 0;

	for (;;)
	{
		char cmd[32];
		int ci = 0;
		char c = 0;

		while (ci < (int)sizeof(cmd) - 1)
		{
			if (read(csock, &c, 1) <= 0)
			{
				printf("\nclient; server closed connection\n");
				goto done;
			}
			if (c == '|' || c == '\r')
				break;
			cmd[ci++] = c;
		}
		cmd[ci] = '\0';
		// QUES|size|text
		if (strcmp(cmd, "QUES") == 0 && c == '|')
		{
			question_num++;
			// read size until next '|'
			char sizestr[32];
			int si = 0;
			while (si < (int)sizeof(sizestr) - 1)
			{
				if (read(csock, &c, 1) <= 0)
					goto done;
				if (c == '|')
					break;
				sizestr[si++] = c;
			}
			sizestr[si] = '\0';
			int textlen = atoi(sizestr);

			if (textlen <= 0 || textlen > MAX_QTEXT)
			{
				fprintf(stderr, "client: bad QUES size %d\n", textlen);
				goto done;
			}

			// read exactly textlen bytes of question text
			char qtext[MAX_QTEXT + 1];
			if (read_exact(qtext, textlen) != textlen)
				goto done;
			qtext[textlen] = '\0';
			// display question
			printf("\n=== Question %d ===\n%s\n", question_num, qtext);
			printf("Your answer [%d sec]: ", TIMEOUT_SECS);
			fflush(stdout);
			// read user answer with timeout
			char answer[64];
			if (read_stdin_timed(answer, sizeof(answer), TIMEOUT_SECS) && strlen(answer) > 0)
			{
				// user typed
			}
			else
			{
				// user timed out or empty
				strcpy(answer, "NOANS");
				printf("\nTime's up! Therefore, no answer.\n");
				fflush(stdout);
			}
			// ANS|answerID
			snprintf(buf, sizeof(buf), "ANS|%s\r\n", answer);
			send_all(buf, (int)strlen(buf));
		}
		else if (strcmp(cmd, "WIN") == 0 && c == '|')
		{
			if (read_line_server(buf, sizeof(buf)) < 0)
				goto done;
			if (strlen(buf) > 0)
				printf("=== Correct! Point to: %s ===\n", buf);
			else
				printf("=== Wrong. Sorry! ===\n");
			fflush(stdout);
		}
		else if (strcmp(cmd, "RESULT") == 0 && c == '|')
		{
			if (read_line_server(buf, sizeof(buf)) < 0)
				goto done;
			char *pipe = strchr(buf, '|');
			if (pipe)
			{
				*pipe = '\0';
				printf("\n=== Quiz Over! ===\n");
				printf("Player: %s\n", buf);
				printf("Score: %s\n", pipe + 1);
			}
			else
			{
				printf("\nQuiz Over! Result: %s ===\n", buf);
			}
			fflush(stdout);

			break; // server will close socket
		}
		else
		{
			fprintf(stderr, "client: unexpected command '%s', skipping\n", cmd);
			if (c != '\r')
			{
				while (read(csock, &c, 1) > 0 && c != '\n')
					;
			}
			else
			{
				read(csock, &c, 1);
			}
		}
	}
done:
	close(csock);

	return 0;
}
