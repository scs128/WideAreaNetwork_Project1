#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h> 
#include <netdb.h>
#include <stdbool.h>

#define MAX_MESS_LEN 1366

#define MB 1000000

#define MODE_LAN 1
#define MODE_WAN 2

#define PKT_START      1
#define PKT_DATA       2
#define PKT_FIN        3
#define PKT_ACK        4 
#define PKT_BUSY       5

#define WINDOW_SIZE 9

#define MAX_RETRANSMISSIONS 10

    typedef struct dummy_ncp_msg {
        /* Fill in header information needed for your protocol */
        int flag;
        uint32_t seq;
        char payload[MAX_MESS_LEN];
        int size;
        int64_t  ts_sec;
        int32_t  ts_usec;
    } ncp_msg;

/* Uncomment and fill in fields for your protocol */
typedef struct dummy_rcv_msg {
    int flag;
    uint32_t seq; // Sequence number of cumulative acknowledged packet
    int last;
    bool buffer[WINDOW_SIZE]; // Receiver current buffer state from seq to last out of order packet received.
    int64_t  ts_sec;
    int32_t  ts_usec;
} rcv_msg;


struct upkt {
    int64_t  ts_sec;
    int32_t  ts_usec;
    uint32_t seq;
    char     payload[MAX_MESS_LEN];
};

typedef struct circ_buf {
    ncp_msg *buffer[WINDOW_SIZE];
    uint32_t seq; // sequence number represented at head
    int head;
    int maxlen;
} circular_buffer;

int circ_bbuf_push(circular_buffer *cb, ncp_msg *packet, int i);
ncp_msg *circ_bbuf_pop(circular_buffer *cb);
ncp_msg *circ_bbuf_get(circular_buffer *cb, int i);
