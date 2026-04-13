#include "skynet.h"

#include "socket_server.h"
#include "atomic.h"
#include "spinlock.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <winerror.h>
#include <conio.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO 128
#define MAX_SOCKET_P 16
#define MIN_READ_BUFFER 64
#define SOCKET_TYPE_INVALID 0
#define SOCKET_TYPE_RESERVE 1
#define SOCKET_TYPE_PLISTEN 2
#define SOCKET_TYPE_LISTEN 3
#define SOCKET_TYPE_CONNECTING 4
#define SOCKET_TYPE_CONNECTED 5
#define SOCKET_TYPE_HALFCLOSE_READ 6
#define SOCKET_TYPE_HALFCLOSE_WRITE 7
#define SOCKET_TYPE_PACCEPT 8
#define SOCKET_TYPE_BIND 9

#define MAX_SOCKET (1<<MAX_SOCKET_P)

#define PRIORITY_HIGH 0
#define PRIORITY_LOW 1

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)
#define ID_TAG16(id) ((id>>MAX_SOCKET_P) & 0xffff)

#define PROTOCOL_TCP 0
#define PROTOCOL_UDP 1
#define PROTOCOL_UDPv6 2
#define PROTOCOL_UNKNOWN 255

#define UDP_ADDRESS_SIZE 19
#define MAX_UDP_PACKAGE 65535
#define WARNING_SIZE (1024*1024)
#define USEROBJECT ((size_t)(-1))

#define IOCP_KEY_CTRL 1

enum iocp_op_type {
	IOCP_OP_ACCEPT = 1,
	IOCP_OP_CONNECT = 2,
	IOCP_OP_READ = 3,
	IOCP_OP_WRITE = 4,
	IOCP_OP_UDP_RECV = 5
};

union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

struct write_buffer {
	struct write_buffer * next;
	const void *buffer;
	char *ptr;
	size_t sz;
	bool userobject;
};

struct write_buffer_udp {
	struct write_buffer buffer;
	uint8_t udp_address[UDP_ADDRESS_SIZE];
};

#define SIZEOF_TCPBUFFER (sizeof(struct write_buffer))
#define SIZEOF_UDPBUFFER (sizeof(struct write_buffer_udp))

struct wb_list {
	struct write_buffer * head;
	struct write_buffer * tail;
};

struct socket_stat {
	uint64_t rtime;
	uint64_t wtime;
	uint64_t read;
	uint64_t write;
};

struct socket {
	uintptr_t opaque;
	struct wb_list high;
	struct wb_list low;
	int64_t wb_size;
	struct socket_stat stat;
	ATOM_ULONG sending;
	SOCKET fd;
	int id;
	ATOM_INT type;
	uint8_t protocol;
	bool reading;
	bool writing;
	bool closing;
	ATOM_INT udpconnecting;
	int64_t warn_size;
	union {
		int size;
		uint8_t udp_address[UDP_ADDRESS_SIZE];
	} p;
	struct spinlock dw_lock;
	int dw_offset;
	const void * dw_buffer;
	size_t dw_size;
	bool read_posted;
	bool write_posted;
};

struct iocp_op {
	OVERLAPPED ol;
	enum iocp_op_type op;
	struct socket *s;
	int sid;
	WSABUF wsa;
	char *buffer;
	int bufsize;
	struct write_buffer *wb;
	size_t write_len;
	SOCKET accept_socket;
	struct sockaddr_storage addr;
	int addrlen;
};

struct ctrl_request {
	struct ctrl_request *next;
	char type;
	uint8_t len;
	uint8_t data[256];
};

struct msg_node {
	struct msg_node *next;
	struct socket_message msg;
};

struct socket_server {
	volatile uint64_t time;
	SOCKET reserve_fd;
	HANDLE iocp;
	ATOM_INT ctrl_count;
	CRITICAL_SECTION ctrl_lock;
	struct ctrl_request *ctrl_head;
	struct ctrl_request *ctrl_tail;
	struct msg_node *msg_head;
	struct msg_node *msg_tail;
	ATOM_INT alloc_id;
	struct socket_object_interface soi;
	struct socket slot[MAX_SOCKET];
	struct socket *stdin_socket;
	int stdin_id;
	char stdin_buf[1024];
	int stdin_len;
	char buffer[MAX_INFO];
	uint8_t udpbuffer[MAX_UDP_PACKAGE];
	LPFN_ACCEPTEX acceptex;
	LPFN_CONNECTEX connectex;
};

struct request_open {
	int id;
	int port;
	uintptr_t opaque;
	char host[1];
};

struct request_send {
	int id;
	size_t sz;
	const void * buffer;
};

