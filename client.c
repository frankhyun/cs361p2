/*
 * client2.c - pa2 part 2
 *
 * usage:
 *   client2 [host] port <- connects as group leader
 *   client2 [host] port member <- connects as group member
 *
 * leader flow: QS|ADMIN -> GROUP|name|size ->  WAIT -> quiz
 * member flow: QS|JOIN -> JOIN|name -> WAIT ->  quiz
 * rejected: QS|FULL -> (server closes)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define BUFSIZE 4096
#define MAX_QTEXT 2048
#define TIMEOUT_SECS 8

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

int connectsock(char *host, char *service, char *protocol);

static int csock;

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
			return r == 0 ? 0 : -1;
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
	int len = (int)strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';
	return 1;
}

/* ------------------------------------------------------------------ */
/*  quiz loop, shared by leader and member after handshake            */
/* ------------------------------------------------------------------ */

static void run_quiz_client(void)
{
	char buf[BUFSIZE];
	int question_num = 0;
	int announced = 0; // print "All players joined!" once on first QUES

	for (;;)
	{
		// read command word up to '|' or '\r'
		char cmd[32];
		int ci = 0;
		char c = 0;
		while (ci < (int)sizeof(cmd) - 1)
		{
			if (read(csock, &c, 1) <= 0)
			{
				printf("\nclient: server closed connection\n");
				return;
			}
			if (c == '|' || c == '\r')
				break;
			cmd[ci++] = c;
		}
		cmd[ci] = '\0';

		// QUES|size|text
		if (strcmp(cmd, "QUES") == 0 && c == '|')
		{
			if (!announced)
			{
				printf("All players joined! Quiz starting...\n");
				fflush(stdout);
				announced = 1;
			}
			question_num++;

			char sizestr[32];
			int si = 0;
			while (si < (int)sizeof(sizestr) - 1)
			{
				if (read(csock, &c, 1) <= 0)
					return;
				if (c == '|')
					break;
				sizestr[si++] = c;
			}
			sizestr[si] = '\0';
			int textlen = atoi(sizestr);
			if (textlen <= 0 || textlen > MAX_QTEXT)
			{
				fprintf(stderr, "client: bad QUES size %d\n", textlen);
				return;
			}

			char qtext[MAX_QTEXT + 1];
			if (read_exact(qtext, textlen) != textlen)
				return;
			qtext[textlen] = '\0';

			printf("\n=== Question %d ===\n%s\n", question_num, qtext);
			printf("Your answer [%d sec]: ", TIMEOUT_SECS);
			fflush(stdout);

			char answer[64];
			if (read_stdin_timed(answer, sizeof(answer), TIMEOUT_SECS) &&
				strlen(answer) > 0)
			{
				// user typed
			}
			else
			{
				strcpy(answer, "NOANS");
				printf("\nTime's up!\n");
				fflush(stdout);
			}

			snprintf(buf, sizeof(buf), "ANS|%s\r\n", answer);
			send_all(buf, (int)strlen(buf));

			// WIN|name
		}
		else if (strcmp(cmd, "WIN") == 0 && c == '|')
		{
			if (read_line_server(buf, sizeof(buf)) < 0)
				return;
			if (strlen(buf) > 0)
				printf("=== Correct! First right answer: %s ===\n", buf);
			else
				printf("=== No one got it right. ===\n");
			fflush(stdout);

			// RESULT|name|score|name|score...
		}
		else if (strcmp(cmd, "RESULT") == 0 && c == '|')
		{
			if (read_line_server(buf, sizeof(buf)) < 0)
				return;

			printf("\n=== Quiz Over! Final Standings ===\n");
			int rank = 1;
			char *p = buf;
			while (*p)
			{
				char *pipe1 = strchr(p, '|');
				if (!pipe1)
				{
					printf("%d. %s\n", rank, p);
					break;
				}
				*pipe1 = '\0';
				char *name = p;
				char *rest = pipe1 + 1;
				char *pipe2 = strchr(rest, '|');
				char *score;
				if (pipe2)
				{
					*pipe2 = '\0';
					score = rest;
					p = pipe2 + 1;
				}
				else
				{
					score = rest;
					p = rest + strlen(rest);
				}
				printf("%d. %-20s %s pts\n", rank++, name, score);
			}
			fflush(stdout);
			break;
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
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
	char buf[BUFSIZE];
	char *host = "localhost";
	char *service;
	int is_member = 0;

	/*
	 *  accept:
	 *      client2 port
	 *      client2 host port
	 *      client2 port member
	 *      client2 host port member
	 */
	if (argc == 2)
	{
		service = argv[1];
	}
	else if (argc == 3)
	{
		if (strcmp(argv[2], "member") == 0)
		{
			service = argv[1];
			is_member = 1;
		}
		else
		{
			host = argv[1];
			service = argv[2];
		}
	}
	else if (argc == 4)
	{
		host = argv[1];
		service = argv[2];
		is_member = (strcmp(argv[3], "member") == 0);
	}
	else
	{
		fprintf(stderr, "usage: client2 [host] port [member]\n");
		exit(1);
	}

	if ((csock = connectsock(host, service, "tcp")) == 0)
	{
		fprintf(stderr, "client2: cannot connect to %s:%s\n", host, service);
		exit(1);
	}

	/* Read server greeting */
	if (read_line_server(buf, sizeof(buf)) <= 0)
	{
		fprintf(stderr, "client2: server disconnected during handshake\n");
		close(csock);
		exit(1);
	}

	/* ---- QS|FULL ---- */
	if (strcmp(buf, "QS|FULL") == 0)
	{
		printf("Server is busy (group full or quiz in progress). Try again later.\n");
		close(csock);
		exit(0);
	}

	/* ---- QS|ADMIN (leader) ---- */
	if (strcmp(buf, "QS|ADMIN") == 0)
	{
		printf("=== Welcome to da quiz (Leader) ===\n");
		printf("Your name: ");
		fflush(stdout);

		char name[256];
		if (fgets(name, sizeof(name), stdin) == NULL)
		{
			close(csock);
			exit(1);
		}
		int nlen = strlen(name);
		while (nlen > 0 && (name[nlen - 1] == '\n' || name[nlen - 1] == '\r'))
			name[--nlen] = '\0';

		int group_size = 1;
		printf("Group size (how many players total, including you): ");
		fflush(stdout);
		char gs_buf[32];
		if (fgets(gs_buf, sizeof(gs_buf), stdin) != NULL)
			group_size = atoi(gs_buf);
		if (group_size < 1)
			group_size = 1;

		snprintf(buf, sizeof(buf), "GROUP|%s|%d\r\n", name, group_size);
		send_all(buf, strlen(buf));

		printf("Waiting for %d player(s) to join...\n", group_size - 1);
		fflush(stdout);

		/* expect WAIT */
		if (read_line_server(buf, sizeof(buf)) < 0)
		{
			close(csock);
			exit(1);
		}
		if (strcmp(buf, "WAIT") != 0)
		{
			fprintf(stderr, "client2: expected WAIT, got '%s'\n", buf);
			close(csock);
			exit(1);
		}

		printf("Waiting for all players to join...\n");
		fflush(stdout);

		run_quiz_client();

		/* ---- QS|JOIN (member) ---- */
	}
	else if (strcmp(buf, "QS|JOIN") == 0)
	{
		printf("=== Welcome to da quiz (Member) ===\n");
		printf("Your name: ");
		fflush(stdout);

		char name[256];
		if (fgets(name, sizeof(name), stdin) == NULL)
		{
			close(csock);
			exit(1);
		}
		int nlen = strlen(name);
		while (nlen > 0 && (name[nlen - 1] == '\n' || name[nlen - 1] == '\r'))
			name[--nlen] = '\0';

		snprintf(buf, sizeof(buf), "JOIN|%s\r\n", name);
		send_all(buf, strlen(buf));

		printf("Waiting for quiz to start...\n");
		fflush(stdout);

		/* expect WAIT */
		if (read_line_server(buf, sizeof(buf)) < 0)
		{
			close(csock);
			exit(1);
		}
		if (strcmp(buf, "WAIT") != 0)
		{
			fprintf(stderr, "client2: expected WAIT, got '%s'\n", buf);
			close(csock);
			exit(1);
		}

		printf("Quiz starting!\n");
		fflush(stdout);

		run_quiz_client();
	}
	else
	{
		fprintf(stderr, "client2: unexpected greeting '%s'\n", buf);
		close(csock);
		exit(1);
	}

	(void)is_member; /* role is driven by server greeting, not CLI flag */
	close(csock);
	return 0;
}