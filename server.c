#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <string.h>
#include <netdb.h>

#define QLEN 5
#define BUFSIZE 4096
#define MAX_QUESTIONS 100
#define MAX_QTEXT 2048
#define TIMEOUT_SECS 10

int passivesock(char *service, char *protocol, int qlen, int *rport);

typedef struct
{
	char text[MAX_QTEXT];
	char correct[16];
} Question;

static Question questions[MAX_QUESTIONS];
static int num_questions = 0;

/**
 * Parse quiz file into questions[] array
 *
 * File format:
 * id question_text\n
 * id answer_text\n
 * ...
 * \n (blank line separates answers from correct-id)
 * correctID\n
 * \n (blank line ends question block)
 *
 * raw lines including '\n' are concatenated into question.text
 * so they can be sent as-is to client
 */
static int load_questions(const char *filename)
{
	FILE *f = fopen(filename, "r");
	if (!f)
	{
		perror(filename);

		return -1;
	}

	char line[512];
	/**
	 * 0 = waiting for question header line
	 * 1 = reading answer lines
	 * 2 = reading correct-answer ID
	 * 3 = waiting for trailing blank after correct-answer ID
	 */
	int state = 0;
	int qi = -1;

	while (fgets(line, sizeof(line), f))
	{
		int len = strlen(line);
		int blank = (len == 0 || line[0] == '\n' || line[0] == '\r');
		switch (state)
		{
		case 0:
			if (blank)
				continue;
			qi = num_questions;
			if (qi >= MAX_QUESTIONS)
				goto done;
			memset(&questions[qi], 0, sizeof(questions[qi]));
			strncat(questions[qi].text, line, MAX_QTEXT - 1 - (int)strlen(questions[qi].text));
			state = 1;
			break;
		case 1:
			if (blank)
			{
				state = 2;
			}
			else
			{
				strncat(questions[qi].text, line, MAX_QTEXT - 1 - (int)strlen(questions[qi].text));
			}
			break;
		case 2:
			if (blank)
				continue;
			sscanf(line, "%15s", questions[qi].correct);
			state = 3;
			break;
		case 3:
			if (blank)
			{
				num_questions++;
				state = 0;
			}
			break;
		}
	}
	if (state == 3)
		num_questions++;

done:
	fclose(f);

	return num_questions;
}

/**
 * write all 'len' bytes to sock (handles partial writes)
 */
static void send_all(int sock, const char *buf, int len) {
	int sent = 0;
	while (sent < len) {
		int n = write(sock, buf + sent, len - sent);
		if (n <= 0) return;
		sent += n;
	}
}

/**
 * read one CRLF terminated line from sock
 * strips CRLF and NULL terminates buf
 * returns # of chars stored, 0 on clean disconnect
 * -1 on error, -2 on timeout
 * timeout_secs == 0 means wait indefinitely
 */
static int read_crlf_line(int sock, char *buf, int maxlen, int timeout_secs) {
	int i = 0;

	while (i < maxlen - 1) {
		if (timeout_secs > 0) {
			fd_set fds;
			struct timeval tv;
			FD_ZERO(&fds);
			FD_SET(sock, &fds);
			tv.tv_sec = timeout_secs;
			tv.tv_usec = 0;
			int r = select(sock + 1, &fds, NULL, NULL, &tv);
			if (r == 0) {
				buf[i] = '\0';

				return -2;
			}
			if (r < 0) {
				buf[i] = '\0';

				return -1;
			}
		}
		char c;
		int n = read(sock, &c, 1);
		if (n <= 0) {
			buf[i] = '\0';
			
			return n;
		}
		if (c == '\r') continue;
		if (c == '\n') {
			buf[i] = '\0';
			
			return i;
		}
		buf[i++] = c;
	}
	buf[i] = '\0';

	return i;
}

