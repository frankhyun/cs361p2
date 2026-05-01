/* connectsock.c - connectsock */
/* Based on Stevens */

/*
 * ROLE IN THE SYSTEM
 * ------------------
 * connectsock is the CLIENT-SIDE bootstrap helper. It hides the ugly
 * socket(2)/getaddrinfo/connect(2) dance behind one call.
 *
 *   client.c   --[calls]--> connectsock()  --[returns fd]--> client.c
 *                                                             speaks
 *                                                             protocol
 *
 * Mirror of passivesock.c, which does the analogous job on the server
 * side (bind + listen instead of connect). Together they form the
 * tiny socket library (libsocklib.a) that client.c and server.c link
 * against so neither has to repeat the BSD socket boilerplate.
 *
 * After connectsock() returns a connected file descriptor, the client
 * communicates with the server strictly at the application level,
 * read()/write() on that fd. The far end of the connection ends up
 * inside server.c's accept() loop as a per-client handler thread.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>

#include <netinet/in.h>

#include <netdb.h>

#ifndef	INADDR_NONE
#define	INADDR_NONE	0xffffffff
#endif	/* INADDR_NONE */


/*------------------------------------------------------------------------
 * connectsock - allocate & connect a socket using TCP or UDP
 *
 * Steps (all library-level, no app-layer protocol yet):
 *   1. Resolve service name -> port number (or parse numeric port).
 *   2. Resolve host name -> IP address (or parse dotted-decimal).
 *   3. Resolve protocol name ("tcp"/"udp") -> kernel protocol number.
 *   4. socket(2) to allocate the fd.
 *   5. connect(2) to complete the TCP 3-way handshake with the server.
 *
 * Returns: a connected fd on success, exit()s on any failure.
 *------------------------------------------------------------------------
 */
int
connectsock( 
char	*host,		/* name of host to which connection is desired	*/
char	*service,	/* service associated with the desired port	*/
char	*protocol ) 	/* name of protocol to use ("tcp" or "udp")	*/
{
	struct hostent	*phe;	/* pointer to host information entry	*/
	struct servent	*pse;	/* pointer to service information entry	*/
	struct protoent *ppe;	/* pointer to protocol information entry*/
	struct sockaddr_in sin;	/* an Internet endpoint address		*/
	int	s, type;	/* socket descriptor and socket type	*/


	memset((char *)&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;

    /* Map service name to port number */
	if ( pse = getservbyname(service, protocol) )
		sin.sin_port = pse->s_port;
	else if ( (sin.sin_port = htons((u_short)atoi(service))) == 0 )
	{
		fprintf( stderr, "can't get \"%s\" service entry\n", service);
		exit(-1);
	}


    /* Map host name to IP address, allowing for dotted decimal */
	if ( phe = gethostbyname(host) )
		memcpy(phe->h_addr, (char *)&sin.sin_addr, phe->h_length);
	else if ( (sin.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE )
	{
		fprintf( stderr, "can't get \"%s\" host entry\n", host);
		exit(-1);
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

    /* Connect the socket */
	if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) < 0)
	{
		fprintf( stderr, "can't connect to %s.%s: %s\n", host, service, strerror(errno));
		exit(-1);
	}

	return s;
}