struct request_send_udp {
	struct request_send send;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_setudp {
	int id;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_close {
	int id;
	int shutdown;
	uintptr_t opaque;
};

struct request_listen {
	int id;
	int fd;
	uintptr_t opaque;
	char host[1];
};

struct request_bind {
	int id;
	int fd;
	uintptr_t opaque;
};

struct request_resumepause {
	int id;
	uintptr_t opaque;
};

struct request_setopt {
	int id;
	int what;
	int value;
};

struct request_udp {
	int id;
	int fd;
	int family;
	uintptr_t opaque;
};

struct request_dial_udp {
	int id;
	int fd;
	uintptr_t opaque;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_package {
	uint8_t header[8];
	union {
		char buffer[256];
		struct request_open open;
		struct request_send send;
		struct request_send_udp send_udp;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_resumepause resumepause;
		struct request_setopt setopt;
		struct request_udp udp;
		struct request_setudp setudp;
		struct request_dial_udp dial_udp;
	} u;
	uint8_t dummy[256];
};

struct send_object {
	const void *buffer;
	size_t sz;
	void (*free_func)(void *);
};

#define MALLOC skynet_malloc
#define FREE skynet_free

struct socket_lock {
	struct spinlock *lock;
	int count;
};

static inline void
socket_lock_init(struct socket *s, struct socket_lock *sl) {
	sl->lock = &s->dw_lock;
	sl->count = 0;
}

static inline void
socket_lock(struct socket_lock *sl) {
	if (sl->count == 0) {
		spinlock_lock(sl->lock);
	}
	++sl->count;
}

static inline void
socket_unlock(struct socket_lock *sl) {
	--sl->count;
	if (sl->count == 0) {
		spinlock_unlock(sl->lock);
	}
}

static inline bool
socket_trylock(struct socket_lock *sl) {
	if (sl->count == 0) {
		if (!spinlock_trylock(sl->lock)) {
			return false;
		}
	}
	++sl->count;
	return true;
}

static inline bool
socket_invalid(struct socket *s, int id) {
	return ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID || s->id != id;
}

static void force_close(struct socket_server *ss, struct socket *s, struct socket_message *result);
static inline void stat_read(struct socket_server *ss, struct socket *s, int n);
static inline void stat_write(struct socket_server *ss, struct socket *s, int n);

static inline int
send_buffer_empty(struct socket *s) {
	return s->high.head == NULL && s->low.head == NULL;
}

static inline int
nomore_sending_data(struct socket *s) {
	return (send_buffer_empty(s) && s->dw_buffer == NULL && (ATOM_LOAD(&s->sending) & 0xffff) == 0)
		|| (ATOM_LOAD(&s->type) == SOCKET_TYPE_HALFCLOSE_WRITE);
}

static inline void
clear_wb_list(struct wb_list *list) {
	list->head = NULL;
	list->tail = NULL;
}

static const void *
object_buffer(struct socket_server *ss, const void *object, size_t *sz) {
	if (ss->soi.buffer && ss->soi.size) {
		*sz = ss->soi.size(object);
		return ss->soi.buffer(object);
	}
	return NULL;
}

static bool
send_object_init(struct socket_server *ss, struct send_object *so, const void *object, size_t sz) {
	if (sz == USEROBJECT) {
		size_t n = 0;
		const void * buffer = object_buffer(ss, object, &n);
		if (buffer == NULL) {
			return false;
		}
		so->buffer = buffer;
		so->sz = n;
		so->free_func = ss->soi.free;
		return true;
	}
	so->buffer = object;
	so->sz = sz;
	so->free_func = FREE;
	return false;
}

static bool
send_object_init_from_sendbuffer(struct socket_server *ss, struct send_object *so, struct socket_sendbuffer *buf) {
	if (buf->type == SOCKET_BUFFER_OBJECT) {
		return send_object_init(ss, so, buf->buffer, USEROBJECT);
	}
	so->buffer = buf->buffer;
	so->sz = buf->sz;
	so->free_func = (buf->type == SOCKET_BUFFER_MEMORY) ? FREE : NULL;
	return false;
}

static void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {
	if (wb->userobject) {
		ss->soi.free((void *)wb->buffer);
	} else {
		FREE((void *)wb->buffer);
	}
	FREE(wb);
}

static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {
	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		write_buffer_free(ss, tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}

static void
free_buffer(struct socket_server *ss, struct socket_sendbuffer *buf) {
	void *buffer = (void *)buf->buffer;
	switch (buf->type) {
	case SOCKET_BUFFER_MEMORY:
		FREE(buffer);
		break;
	case SOCKET_BUFFER_OBJECT:
		ss->soi.free(buffer);
		break;
	case SOCKET_BUFFER_RAWPOINTER:
		break;
	}
}

static const void *
clone_buffer(struct socket_sendbuffer *buf, size_t *sz) {
	switch (buf->type) {
	case SOCKET_BUFFER_MEMORY:
		*sz = buf->sz;
		return buf->buffer;
	case SOCKET_BUFFER_OBJECT:
		*sz = USEROBJECT;
		return buf->buffer;
	case SOCKET_BUFFER_RAWPOINTER: {
		*sz = buf->sz;
		void * tmp = MALLOC(*sz);
		memcpy(tmp, buf->buffer, *sz);
		return tmp;
	}
	}
	*sz = 0;
	return NULL;
}

static void
push_msg(struct socket_server *ss, struct socket_message *msg) {
	struct msg_node *node = MALLOC(sizeof(*node));
	node->msg = *msg;
	node->next = NULL;
	if (ss->msg_tail) {
		ss->msg_tail->next = node;
		ss->msg_tail = node;
	} else {
		ss->msg_head = ss->msg_tail = node;
	}
}

static bool
pop_msg(struct socket_server *ss, struct socket_message *msg) {
	struct msg_node *node = ss->msg_head;
	if (!node) return false;
	*msg = node->msg;
	ss->msg_head = node->next;
	if (!ss->msg_head) ss->msg_tail = NULL;
	FREE(node);
	return true;
}

static void
push_ctrl(struct socket_server *ss, char type, const void *data, uint8_t len) {
	struct ctrl_request *req = MALLOC(sizeof(*req));
	req->type = type;
	req->len = len;
	memcpy(req->data, data, len);
	req->next = NULL;
	EnterCriticalSection(&ss->ctrl_lock);
	if (ss->ctrl_tail) {
		ss->ctrl_tail->next = req;
		ss->ctrl_tail = req;
	} else {
		ss->ctrl_head = ss->ctrl_tail = req;
	}
	int need_wake = (ATOM_FINC(&ss->ctrl_count) == 0);
	LeaveCriticalSection(&ss->ctrl_lock);
	if (need_wake) {
		PostQueuedCompletionStatus(ss->iocp, 0, IOCP_KEY_CTRL, NULL);
	}
}

static struct ctrl_request *
pop_ctrl(struct socket_server *ss) {
	struct ctrl_request *req = NULL;
	EnterCriticalSection(&ss->ctrl_lock);
	req = ss->ctrl_head;
	if (req) {
		ss->ctrl_head = req->next;
		if (!ss->ctrl_head) ss->ctrl_tail = NULL;
		ATOM_FDEC(&ss->ctrl_count);
	}
	LeaveCriticalSection(&ss->ctrl_lock);
	return req;
}

static void
free_ctrl(struct ctrl_request *req) {
	FREE(req);
}

static void
socket_keepalive(SOCKET fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&keepalive, sizeof(keepalive));
}

static void
socket_nodelay(SOCKET fd) {
	int nodelay = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
}

static void
socket_nonblocking(SOCKET fd) {
	u_long mode = 1;
	ioctlsocket(fd, FIONBIO, &mode);
}

static int
last_wsa_error() {
	return (int)WSAGetLastError();
}

static int
poll_stdin(struct socket_server *ss, struct socket_message *result) {
	struct socket *s = ss->stdin_socket;
	if (!s || !s->reading) {
		return -1;
	}
	int ready = 0;
	while (_kbhit()) {
		int c = _getche();
		if (c == 0 || c == 224) {
			// skip function keys
			_getch();
			continue;
		}
		if (c == '\b') {
			if (ss->stdin_len > 0) {
				ss->stdin_len--;
			}
			continue;
		}
		if (c == '\r') {
			ready = 1;
			break;
		}
		if (c == '\n') {
			ready = 1;
			break;
		}
		if (ss->stdin_len < (int)(sizeof(ss->stdin_buf) - 2)) {
			ss->stdin_buf[ss->stdin_len++] = (char)c;
		} else {
			ready = 1;
			break;
		}
	}
	if (!ready) {
		return -1;
	}
	ss->stdin_buf[ss->stdin_len++] = '\n';
	char *buf = MALLOC(ss->stdin_len);
	memcpy(buf, ss->stdin_buf, ss->stdin_len);
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = ss->stdin_len;
	result->data = buf;
	stat_read(ss, s, ss->stdin_len);
	ss->stdin_len = 0;
	return SOCKET_DATA;
}

static const char *
win32_strerror(int err, char *buf, size_t buflen) {
	DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	wchar_t wbuf[256];
	DWORD ret = FormatMessageW(flags, NULL, (DWORD)err, 0, wbuf, (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])), NULL);
	if (ret == 0) {
		snprintf(buf, buflen, "Win32 error %d", err);
		return buf;
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, (int)buflen, NULL, NULL);
	if (n == 0) {
		snprintf(buf, buflen, "Win32 error %d", err);
		return buf;
	}
	size_t len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
		buf[len - 1] = '\0';
		--len;
	}
	return buf;
}

static const char *
wsa_strerror(int err, char *buf, size_t buflen) {
	return win32_strerror(err, buf, buflen);
}

static int
reserve_id(struct socket_server *ss) {
	for (int i=0;i<MAX_SOCKET;i++) {
		int id = ATOM_FADD(&ss->alloc_id, 1);
		struct socket *s = &ss->slot[HASH_ID(id)];
		int type = ATOM_LOAD(&s->type);
		if (type == SOCKET_TYPE_INVALID) {
			if (ATOM_CAS(&s->type, type, SOCKET_TYPE_RESERVE)) {
				s->id = id;
				s->protocol = PROTOCOL_UNKNOWN;
				ATOM_INIT(&s->udpconnecting, 0);
				s->fd = INVALID_SOCKET;
				return id;
			} else {
				--i;
			}
		}
	}
	return -1;
}

static inline void
stat_read(struct socket_server *ss, struct socket *s, int n) {
	s->stat.read += n;
	s->stat.rtime = ss->time;
}

static inline void
stat_write(struct socket_server *ss, struct socket *s, int n) {
	s->stat.write += n;
	s->stat.wtime = ss->time;
}

static struct socket *
new_fd(struct socket_server *ss, int id, SOCKET fd, int protocol, uintptr_t opaque) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	assert(ATOM_LOAD(&s->type) == SOCKET_TYPE_RESERVE);

	if (CreateIoCompletionPort((HANDLE)fd, ss->iocp, (ULONG_PTR)s, 0) == NULL) {
		ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
		return NULL;
	}

	s->id = id;
	s->fd = fd;
	s->reading = false;
	s->writing = false;
	s->closing = false;
	ATOM_INIT(&s->sending , ID_TAG16(id) << 16 | 0);
	s->protocol = protocol;
	s->p.size = MIN_READ_BUFFER;
	s->opaque = opaque;
	s->wb_size = 0;
	s->warn_size = 0;
	clear_wb_list(&s->high);
	clear_wb_list(&s->low);
	s->dw_buffer = NULL;
	s->dw_size = 0;
	s->read_posted = false;
	s->write_posted = false;
	memset(&s->stat, 0, sizeof(s->stat));
	return s;
}

static struct iocp_op *
alloc_iocp_op(enum iocp_op_type op, struct socket *s, int bufsize) {
	struct iocp_op *ctx = MALLOC(sizeof(*ctx));
	memset(ctx, 0, sizeof(*ctx));
	ctx->op = op;
	ctx->s = s;
	ctx->sid = s ? s->id : -1;
	ctx->bufsize = bufsize;
	if (bufsize > 0) {
		ctx->buffer = MALLOC(bufsize);
		ctx->wsa.buf = ctx->buffer;
		ctx->wsa.len = bufsize;
	}
	return ctx;
}