/*
**	A simple echoserver
**	This poor server ... only serves one client at a time
*/
int main(int argc, char *argv[])
{
	char buf[BUFSIZE];
	char *service = NULL;
	char *quizfile;
	struct sockaddr_in fsin;
	int alen;
	int msock;
	int ssock;
	int cc;
	int rport = 0;

	struct sockaddr_in sin; /* an Internet endpoint address		*/

	switch (argc)
	{
	case 2:
		// User provides a port? then use it
		quizfile = argv[1];
		rport = 1;
		break;
	case 3:
		quizfile = argv[1];
		service = argv[2];
		break;
	default:
		fprintf(stderr, "usage: server quizfile [port]\n");
		exit(1);
	}

	if (load_questions(quizfile) <= 0) {
		fprintf(stderr, "server: no questions loaded from '%s'\n", quizfile);
		exit(1);
	}
	fprintf(stderr, "server: loaded %d question (s)\n", num_questions);

	// Call the function that sets everything up
	msock = passivesock(service, "tcp", QLEN, &rport);

	if (rport)
	{
		// Tell the user the selected port number
		printf("server: port %d\n", rport);
		fflush(stdout);
	}

	/* server main loop */
	for (;;)
	{
		struct sockaddr_in fsin;
		socklen_t alen = sizeof(fsin);

		/* accept a client */
		int ssock = accept(msock, (struct sockaddr *)&fsin, &alen);

		if (ssock < 0)
		{
			fprintf(stderr, "accept: %s\n", strerror(errno));
			continue;
		}

		char buf[BUFSIZE];
		char name[256] = "Unknown";
		int score = 0;
		int aborted = 0;

		// handshake
		send_all(ssock, "QS|ADMIN\r\n", 10);

		if (read_crlf_line(ssock, buf, sizeof(buf), 30) <= 0) {
			close(ssock);
			continue;
		}
		if (strncmp(buf, "GROUP|", 6) == 0) {
			strncpy(name, buf + 6, sizeof(name) - 1);
			name[sizeof(name) - 1] = '\0';
		}
		fprintf(stderr, "server: client '%s' connected\n", name);

		// quiz
		for (int i = 0; i < num_questions && !aborted; i++) {
			int textlen = (int)strlen(questions[i].text);
			int hlen = snprintf(buf, sizeof(buf), "QUES|%d|", textlen);
			send_all(ssock, buf, hlen);
			send_all(ssock, questions[i].text, textlen);
			char answer[64] = "NOANS";
			int r = read_crlf_line(ssock, buf, sizeof(buf), TIMEOUT_SECS);
			if (r == -2) {
				fprintf(stderr, "server: '%s' timed out on Q%d\n", name, i + 1);
			} else if (r <= 0) {
				fprintf(stderr, "server: '%s' disconnected during quiz\n", name);
				aborted = 1;
				break;
			} else if (strncmp(buf, "ANS|", 4) == 0) {
				strncpy(answer, buf + 4, sizeof(answer) - 1);
				answer[sizeof(answer) - 1] = '\0';
				// trim trailing whitespace
				int alen = strlen(answer);
				while (alen > 0 && (answer[alen-1] == ' ' || answer[alen-1] == '\t')) answer[--alen] = '\0';
			}

			// scoring
			int is_noans = (strcmp(answer, "NOANS") == 0);
			int is_correct = !is_noans && (strcmp(answer, questions[i].correct) == 0);
			int is_wrong = !is_noans && !is_correct;

			if (is_correct) score++;
			else if (is_wrong) score--;

			fprintf(stderr, "server: Q%d answer='%s' correct='%s' -> %s\n", i + 1, answer, questions[i].correct,
					is_correct ? "CORRECT" : (is_noans ? "NOANS" : "WRONG"));
			
			// send WIN|name or WIN|
			if (is_correct) snprintf(buf, sizeof(buf), "WIN|%s\r\n", name);
			else snprintf(buf, sizeof(buf), "WIN|\r\n");

			send_all(ssock, buf, strlen(buf));
		}

		// results
		if (!aborted) {
			snprintf(buf, sizeof(buf), "RESULT|%s|%d\r\n", name, score);
			send_all(ssock, buf, strlen(buf));
			fprintf(stderr, "server: quiz done. '%s' scored %d\n", name, score);
		}

		close(ssock);
		fprintf(stderr, "server: ready for next client\n");
	}

	return 0;
}
