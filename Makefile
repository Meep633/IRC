chatroom_server.out: chatroom_server.c
	clang -Wall -Wextra -O2 -pthread chatroom_server.c -o chatroom_server.out -g

clean:
	rm -f chatroom_server.out