static void
free_iocp_op(struct iocp_op *ctx, bool free_buffer) {
	if (ctx->buffer && free_buffer) {
		FREE(ctx->buffer);
	}
	FREE(ctx);
}

static int
post_read(struct socket_server *ss, struct socket *s) {
	if (s->read_posted || s->closing || !s->reading) return 0;
	struct iocp_op *op = alloc_iocp_op(IOCP_OP_READ, s, s->p.size);
	DWORD flags = 0;
	DWORD recv_bytes = 0;
	int r = WSARecv(s->fd, &op->wsa, 1, &recv_bytes, &flags, &op->ol, NULL);
	if (r == SOCKET_ERROR) {
		int err = WSAGetLastError();
		if (err != WSA_IO_PENDING) {
			free_iocp_op(op, true);
			// fail to post read, report error and close
			struct socket_message msg;
			report_error_code(s, &msg, err);
			push_msg(ss, &msg);
			force_close(ss, s, &msg);
			return -1;
		}
	}
	s->read_posted = true;
	(void)ss;
	return 0;
}

static int
post_udp_recv(struct socket_server *ss, struct socket *s) {
	if (s->read_posted || s->closing || !s->reading) return 0;
	struct iocp_op *op = alloc_iocp_op(IOCP_OP_UDP_RECV, s, MAX_UDP_PACKAGE);
	op->addrlen = sizeof(op->addr);
	DWORD flags = 0;
	DWORD recv_bytes = 0;
	int r = WSARecvFrom(s->fd, &op->wsa, 1, &recv_bytes, &flags, (struct sockaddr *)&op->addr, &op->addrlen, &op->ol, NULL);
	if (r == SOCKET_ERROR) {
		int err = WSAGetLastError();
		if (err != WSA_IO_PENDING) {
			free_iocp_op(op, true);
			// fail to post udp recv, report error and close
			struct socket_message msg;
			report_error_code(s, &msg, err);
			push_msg(ss, &msg);
			force_close(ss, s, &msg);
			return -1;
		}
	}
	s->read_posted = true;
	(void)ss;
	return 0;
}

static void
post_accept(struct socket_server *ss, struct socket *listen_s) {
	if (!ss->acceptex) return;
	struct iocp_op *op = alloc_iocp_op(IOCP_OP_ACCEPT, listen_s, 0);
	SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	op->accept_socket = client;
	DWORD bytes = 0;
	// AcceptEx output buffer must outlive post_accept(); use op->addr (sockaddr_storage, 128B >= needed 88B).
	memset(&op->addr, 0, sizeof(op->addr));
	BOOL ok = ss->acceptex(listen_s->fd, client, &op->addr, 0,
					 sizeof(struct sockaddr_in6) + 16,
					 sizeof(struct sockaddr_in6) + 16,
					 &bytes, &op->ol);
	if (!ok) {
		int err = WSAGetLastError();
		if (err != ERROR_IO_PENDING) {
			closesocket(client);
			free_iocp_op(op, false);
		}
	}
}

static int
post_write(struct socket_server *ss, struct socket *s, struct write_buffer *wb) {
	if (s->write_posted) return 0;
	struct iocp_op *op = alloc_iocp_op(IOCP_OP_WRITE, s, 0);
	op->buffer = wb->ptr;
	op->wsa.buf = wb->ptr;
	op->wsa.len = (ULONG)wb->sz;
	op->wb = wb;
	op->write_len = wb->sz;
	DWORD sent = 0;
	int r = WSASend(s->fd, &op->wsa, 1, &sent, 0, &op->ol, NULL);
	if (r == SOCKET_ERROR) {
		int err = WSAGetLastError();
		if (err != WSA_IO_PENDING) {
			free_iocp_op(op, false);
			return -1;
		}
	}
	s->write_posted = true;
	(void)ss;
	return 0;
}

static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send * request, int size) {
	struct write_buffer * buf = MALLOC(size);
	struct send_object so;
	buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);
	buf->ptr = (char*)so.buffer;
	buf->sz = so.sz;
	buf->buffer = request->buffer;
	buf->next = NULL;
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}
	return buf;
}

static struct write_buffer *
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send * request, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
	struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;
	struct write_buffer_udp * buf = (struct write_buffer_udp *)append_sendbuffer_(ss, wl, request, SIZEOF_UDPBUFFER);
	memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);
	return &buf->buffer;
}

static void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send * request, int priority, const uint8_t *udp_address) {
	if (s->protocol == PROTOCOL_TCP) {
		append_sendbuffer_(ss, (priority == PRIORITY_HIGH) ? &s->high : &s->low, request, SIZEOF_TCPBUFFER);
	} else {
		append_sendbuffer_udp(ss, s, priority, request, udp_address);
	}
	s->wb_size += request->sz;
}

static struct write_buffer *
next_write_buffer(struct socket *s) {
	if (s->high.head) return s->high.head;
	return s->low.head;
}

static void
consume_write_buffer(struct socket_server *ss, struct socket *s, struct write_buffer *wb) {
	struct wb_list *list = (s->high.head == wb) ? &s->high : &s->low;
	list->head = wb->next;
	if (!list->head) list->tail = NULL;
	write_buffer_free(ss, wb);
}

static void
try_post_write(struct socket_server *ss, struct socket *s) {
	if (s->write_posted) return;
	struct write_buffer *wb = next_write_buffer(s);
	if (!wb) return;
	post_write(ss, s, wb);
}

static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result, int priority, const uint8_t *udp_address) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id) || s->closing) {
		if (request->sz == USEROBJECT) {
			ss->soi.free((void *)request->buffer);
		} else {
			FREE((void *)request->buffer);
		}
		return -1;
	}
	if (s->protocol != PROTOCOL_TCP) {
		append_sendbuffer(ss, s, request, priority, udp_address);
		struct write_buffer *wb = next_write_buffer(s);
		if (!wb) {
			return -1;
		}
		struct write_buffer_udp *udp = (struct write_buffer_udp *)wb;
		union sockaddr_all sa;
		socklen_t sasz = udp_socket_address(s, udp->udp_address, &sa);
		if (sasz == 0) {
			consume_write_buffer(ss, s, wb);
			return -1;
		}
		int err = sendto(s->fd, wb->ptr, (int)wb->sz, 0, &sa.s, sasz);
		if (err < 0) {
			consume_write_buffer(ss, s, wb);
			return -1;
		}
		stat_write(ss, s, (int)wb->sz);
		s->wb_size -= wb->sz;
		consume_write_buffer(ss, s, wb);
		goto _check_warn;
	}
	append_sendbuffer(ss, s, request, priority, udp_address);
	try_post_write(ss, s);
_check_warn:
	if (s->wb_size >= WARNING_SIZE && s->wb_size >= s->warn_size) {
		s->warn_size = (s->warn_size == 0) ? WARNING_SIZE * 2 : s->warn_size * 2;
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = s->wb_size % 1024 == 0 ? s->wb_size / 1024 : s->wb_size / 1024 + 1;
		result->data = NULL;
		return SOCKET_WARNING;
	}
	return -1;
}

static int
trigger_write(struct socket_server *ss, struct request_send * request, struct socket_message *result) {
	return send_socket(ss, request, result, PRIORITY_HIGH, NULL);
}

static int
set_udp_address(struct socket_server *ss, struct request_setudp *request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		return -1;
	}
	int type = request->address[0];
	if (type != s->protocol) {
		return report_error(s, result, "protocol mismatch");
	}
	memcpy(s->p.udp_address, request->address, UDP_ADDRESS_SIZE);
	if (ATOM_LOAD(&s->udpconnecting) > 0) {
		ATOM_FDEC(&s->udpconnecting);
	}
	(void)ss;
	(void)result;
	return -1;
}

static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {
	int type = (uint8_t)udp_address[0];
	if (type != s->protocol)
		return 0;
	uint16_t port = 0;
	memcpy(&port, udp_address+1, sizeof(uint16_t));
	switch (s->protocol) {
	case PROTOCOL_UDP:
		memset(&sa->v4, 0, sizeof(sa->v4));
		sa->s.sa_family = AF_INET;
		sa->v4.sin_port = port;
		memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v4.sin_addr));
		return sizeof(sa->v4);
	case PROTOCOL_UDPv6:
		memset(&sa->v6, 0, sizeof(sa->v6));
		sa->s.sa_family = AF_INET6;
		sa->v6.sin6_port = port;
		memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v6.sin6_addr));
		return sizeof(sa->v6);
	}
	return 0;
}

