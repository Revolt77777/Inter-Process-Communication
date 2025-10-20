CFLAGS := -Wall --std=gnu99 -g3 -Werror -fPIC
ASAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer
ASAN_LIBS = -static-libasan
CURL_LIBS := $(shell curl-config --libs)
CURL_CFLAGS := $(shell curl-config --cflags)

ARCH := $(shell uname)
ifneq ($(ARCH),Darwin)
  LDFLAGS += -lpthread -lrt
endif

PROXY_OBJ := webproxy.o steque.o
PROXY_OBJ_NOASAN := webproxy_noasan.o steque_noasan.o handle_with_curl_noasan.o gfserver_noasan.o

all: clean all_asan all_noasan

all_asan: webproxy

all_noasan: webproxy_noasan

noasan: all_noasan

webproxy: $(PROXY_OBJ) handle_with_curl.o gfserver.o 
	$(CC) -o $@ $(CFLAGS) $(ASAN_FLAGS) $(CURL_CFLAGS) $^ $(LDFLAGS) $(CURL_LIBS) $(ASAN_LIBS)

webproxy_noasan: $(PROXY_OBJ_NOASAN) 
	$(CC) -o $@ $(CFLAGS) $(CURL_CFLAGS) $^ $(LDFLAGS) $(CURL_LIBS)

%_noasan.o : %.c
	$(CC) -c -o $@ $(CFLAGS) $<

%.o : %.c
	$(CC) -c -o $@ $(CFLAGS) $(ASAN_FLAGS) $<

.PHONY: clean

clean:
	rm -f webproxy.o steque.o handle_with_curl.o webproxy_noasan.o steque_noasan.o handle_with_curl_noasan.o
	rm -f webproxy webproxy_noasan
