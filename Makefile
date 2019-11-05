CC = /usr/bin/gcc
CCFLAGS = -c -g 
LD = /usr/bin/gcc
LDFLAGS = -L/usr/lib -L/usr/local/lib
INCLUDE = -I/usr/include -I/usr/local/include

SEND = reliable_sender
RECV = reliable_receiver
UDP = udp

EXE = $(SEND) $(RECV)

OBJ_SEND = $(SEND).o $(UDP).o
OBJ_RECV = $(RECV).o $(UDP).o

OBJ = $(OBJ_SEND) $(OBJ_RECV)

.PHONY : all clean

all : $(EXE)

clean :
	-rm -fv $(EXE) $(OBJ)

$(SEND) : $(OBJ_SEND)
	$(LD) $(INCLUDE) $(LDFLAGS) $(OBJ_SEND) -o $(SEND)

$(SEND).o : $(SEND).c $(UDP).h
	$(CC) $(INCLUDE) $(CCFLAGS) $(SEND).c

$(RECV) : $(OBJ_RECV)
	$(LD) $(INCLUDE) $(LDFLAGS) $(OBJ_RECV) -o $(RECV)

$(RECV).o : $(RECV).c $(UDP).h
	$(CC) $(INCLUDE) $(CCFLAGS) $(RECV).c

$(UDP).o : $(UDP).c $(UDP).h
	$(CC) $(INCLUDE) $(CCFLAGS) $(UDP).c