static int
gen_udp_address(int protocol, struct sockaddr_storage *sa, uint8_t * udp_address) {
	int addrsz = 1;
	udp_address[0] = (uint8_t)protocol;
	if (protocol == PROTOCOL_UDP) {
		struct sockaddr_in *v4 = (struct sockaddr_in *)sa;
		memcpy(udp_address+addrsz, &v4->sin_port, sizeof(v4->sin_port));
		addrsz += sizeof(v4->sin_port);
		memcpy(udp_address+addrsz, &v4->sin_addr, sizeof(v4->sin_addr));
		addrsz += sizeof(v4->sin_addr);
	} else {
		struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)sa;
		memcpy(udp_address+addrsz, &v6->sin6_port, sizeof(v6->sin6_port));
		addrsz += sizeof(v6->sin6_port);
		memcpy(udp_address+addrsz, &v6->sin6_addr, sizeof(v6->sin6_addr));
		addrsz += sizeof(v6->sin6_addr);
	}
	return addrsz;
}

static int
report_error(struct socket *s, struct socket_message *result, const char *err) {
	result->id = s->id;
	result->ud = 0;
	result->opaque = s->opaque;
	result->data = (char *)err;
	return SOCKET_ERR;
}

static int
report_error_code(struct socket *s, struct socket_message *result, int err) {
	static char errbuf[256];
	return report_error(s, result, win32_strerror(err, errbuf, sizeof(errbuf)));
}

static void
close_read(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	ATOM_STORE(&s->type , SOCKET_TYPE_HALFCLOSE_READ);
	s->reading = false;
	shutdown(s->fd, SD_RECEIVE);
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	(void)ss;
}

static inline int
halfclose_read(struct socket *s) {
	return ATOM_LOAD(&s->type) == SOCKET_TYPE_HALFCLOSE_READ;
}

static int
close_write(struct socket_server *ss, struct socket *s, struct socket_message *result, int errcode) {
	if (s->closing) {
		force_close(ss, s, result);
		return SOCKET_RST;
	}
	int t = ATOM_LOAD(&s->type);
	if (t == SOCKET_TYPE_HALFCLOSE_READ) {
		force_close(ss, s, result);
		return SOCKET_RST;
	}
	if (t == SOCKET_TYPE_HALFCLOSE_WRITE) {
		return SOCKET_RST;
	}
	ATOM_STORE(&s->type, SOCKET_TYPE_HALFCLOSE_WRITE);
	shutdown(s->fd, SD_SEND);
	return report_error_code(s, result, errcode);
}

// 检查并完成延迟的资源释放
static void
check_delayed_close(struct socket_server *ss, struct socket *s) {
	if (s->closing && !s->read_posted && !s->write_posted) {
		// 所有 I/O 操作都已完成，可以安全释放资源
		free_wb_list(ss, &s->high);
		free_wb_list(ss, &s->low);
		if (s->fd != INVALID_SOCKET) {
			closesocket(s->fd);
			s->fd = INVALID_SOCKET;
		}
		ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
		if (s->dw_buffer) {
			struct socket_sendbuffer tmp;
			tmp.buffer = s->dw_buffer;
			tmp.sz = s->dw_size;
			tmp.id = s->id;
			tmp.type = (tmp.sz == USEROBJECT) ? SOCKET_BUFFER_OBJECT : SOCKET_BUFFER_MEMORY;
			free_buffer(ss, &tmp);
			s->dw_buffer = NULL;
		}
	}
}

static void
force_close(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	uint8_t type = ATOM_LOAD(&s->type);
	if (type == SOCKET_TYPE_INVALID) {
		return;
	}

	// 取消所有挂起的 I/O 操作
	if (s->fd != INVALID_SOCKET) {
		CancelIoEx((HANDLE)s->fd, NULL);
	}

	// 标记为 closing，防止新的 I/O 操作
	s->closing = true;

	// 只有在没有挂起的 I/O 操作时才立即释放资源
	if (!s->read_posted && !s->write_posted) {
		free_wb_list(ss,&s->high);
		free_wb_list(ss,&s->low);
		if (s->fd != INVALID_SOCKET) {
			closesocket(s->fd);
			s->fd = INVALID_SOCKET;
		}
		ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
		if (s->dw_buffer) {
			struct socket_sendbuffer tmp;
			tmp.buffer = s->dw_buffer;
			tmp.sz = s->dw_size;
			tmp.id = s->id;
			tmp.type = (tmp.sz == USEROBJECT) ? SOCKET_BUFFER_OBJECT : SOCKET_BUFFER_MEMORY;
			free_buffer(ss, &tmp);
			s->dw_buffer = NULL;
		}
	}
	// 否则，资源会在 I/O 完成回调中释放
}

static int
open_socket(struct socket_server *ss, struct request_open * request, struct socket_message *result) {
	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;
	char port[16];
	snprintf(port, sizeof(port), "%d", request->port);
	memset(&ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	int status = getaddrinfo( request->host, port, &ai_hints, &ai_list );
	if ( status != 0 ) {
		result->data = (void *)gai_strerror(status);
		goto _failed_getaddrinfo;
	}
	SOCKET sock = INVALID_SOCKET;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock == INVALID_SOCKET ) {
			continue;
		}
		socket_keepalive(sock);
		socket_nonblocking(sock);
		break;
	}

	if (sock == INVALID_SOCKET) {
		static char errbuf[256];
		result->data = (void *)wsa_strerror(last_wsa_error(), errbuf, sizeof(errbuf));
		goto _failed;
	}

	struct socket *ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque);
	if (ns == NULL) {
		result->data = "reach skynet socket number limit";
		goto _failed;
	}

	if (ss->connectex == NULL) {
		result->data = "ConnectEx unavailable";
		goto _failed;
	}

	if (ai_ptr->ai_family == AF_INET6) {
		struct sockaddr_in6 bind_addr;
		memset(&bind_addr, 0, sizeof(bind_addr));
		bind_addr.sin6_family = AF_INET6;
		bind_addr.sin6_addr = in6addr_any;
		bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
	} else {
		struct sockaddr_in bind_addr;
		memset(&bind_addr, 0, sizeof(bind_addr));
		bind_addr.sin_family = AF_INET;
		bind_addr.sin_addr.s_addr = INADDR_ANY;
		bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
	}

	struct iocp_op *op = alloc_iocp_op(IOCP_OP_CONNECT, ns, 0);
	BOOL ok = ss->connectex(sock, ai_ptr->ai_addr, (int)ai_ptr->ai_addrlen, NULL, 0, NULL, &op->ol);
	if (!ok) {
		int err = last_wsa_error();
		if (err != ERROR_IO_PENDING) {
			free_iocp_op(op, false);
			static char errbuf[256];
			result->data = (void *)wsa_strerror(err, errbuf, sizeof(errbuf));
			goto _failed;
		}
	}

	ATOM_STORE(&ns->type , SOCKET_TYPE_CONNECTING);
	freeaddrinfo(ai_list);
	return -1;
_failed:
	if (sock != INVALID_SOCKET)
		closesocket(sock);
	freeaddrinfo(ai_list);
_failed_getaddrinfo:
	ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
	return SOCKET_ERR;
}

static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {
	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;

	SOCKET sock = (SOCKET)request->fd;
	if (sock == INVALID_SOCKET) {
		ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
		result->data = "Invalid fd";
		return SOCKET_ERR;
	}

	struct socket *s = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque);
	if (!s) {
		result->data = "reach skynet socket number limit";
		goto _failed;
	}

	ATOM_STORE(&s->type , SOCKET_TYPE_PLISTEN);

	union sockaddr_all u;
	socklen_t slen = sizeof(u);
	if (getsockname(sock, &u.s, &slen) == 0) {
		void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
		if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer)) == 0) {
			result->data = strerror(errno);
			return SOCKET_ERR;
		}
		int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
		result->data = ss->buffer;
		result->ud = sin_port;
	} else {
		result->data = strerror(errno);
		return SOCKET_ERR;
	}

	return SOCKET_OPEN;
_failed:
	closesocket(sock);
	ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
	return SOCKET_ERR;
}

