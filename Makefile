INCLUDES        = -I. -I/usr/include

LIBS		= libsocklib.a -ldl -lpthread -lm

COMPILE_FLAGS   = ${INCLUDES} -c
COMPILE         = gcc ${COMPILE_FLAGS}
LINK            = gcc -o

EXEC		= client server

.SUFFIXES       :       .o .c .h

all		:	library client server

%.o            :	%.c
			@echo "Compiling $< ..."
			@${COMPILE} $<

library		:	passivesock.o connectsock.o
			ar rv libsocklib.a passivesock.o connectsock.o

server		:	server.o
			${LINK} $@ server.o ${LIBS}

client		:	client.o
			${LINK} $@ client.o ${LIBS}

clean           :
			@echo "    Cleaning ..."
			rm -f tags core *.out *.o *.lis *.a ${EXEC} libsocklib.a
