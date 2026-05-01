/* passivesock.c - passivesock */
/*
 * This code was adapted from the Stevens and
 * the Comer books on Network Programming
 */

/*
 * ROLE IN THE SYSTEM
 * ------------------
 * passivesock is the SERVER-SIDE bootstrap helper, the mirror of
 * connectsock.c. A "passive" socket is one that listens for incoming
 * connections rather than initiating one.
 *
 *   server.c --[calls]--> passivesock()  --[returns listening fd]--> server.c
 *                                                                    accept()
 *                                                                    loop
 *
 * Relationship to the other three files:
 *
 *   client.c  ---connectsock()---> TCP ---passivesock() fd--->  server.c
 *                                                                  |
 *                                                              accept()
 *                                                                  |
 *                                                              new fd per
 *                                                              connection
 *                                                              -> thread
 *
 * server.c calls passivesock() exactly once at startup to get the
 * "master socket" msock. It then loops on accept(msock, ...) which
 * returns a fresh per-client socket for each inbound connection,
 * one of which was just opened by a call to connectsock() inside
 * some client.c process.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <netinet/in.h>

#include <netdb.h>


static u_short	portbase = 37000;    /* port base, for non-root servers	*/

/*------------------------------------------------------------------------
 * passivesock - allocate & bind a server socket using TCP or UDP
 *
 * Steps (mirror of connectsock, but ending in bind+listen, not connect):
 *   1. Resolve service name -> port number (or use *rport=1 to let
 *      the kernel pick a free port, which is then written back via
 *      *rport so the caller can print it for the user).
 *   2. Resolve protocol name -> kernel protocol number.
 *   3. socket(2) to allocate the fd.
 *   4. bind(2) to the chosen port on INADDR_ANY (all interfaces).
 *   5. listen(2) with backlog qlen, if TCP.
 *
 * Returns: a listening fd on success (server.c will accept() on it),
 *          exit()s on failure.
 *------------------------------------------------------------------------
 */

int
passivesock( 
	char	*service,   /* service associated with the desired port	*/
	char	*protocol,  /* name of protocol to use ("tcp" or "udp")	*/
	int	qlen,	    /* max length of the server request queue	*/
	int	*rport )
{
	struct servent	*pse;	/* pointer to service information entry	*/
	struct protoent *ppe;	/* pointer to protocol information entry*/
	struct sockaddr_in sin;	/* an Internet endpoint address		*/
	int	s, type;	/* socket descriptor and socket type	*/

	memset((char *)&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = INADDR_ANY;

    /* Map service name to port number */
	if ( *rport )
	{
		/*	If a 1 is in this field, choose a free port */
		sin.sin_port = htons((u_short) 0);
	}
	else
	{
		if ( pse = getservbyname(service, protocol) )
			sin.sin_port = htons(ntohs((u_short)pse->s_port)
				+ portbase);
		else if ( (sin.sin_port = htons((u_short)atoi(service))) == 0 )
		{
			fprintf( stderr, "can't get \"%s\" service entry\n", service);
			exit(-1);
		}
	}

    /* Map protocol name to protocol number */
	if ( (ppe = getprotobyname(protocol)) == 0)
	{
		fprintf( stderr, "can't get \"%s\" protocol entry\n", protocol);
		exit(-1);
	}

    /* Use protocol to choose a socket type */
	if (strcmp(protocol, "udp") == 0)
		type = SOCK_DGRAM;
	else
		type = SOCK_STREAM;

    /* Allocate a socket */
	s = socket(PF_INET, type, ppe->p_proto);
	if (s < 0)
	{
		fprintf( stderr, "can't create socket: %s\n", strerror(errno));
		exit(-1);
	}

    /* Bind the socket */
	if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0)
	{
		fprintf( stderr, "can't bind to %s port: %s\n", service, strerror(errno));
		exit(-1);
	}
	if (type == SOCK_STREAM && listen(s, qlen) < 0)
	{
		fprintf( stderr, "can't listen on %s port: %s\n", service, strerror(errno));
		exit(-1);
	}

	if ( *rport )
	{
		int	len;

		/* return the selected port in rport */
		len = sizeof(sin);
		if ( getsockname( s, (struct sockaddr *)&sin, &len ) )
		{
			fprintf(  stderr, "chatd: cannot getsockname: %s\n", strerror(errno) );
			exit(-1);
		}
		*rport = ntohs(sin.sin_port);	

	}
	return s;
}