static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;

	if (request->fd == 0) {
		struct socket *s = &ss->slot[HASH_ID(id)];
		s->id = id;
		s->fd = 0;
		s->opaque = request->opaque;
		s->protocol = PROTOCOL_TCP;
		s->reading = true;
		s->writing = false;
		s->closing = false;
		s->p.size = MIN_READ_BUFFER;
		s->warn_size = 0;
		s->wb_size = 0;
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
		ATOM_STORE(&s->type , SOCKET_TYPE_BIND);
		ss->stdin_socket = s;
		ss->stdin_id = id;
		result->data = "binding";
		return SOCKET_OPEN;
	}

	struct socket *s = new_fd(ss, id, (SOCKET)request->fd, PROTOCOL_TCP, request->opaque);
	if (s == NULL) {
		result->data = "reach skynet socket number limit";
		return SOCKET_ERR;
	}
	ATOM_STORE(&s->type , SOCKET_TYPE_BIND);
	result->data = "binding";
	return SOCKET_OPEN;
}

static int
resume_socket(struct socket_server *ss, struct request_resumepause *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		result->data = "invalid socket";
		return SOCKET_ERR;
	}
	if (halfclose_read(s)) {
		result->data = "socket closed";
		return SOCKET_ERR;
	}
	uint8_t type = ATOM_LOAD(&s->type);
	if (type == SOCKET_TYPE_PACCEPT) {
		ATOM_STORE(&s->type , SOCKET_TYPE_CONNECTED);
		s->opaque = request->opaque;
		s->reading = true;
		post_read(ss, s);
		result->data = "start";
		return SOCKET_OPEN;
	} else if (type == SOCKET_TYPE_PLISTEN) {
		ATOM_STORE(&s->type , SOCKET_TYPE_LISTEN);
		s->opaque = request->opaque;
		post_accept(ss, s);
		result->data = "start";
		return SOCKET_OPEN;
	} else if (type == SOCKET_TYPE_CONNECTED) {
		s->opaque = request->opaque;
		s->reading = true;
		if (s->protocol == PROTOCOL_TCP) {
			post_read(ss, s);
		} else {
			post_udp_recv(ss, s);
		}
		result->data = "transfer";
		return SOCKET_OPEN;
	}
	s->reading = true;
	if (s->protocol == PROTOCOL_TCP) {
		post_read(ss, s);
	} else {
		post_udp_recv(ss, s);
	}
	(void)result;
	return -1;
}

static int
pause_socket(struct socket_server *ss, struct request_resumepause *request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		return -1;
	}
	s->reading = false;
	CancelIoEx((HANDLE)s->fd, NULL);
	(void)result;
	return -1;
}

static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
	struct socket *s = &ss->slot[HASH_ID(request->id)];
	if (socket_invalid(s, request->id)) {
		return;
	}
	setsockopt(s->fd, SOL_SOCKET, request->what, (const char *)&request->value, sizeof(int));
	(void)ss;
}

static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
	struct socket *s = &ss->slot[HASH_ID(udp->id)];
	s->fd = (SOCKET)udp->fd;
	s->id = udp->id;
	s->opaque = udp->opaque;
	s->protocol = (udp->family == AF_INET) ? PROTOCOL_UDP : PROTOCOL_UDPv6;
	clear_wb_list(&s->high);
	clear_wb_list(&s->low);
	s->wb_size = 0;
	s->warn_size = 0;
	s->p.size = MIN_READ_BUFFER;
	s->reading = true;
	s->closing = false;
	s->read_posted = false;
	s->write_posted = false;
	ATOM_STORE(&s->type , SOCKET_TYPE_CONNECTED);
	CreateIoCompletionPort((HANDLE)s->fd, ss->iocp, (ULONG_PTR)s, 0);
	post_udp_recv(ss, s);
}

static int
dial_udp_socket(struct socket_server *ss, struct request_dial_udp *request, struct socket_message *result) {
	int id = request->id;
	int protocol = request->address[0];
	struct socket *s = &ss->slot[HASH_ID(id)];
	s->fd = (SOCKET)request->fd;
	s->id = id;
	s->opaque = request->opaque;
	s->protocol = protocol;
	clear_wb_list(&s->high);
	clear_wb_list(&s->low);
	s->wb_size = 0;
	s->warn_size = 0;
	s->reading = true;
	s->closing = false;
	s->read_posted = false;
	s->write_posted = false;
	if (protocol == PROTOCOL_UDP) {
		memcpy(s->p.udp_address, request->address, 1 + 2 + 4);
	} else {
		memcpy(s->p.udp_address, request->address, 1 + 2 + 16);
	}
	ATOM_STORE(&s->type, SOCKET_TYPE_CONNECTED);
	CreateIoCompletionPort((HANDLE)s->fd, ss->iocp, (ULONG_PTR)s, 0);
	post_udp_recv(ss, s);
	(void)result;
	return -1;
}

static void
dec_sending_ref(struct socket_server *ss, int id) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id == id && s->protocol == PROTOCOL_TCP) {
		assert((ATOM_LOAD(&s->sending) & 0xffff) != 0);
		ATOM_FDEC(&s->sending);
	}
	(void)ss;
}

static inline void
inc_sending_ref(struct socket *s, int id) {
	for (;;) {
		unsigned long sending = ATOM_LOAD(&s->sending);
		if ((sending >> 16) == ID_TAG16(id)) {
			if ((sending & 0xffff) == 0xffff) {
				// sending counter overflow, wait for socket thread to dec
				continue;
			}
			if (ATOM_CAS_ULONG(&s->sending, sending, sending + 1)) {
				return;
			}
		} else {
			return;
		}
	}
}

static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		return -1;
	}
	int shutdown_read = halfclose_read(s);
	if (request->shutdown || nomore_sending_data(s)) {
		int r = shutdown_read ? -1 : SOCKET_CLOSE;
		CancelIoEx((HANDLE)s->fd, NULL);
		force_close(ss, s, result);
		return r;
	}
	s->closing = true;
	if (!shutdown_read) {
		close_read(ss, s, result);
		return SOCKET_CLOSE;
	}
	return -1;
}

static int
handle_ctrl(struct socket_server *ss, struct socket_message *result) {
	struct ctrl_request *req = pop_ctrl(ss);
	if (!req) return -1;
	int type = req->type;
	int ret = -1;
	struct request_package pkg;
	memset(&pkg, 0, sizeof(pkg));
	memcpy(&pkg.u, req->data, req->len);
	switch (type) {
	case 'R':
		ret = resume_socket(ss,(struct request_resumepause *)&pkg.u, result);
		break;
	case 'S':
		ret = pause_socket(ss,(struct request_resumepause *)&pkg.u, result);
		break;
	case 'B':
		ret = bind_socket(ss,(struct request_bind *)&pkg.u, result);
		break;
	case 'L':
		ret = listen_socket(ss,(struct request_listen *)&pkg.u, result);
		break;
	case 'K':
		ret = close_socket(ss,(struct request_close *)&pkg.u, result);
		break;
	case 'O':
		ret = open_socket(ss,(struct request_open *)&pkg.u, result);
		break;
	case 'X':
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		ret = SOCKET_EXIT;
		break;
	case 'W':
		ret = trigger_write(ss,(struct request_send *)&pkg.u, result);
		break;
	case 'D':
	case 'P': {
		int priority = (type == 'D') ? PRIORITY_HIGH : PRIORITY_LOW;
		struct request_send * request = (struct request_send *)&pkg.u;
		ret = send_socket(ss, request, result, priority, NULL);
		dec_sending_ref(ss, request->id);
		break;
	}
	case 'A': {
		struct request_send_udp * rsu = (struct request_send_udp *)&pkg.u;
		ret = send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
		break;
	}
	case 'C':
		ret = set_udp_address(ss, (struct request_setudp *)&pkg.u, result);
		break;
	case 'N':
		ret = dial_udp_socket(ss, (struct request_dial_udp *)&pkg.u, result);
		break;
	case 'T':
		setopt_socket(ss, (struct request_setopt *)&pkg.u);
		ret = -1;
		break;
	case 'U':
		add_udp_socket(ss, (struct request_udp *)&pkg.u);
		ret = -1;
		break;
	default:
		skynet_error(NULL, "socket-server: Unknown ctrl %c.", type);
		ret = -1;
		break;
	}
	free_ctrl(req);
	return ret;
}

