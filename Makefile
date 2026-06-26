CC = gcc
CFLAGS = -Wall -g -finput-charset=UTF-8 -fexec-charset=UTF-8
# [필수] OpenSSL 및 Pthread 라이브러리 링크
LIBS = -lpthread -lssl -lcrypto

TARGET = ott_server
SRCS = src/main.c src/handler.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)