static int
handle_accept_complete(struct socket_server *ss, struct iocp_op *op, struct socket_message *result) {
	struct socket *ls = op->s;
	if (ATOM_LOAD(&ls->type) != SOCKET_TYPE_LISTEN) {
		free_iocp_op(op, false);
		return -1;
	}
	SOCKET client = op->accept_socket;
	if (client == INVALID_SOCKET) { free_iocp_op(op, false); return -1; }
	setsockopt(client, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&ls->fd, sizeof(ls->fd));
	socket_keepalive(client);
	socket_nonblocking(client);
	int id = reserve_id(ss);
	if (id < 0) {
		closesocket(client);
		free_iocp_op(op, false);
		return -1;
	}
	struct socket *ns = new_fd(ss, id, client, PROTOCOL_TCP, ls->opaque);
	if (ns == NULL) {
		closesocket(client);
		free_iocp_op(op, false);
		return -1;
	}
	ATOM_STORE(&ns->type , SOCKET_TYPE_PACCEPT);
	result->opaque = ls->opaque;
	result->id = ls->id;
	result->ud = id;
	result->data = NULL;
	union sockaddr_all u;
	socklen_t slen = sizeof(u);
	if (getpeername(client, &u.s, &slen) == 0) {
		if (getname(&u, ss->buffer, sizeof(ss->buffer))) {
			result->data = ss->buffer;
		}
	}
	post_accept(ss, ls);
	free_iocp_op(op, false);
	return SOCKET_ACCEPT;
}

static int
handle_connect_complete(struct socket_server *ss, struct iocp_op *op, struct socket_message *result) {
	struct socket *s = op->s;
	if (ATOM_LOAD(&s->type) != SOCKET_TYPE_CONNECTING) {
		free_iocp_op(op, false);
		return -1;
	}
	setsockopt(s->fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
	ATOM_STORE(&s->type , SOCKET_TYPE_CONNECTED);
	s->reading = true;
	post_read(ss, s);
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	union sockaddr_all u;
	socklen_t slen = sizeof(u);
	if (getpeername(s->fd, &u.s, &slen) == 0) {
		void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
		if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			result->data = ss->buffer;
		}
	}
	return SOCKET_OPEN;
}

static int
handle_read_complete(struct socket_server *ss, struct iocp_op *op, DWORD bytes, struct socket_message *result) {
	struct socket *s = op->s;
	if (ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID) {
		free_iocp_op(op, true);
		return -1;
	}
	s->read_posted = false;

	// 检查是否需要延迟清理
	check_delayed_close(ss, s);

	// 如果 socket 已经被清理或被重用，直接返回
	if (ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID || op->sid != s->id) {
		free_iocp_op(op, true);
		return -1;
	}

	if (bytes == 0) {
		free_iocp_op(op, true);
		if (s->closing) {
			if (nomore_sending_data(s)) {
				force_close(ss, s, result);
			}
			return -1;
		}
		if (halfclose_read(s)) {
			return -1;
		}
		int t = ATOM_LOAD(&s->type);
		if (t == SOCKET_TYPE_HALFCLOSE_WRITE) {
			force_close(ss, s, result);
			return SOCKET_CLOSE;
		}
		close_read(ss, s, result);
		return SOCKET_CLOSE;
	}
	stat_read(ss, s, (int)bytes);
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = (int)bytes;
	result->data = op->buffer;
	if ((int)bytes == op->bufsize) {
		if (s->p.size < 65536) s->p.size *= 2;
	}
	post_read(ss, s);
	free_iocp_op(op, false);
	return SOCKET_DATA;
}

static int
handle_udp_recv_complete(struct socket_server *ss, struct iocp_op *op, DWORD bytes, struct socket_message *result) {
	struct socket *s = op->s;
	if (ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID) {
		free_iocp_op(op, true);
		return -1;
	}
	s->read_posted = false;

	// 检查是否需要延迟清理
	check_delayed_close(ss, s);

	// 如果 socket 已经被清理或被重用，直接返回
	if (ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID || op->sid != s->id) {
		free_iocp_op(op, true);
		return -1;
	}

	if (bytes == 0) {
		free_iocp_op(op, true);
		return -1;
	}
	stat_read(ss, s, (int)bytes);
	uint8_t * data = MALLOC(bytes + UDP_ADDRESS_SIZE);
	memcpy(data, op->buffer, bytes);
	gen_udp_address(s->protocol, &op->addr, data + bytes);
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = (int)bytes;
	result->data = (char *)data;
	post_udp_recv(ss, s);
	free_iocp_op(op, true);
	return SOCKET_UDP;
}

static int
handle_write_complete(struct socket_server *ss, struct iocp_op *op, DWORD bytes, struct socket_message *result) {
	struct socket *s = op->s;
	if (ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID) {
		free_iocp_op(op, false);
		return -1;
	}
	s->write_posted = false;

	// 不在这里调用 check_delayed_close：
	// 若此时 closing=true 且 read_posted=false，提前清理会把队列里还未发送的
	// write_buffer（如 HTTP body）一并释放，导致多段 write 只发出第一段。
	// 正确做法是先尝试投递下一段，确认无后续写后再做延迟清理。

	// 如果 socket 已经被清理或被重用，直接返回
	if (ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID || op->sid != s->id) {
		free_iocp_op(op, false);
		return -1;
	}

	struct write_buffer *wb = op->wb;
	if (!wb) {
		check_delayed_close(ss, s);
		free_iocp_op(op, false);
		return -1;
	}
	stat_write(ss, s, (int)bytes);
	s->wb_size -= bytes;
	if (bytes < wb->sz) {
		wb->ptr += bytes;
		wb->sz -= bytes;
		try_post_write(ss, s);
		// 只有在没有新写投递成功（write_posted 仍为 false）时才做延迟清理
		if (!s->write_posted) {
			check_delayed_close(ss, s);
		}
		free_iocp_op(op, false);
		return -1;
	}
	consume_write_buffer(ss, s, wb);
	try_post_write(ss, s);
	// 只有在没有新写投递成功（write_posted 仍为 false）时才做延迟清理
	if (!s->write_posted) {
		check_delayed_close(ss, s);
	}
	free_iocp_op(op, false);
	return -1;
}

static int
handle_io_completion(struct socket_server *ss, struct iocp_op *op, DWORD bytes, DWORD err, struct socket_message *result) {
	if (op == NULL) {
		skynet_error(NULL, "handle_io_completion: op is NULL");
		return -1;
	}

	// 通过 sid 重新获取 socket，而不是直接使用 op->s
	// 因为 op->s 可能指向已释放或重用的内存
	struct socket *s = NULL;
	if (op->sid >= 0) {
		s = &ss->slot[HASH_ID(op->sid)];
		// 验证 socket 是否仍然有效
		if (s->id != op->sid || ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID) {
			skynet_error(NULL, "handle_io_completion: socket invalid, op->sid=%d, s->id=%d, s->type=%d",
				op->sid, s->id, ATOM_LOAD(&s->type));
			free_iocp_op(op, (op->op == IOCP_OP_READ || op->op == IOCP_OP_UDP_RECV));
			return -1;
		}
	} else {
		skynet_error(NULL, "handle_io_completion: op->sid < 0");
		free_iocp_op(op, (op->op == IOCP_OP_READ || op->op == IOCP_OP_UDP_RECV));
		return -1;
	}

	// 更新 op->s 为有效的 socket 指针
	op->s = s;
	if (err != 0) {
		if (err == ERROR_OPERATION_ABORTED || err == ERROR_IO_PENDING) {
			// 操作被取消（通常是 CancelIoEx 导致的）
			// 需要更新 posted 标志并检查延迟清理
			if (op->op == IOCP_OP_READ || op->op == IOCP_OP_UDP_RECV) {
				s->read_posted = false;
			} else if (op->op == IOCP_OP_WRITE) {
				s->write_posted = false;
			}
			free_iocp_op(op, (op->op == IOCP_OP_READ || op->op == IOCP_OP_UDP_RECV));

			// 注意：此路径对应 force_close → CancelIoEx 触发的 ABORTED
			// 此时 op->wb（被取消的写操作）仍留在队列头，不能调用 try_post_write：
			// 否则会把同一块 wb1 重新投递一遍，导致 TCP 数据重复。
			// 应直接走延迟清理：check_delayed_close 会通过 free_wb_list 释放队列。
			check_delayed_close(ss, s);
			return -1;
		}
		int code = (int)err;
		if (op->op == IOCP_OP_ACCEPT) {
			SOCKET acc = op->accept_socket;
			if (acc != INVALID_SOCKET) {
				closesocket(acc);
			}
			if (s && ATOM_LOAD(&s->type) == SOCKET_TYPE_LISTEN) {
				post_accept(ss, s);
			}
			free_iocp_op(op, false);
			return -1;
		}
		if (op->op == IOCP_OP_CONNECT) {
			free_iocp_op(op, false);
			if (s && s->fd != INVALID_SOCKET) {
				closesocket(s->fd);
			}
			if (s) {
				ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
				return report_error_code(s, result, code);
			}
			return -1;
		}
		if (op->op == IOCP_OP_WRITE) {
			free_iocp_op(op, false);
			return close_write(ss, s, result, code);
		}
		if (op->op == IOCP_OP_READ || op->op == IOCP_OP_UDP_RECV) {
			free_iocp_op(op, true);
		} else {
			free_iocp_op(op, false);
		}
		if (s) {
			force_close(ss, s, result);
			return report_error_code(s, result, code);
		}
		return -1;
	}
	switch (op->op) {
	case IOCP_OP_ACCEPT:
		return handle_accept_complete(ss, op, result);
	case IOCP_OP_CONNECT:
		return handle_connect_complete(ss, op, result);
	case IOCP_OP_READ:
		return handle_read_complete(ss, op, bytes, result);
	case IOCP_OP_WRITE:
		return handle_write_complete(ss, op, bytes, result);
	case IOCP_OP_UDP_RECV:
		return handle_udp_recv_complete(ss, op, bytes, result);
	default:
		break;
	}
	return -1;
}

struct socket_server *
socket_server_create(uint64_t time) {
	WSADATA wsadata;
	if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
		skynet_error(NULL, "socket-server: WSAStartup failed.");
		return NULL;
	}
	struct socket_server *ss = MALLOC(sizeof(*ss));
	memset(ss, 0, sizeof(*ss));
	ss->time = time;
	ss->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (ss->iocp == NULL) {
		FREE(ss);
		return NULL;
	}
	InitializeCriticalSection(&ss->ctrl_lock);
	ATOM_INIT(&ss->ctrl_count, 0);
	ATOM_INIT(&ss->alloc_id , 0);
	memset(&ss->soi, 0, sizeof(ss->soi));
	ss->stdin_socket = NULL;
	ss->stdin_id = -1;
	ss->stdin_len = 0;
	for (int i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		ATOM_INIT(&s->type, SOCKET_TYPE_INVALID);
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
		spinlock_init(&s->dw_lock);
	}

	GUID guid_accept = WSAID_ACCEPTEX;
	GUID guid_connect = WSAID_CONNECTEX;
	DWORD bytes = 0;
	SOCKET tmp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid_accept, sizeof(guid_accept), &ss->acceptex, sizeof(ss->acceptex), &bytes, NULL, NULL);
	WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid_connect, sizeof(guid_connect), &ss->connectex, sizeof(ss->connectex), &bytes, NULL, NULL);
	closesocket(tmp);

	ss->reserve_fd = INVALID_SOCKET;
	return ss;
}

void
socket_server_release(struct socket_server *ss) {
	struct socket_message dummy;
	for (int i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		if (ATOM_LOAD(&s->type) != SOCKET_TYPE_RESERVE) {
			force_close(ss, s, &dummy);
		}
		spinlock_destroy(&s->dw_lock);
	}
	CloseHandle(ss->iocp);
	// 释放未处理完的 ctrl 请求
	for (;;) {
		struct ctrl_request *req = pop_ctrl(ss);
		if (!req) break;
		free_ctrl(req);
	}
	DeleteCriticalSection(&ss->ctrl_lock);
	FREE(ss);
	WSACleanup();
}

void
socket_server_updatetime(struct socket_server *ss, uint64_t time) {
	ss->time = time;
}

int
socket_server_poll(struct socket_server *ss, struct socket_message *result, int *more) {
	if (pop_msg(ss, result)) {
		if (more) *more = (ss->msg_head != NULL);
		return SOCKET_DATA;
	}
	for (;;) {
		if (ATOM_LOAD(&ss->ctrl_count) > 0) {
			int ret = handle_ctrl(ss, result);
			if (ret != -1) return ret;
			continue;
		}
		DWORD bytes = 0;
		ULONG_PTR key = 0;
		LPOVERLAPPED ov = NULL;
		BOOL ok = GetQueuedCompletionStatus(ss->iocp, &bytes, &key, &ov, 13);
		if (!ok && ov == NULL) {
			int ret = poll_stdin(ss, result);
			if (ret != -1) {
				return ret;
			}
			continue;
		}
		if (key == IOCP_KEY_CTRL) {
			int ret = handle_ctrl(ss, result);
			if (ret != -1) return ret;
			continue;
		}
		if (ov == NULL) {
			continue;
		}
		struct iocp_op *op = (struct iocp_op *)ov;
		DWORD err = ok ? 0 : GetLastError();
		int ret = handle_io_completion(ss, op, bytes, err, result);
		if (ret != -1) {
			return ret;
		}
	}
}

static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	if (len < 0 || len > 0xff) {
		skynet_error(NULL, "[iocp][send_request] invalid len type=%c len=%d", type, len);
		return;
	}
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;
	push_ctrl(ss, type, &request->u, (uint8_t)len);
}

static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = (int)strlen(addr);
	if (len + (int)sizeof(req->u.open) >= 256) {
		skynet_error(NULL, "socket-server : Invalid addr %s.",addr);
		return -1;
	}
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;
	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}

int
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;
	int len = open_request(ss, &request, opaque, addr, port);
	if (len < 0)
		return -1;
	send_request(ss, &request, 'O', (int)sizeof(request.u.open) + len);
	return request.u.open.id;
}

int
socket_server_send(struct socket_server *ss, struct socket_sendbuffer *buf) {
	int id = buf->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		free_buffer(ss, buf);
		return -1;
	}
	inc_sending_ref(s, id);
	struct request_package request;
	size_t sz = 0;
	const void * buffer = clone_buffer(buf, &sz);
	if (buffer == NULL)
		return -1;
	request.u.send.id = id;
	request.u.send.buffer = buffer;
	request.u.send.sz = sz;
	send_request(ss, &request, 'D', sizeof(request.u.send));
	return 0;
}

int
socket_server_send_lowpriority(struct socket_server *ss, struct socket_sendbuffer *buf) {
	int id = buf->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		free_buffer(ss, buf);
		return -1;
	}
	inc_sending_ref(s, id);
	struct request_package request;
	size_t sz = 0;
	const void * buffer = clone_buffer(buf, &sz);
	if (buffer == NULL)
		return -1;
	request.u.send.id = id;
	request.u.send.buffer = buffer;
	request.u.send.sz = sz;
	send_request(ss, &request, 'P', sizeof(request.u.send));
	return 0;
}

void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}

void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 0;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

void
socket_server_shutdown(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 1;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

int
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	struct request_package request;
	int len = (int)strlen(addr);
	if (len + (int)sizeof(request.u.listen) >= 256) {
		ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
		return -1;
	}

	SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd == INVALID_SOCKET) {
		ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
		return -1;
	}
	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

	struct sockaddr_in addr4;
	memset(&addr4, 0, sizeof(addr4));
	addr4.sin_family = AF_INET;
	addr4.sin_port = htons(port);
	addr4.sin_addr.s_addr = inet_addr(addr);
	if (bind(fd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0) {
		closesocket(fd);
		ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
		return -1;
	}
	if (listen(fd, backlog) < 0) {
		closesocket(fd);
		ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
		return -1;
	}

	request.u.listen.id = id;
	request.u.listen.fd = (int)fd;
	request.u.listen.opaque = opaque;
	memcpy(request.u.listen.host, addr, len);
	request.u.listen.host[len] = '\0';

	send_request(ss, &request, 'L', (int)sizeof(request.u.listen) + len);
	return id;
}

int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	struct request_package request;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	request.u.bind.opaque = opaque;
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}

void
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.resumepause.id = id;
	request.u.resumepause.opaque = opaque;
	send_request(ss, &request, 'R', sizeof(request.u.resumepause));
}

void
socket_server_pause(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.resumepause.id = id;
	request.u.resumepause.opaque = opaque;
	send_request(ss, &request, 'S', sizeof(request.u.resumepause));
}

void
socket_server_nodelay(struct socket_server *ss, int id) {
	struct request_package request;
	request.u.setopt.id = id;
	request.u.setopt.what = TCP_NODELAY;
	request.u.setopt.value = 1;
	send_request(ss, &request, 'T', sizeof(request.u.setopt));
}

void
socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
	ss->soi = *soi;
}

int
socket_server_udp(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	int family = AF_INET;
	SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd == INVALID_SOCKET) {
		ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
		return -1;
	}
	if (port != 0) {
		struct sockaddr_in addr4;
		memset(&addr4, 0, sizeof(addr4));
		addr4.sin_family = AF_INET;
		addr4.sin_port = htons(port);
		addr4.sin_addr.s_addr = (addr == NULL) ? INADDR_ANY : inet_addr(addr);
		if (bind(fd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0) {
			closesocket(fd);
			ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
			return -1;
		}
	}

	struct request_package request;
	request.u.udp.id = id;
	request.u.udp.fd = (int)fd;
	request.u.udp.family = family;
	request.u.udp.opaque = opaque;
	send_request(ss, &request, 'U', sizeof(request.u.udp));
	return id;
}

int
socket_server_udp_listen(struct socket_server *ss, uintptr_t opaque, const char* addr, int port) {
	if (port == 0) {
		return -1;
	}
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	int family = AF_INET;
	SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd == INVALID_SOCKET) {
		ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
		return -1;
	}
	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
	struct sockaddr_in addr4;
	memset(&addr4, 0, sizeof(addr4));
	addr4.sin_family = AF_INET;
	addr4.sin_port = htons(port);
	addr4.sin_addr.s_addr = (addr == NULL || addr[0] == 0) ? INADDR_ANY : inet_addr(addr);
	if (bind(fd, (struct sockaddr *)&addr4, sizeof(addr4)) != 0) {
		closesocket(fd);
		ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
		return -1;
	}
	struct request_package request;
	request.u.udp.id = id;
	request.u.udp.fd = (int)fd;
	request.u.udp.family = family;
	request.u.udp.opaque = opaque;
	send_request(ss, &request, 'U', sizeof(request.u.udp));
	return id;
}

int
socket_server_udp_dial(struct socket_server *ss, uintptr_t opaque, const char* addr, int port) {
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	sprintf(portstr, "%d", port);
	memset(&ai_hints, 0, sizeof(ai_hints));
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_DGRAM;
	ai_hints.ai_protocol = IPPROTO_UDP;

	status = getaddrinfo(addr, portstr, &ai_hints, &ai_list);
	if (status != 0) {
		return -1;
	}

	int protocol;
	if (ai_list->ai_family == AF_INET) {
		protocol = PROTOCOL_UDP;
	} else if (ai_list->ai_family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		freeaddrinfo(ai_list);
		return -1;
	}

	SOCKET fd = socket(ai_list->ai_family, SOCK_DGRAM, 0);
	if (fd == INVALID_SOCKET) {
		freeaddrinfo(ai_list);
		return -1;
	}

	int id = reserve_id(ss);
	if (id < 0) {
		closesocket(fd);
		freeaddrinfo(ai_list);
		return -1;
	}

	struct request_package request;
	request.u.dial_udp.id = id;
	request.u.dial_udp.fd = (int)fd;
	request.u.dial_udp.opaque = opaque;
	int addrsz = gen_udp_address(protocol, (struct sockaddr_storage *)ai_list->ai_addr, request.u.dial_udp.address);
	freeaddrinfo(ai_list);

	send_request(ss, &request, 'N', sizeof(request.u.dial_udp) - sizeof(request.u.dial_udp.address) + addrsz);
	return id;
}

int
socket_server_udp_send(struct socket_server *ss, const struct socket_udp_address *addr, struct socket_sendbuffer *buf) {
	struct request_package request;
	size_t sz = 0;
	const void * buffer = clone_buffer(buf, &sz);
	if (buffer == NULL)
		return -1;
	request.u.send_udp.send.id = buf->id;
	request.u.send_udp.send.buffer = buffer;
	request.u.send_udp.send.sz = sz;
	if (addr) {
		memcpy(request.u.send_udp.address, addr, UDP_ADDRESS_SIZE);
		send_request(ss, &request, 'A', sizeof(request.u.send_udp));
	} else {
		send_request(ss, &request, 'D', sizeof(request.u.send_udp.send));
	}
	return 0;
}

int
socket_server_udp_connect(struct socket_server *ss, int id, const char * addr, int port) {
	struct request_package request;
	request.u.setudp.id = id;
	request.u.setudp.address[0] = PROTOCOL_UDP;
	uint16_t nport = htons(port);
	memcpy(request.u.setudp.address+1, &nport, sizeof(nport));
	uint32_t naddr = inet_addr(addr);
	memcpy(request.u.setudp.address+1+sizeof(nport), &naddr, sizeof(naddr));
	send_request(ss, &request, 'C', sizeof(request.u.setudp));
	return 0;
}

const struct socket_udp_address *
socket_server_udp_address(struct socket_server *ss, struct socket_message *msg, int *addrsz) {
	if (msg->data == NULL)
		return NULL;
	int sz = msg->ud;
	uint8_t * data = (uint8_t *)msg->data;
	if (addrsz) *addrsz = UDP_ADDRESS_SIZE;
	(void)ss;
	return (const struct socket_udp_address *)(data + sz);
}

struct socket_info *
socket_info_create(struct socket_info *last) {
	struct socket_info *si = skynet_malloc(sizeof(*si));
	memset(si, 0 , sizeof(*si));
	si->next = last;
	return si;
}

void
socket_info_release(struct socket_info *si) {
	while (si) {
		struct socket_info *temp = si;
		si = si->next;
		skynet_free(temp);
	}
}

static int
getname(union sockaddr_all *u, char *buffer, size_t sz) {
	if (sz == 0) {
		return 0;
	}
	char tmp[INET6_ADDRSTRLEN];
	void * sin_addr = (u->s.sa_family == AF_INET) ? (void*)&u->v4.sin_addr : (void *)&u->v6.sin6_addr;
	if (inet_ntop(u->s.sa_family, sin_addr, tmp, sizeof(tmp))) {
		int sin_port = ntohs((u->s.sa_family == AF_INET) ? u->v4.sin_port : u->v6.sin6_port);
		snprintf(buffer, sz, "%s:%d", tmp, sin_port);
		return 1;
	} else {
		buffer[0] = '\0';
		return 0;
	}
}

static int
query_info(struct socket *s, struct socket_info *si) {
	union sockaddr_all u;
	socklen_t slen = sizeof(u);
	int closing = 0;
	switch (ATOM_LOAD(&s->type)) {
	case SOCKET_TYPE_BIND:
		si->type = SOCKET_INFO_BIND;
		si->name[0] = '\0';
		break;
	case SOCKET_TYPE_LISTEN:
		si->type = SOCKET_INFO_LISTEN;
		if (getsockname(s->fd, &u.s, &slen) == 0) {
			getname(&u, si->name, sizeof(si->name));
		}
		break;
	case SOCKET_TYPE_HALFCLOSE_READ:
	case SOCKET_TYPE_HALFCLOSE_WRITE:
		closing = 1;
	case SOCKET_TYPE_CONNECTED:
		if (s->protocol == PROTOCOL_TCP) {
			si->type = closing ? SOCKET_INFO_CLOSING : SOCKET_INFO_TCP;
			if (getpeername(s->fd, &u.s, &slen) == 0) {
				getname(&u, si->name, sizeof(si->name));
			}
		} else {
			si->type = SOCKET_INFO_UDP;
			if (udp_socket_address(s, s->p.udp_address, &u)) {
				getname(&u, si->name, sizeof(si->name));
			}
		}
		break;
	default:
		return 0;
	}
	si->id = s->id;
	si->opaque = (uint64_t)s->opaque;
	si->read = s->stat.read;
	si->write = s->stat.write;
	si->rtime = s->stat.rtime;
	si->wtime = s->stat.wtime;
	si->wbuffer = s->wb_size;
	si->reading = s->reading;
	si->writing = s->writing;

	return 1;
}

struct socket_info *
socket_server_info(struct socket_server *ss) {
	int i;
	struct socket_info * si = NULL;
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket * s = &ss->slot[i];
		int id = s->id;
		struct socket_info temp;
		if (query_info(s, &temp) && s->id == id) {
			si = socket_info_create(si);
			temp.next = si->next;
			*si = temp;
		}
	}
	return si;
}
