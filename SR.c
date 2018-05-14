#include <stdio.h>
#include <string.h>
#include<stdbool.h>
#include "protocol.h"
#include "datalink.h"

const int  MAX_SEQ = 31;
const int  MAX_S = 32;
#define BUF_LEN 16
#define DATA_TIMER  3000
#define ACK_TIMER 500
// packet: unsigned char [PKT_LEN]
typedef struct FRAME {
	unsigned char kind; /* FRAME_DATA */
	unsigned char ack;//piggyback
	unsigned char seq;
	unsigned char data[PKT_LEN];
	unsigned int  padding;
} frame;

static int phl_ready = 0;
static bool no_nak = true;
static void put_frame(unsigned char *frame, int len)
{
	*(unsigned int *)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = 0;
}

static void send_kind_frame(unsigned char kind, unsigned char ack_seq, unsigned char data_seq, unsigned char* buffer) {
	frame f;
	f.kind = kind;
	if (kind == FRAME_DATA) {
		memcpy(f.data, buffer, PKT_LEN);
		f.seq = data_seq;
		f.ack = ack_seq;
		dbg_frame("Send DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
		put_frame((unsigned char *)&f, 3 + PKT_LEN);
		start_timer(data_seq, DATA_TIMER);
	}
	else {
		if (kind == FRAME_ACK) {
			f.ack = ack_seq;//ack_seq 表示此ack的序号
			dbg_frame("Send ACK %d\n", f.ack);
			put_frame((unsigned char *)&f, 2);
		}
		else {
			no_nak = false;
			f.ack = ack_seq;//
			dbg_frame("Send NAK %d\n ", f.ack);
			put_frame((unsigned char *)&f, 2);
		}
	}
	stop_ack_timer();
}

static bool between(unsigned char begin, unsigned char middle, unsigned char end) {
	return ((begin <= middle && middle < end) || (end < begin&&middle >= begin) || (middle < end&&end < begin));
}



int main(int argc, char **argv)
{
	protocol_init(argc, argv);
	lprintf("Designed by G, build: " __DATE__"  "__TIME__"\n");
	typedef unsigned char num;
	num sender_begin, sender_end, rev_begin, rev_end;
	num buf_send = 0;//已占用的发送缓冲区数量
	sender_begin = sender_end = 0;
	rev_begin = 0;
	rev_end = BUF_LEN;
	unsigned char send_buf[PKT_LEN * BUF_LEN];
	unsigned char rev_buf[PKT_LEN * BUF_LEN];
	bool arrived[BUF_LEN];
	for (int i = 0; i < BUF_LEN; ++i) {
		arrived[i] = false;
	}
	int event;
	num	id;
	frame f;
	int len;
	disable_network_layer();
	while (1) {
		event = wait_for_event(&id);
		switch (event) {
		case NETWORK_LAYER_READY:
			get_packet(send_buf + (sender_end % BUF_LEN)*PKT_LEN);
			buf_send++;
			send_kind_frame(FRAME_DATA, (rev_begin + MAX_SEQ) % MAX_S, sender_end, send_buf + (sender_end%BUF_LEN)*PKT_LEN);
			sender_end = (sender_end + 1) % MAX_S;
			break;
		case PHYSICAL_LAYER_READY:
			phl_ready = 1;
			break;
		case FRAME_RECEIVED:
			len = recv_frame((unsigned char*)&f, sizeof(f));
			if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
				dbg_event("**** Receiver Error, Bad CRC Checksum\n");
				if (no_nak)
					send_kind_frame(FRAME_NAK, (rev_begin + MAX_SEQ) % MAX_S, rev_begin, NULL);
				break;
			}

			if (f.kind == FRAME_ACK) {
				dbg_event("Recv ACK  %d\n", f.ack);
				while (between(sender_begin, f.ack, sender_end)) {
					stop_timer(sender_begin);
					sender_begin = (sender_begin + 1) % MAX_S;
					buf_send--;
				}
				break;
			}

			if (f.kind == FRAME_NAK) {
				dbg_event("receive nak\n");
				if(between(sender_begin,(f.ack + 1)%MAX_S,sender_end))
				send_kind_frame(FRAME_DATA, (rev_begin + MAX_SEQ) % MAX_S, (f.ack + 1)%MAX_S, send_buf + ((f.ack + 1)%BUF_LEN)*PKT_LEN);
				while (between(sender_begin, f.ack, sender_end)) {
					stop_timer(sender_begin);
					sender_begin = (sender_begin + 1) % MAX_S;
					buf_send--;
				}
				dbg_event("Recv ACK  %d\n", f.ack);
				break;
			}

			if (f.kind == FRAME_DATA) {
				if (between(rev_begin, f.seq, rev_end) && !arrived[f.seq%BUF_LEN]) {
					dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
					if (f.seq != rev_begin && no_nak)
						send_kind_frame(FRAME_NAK, (rev_begin + MAX_SEQ) % MAX_S, rev_begin, NULL);
					arrived[f.seq % BUF_LEN] = true;
					memcpy(rev_buf + (f.seq % BUF_LEN) * PKT_LEN, f.data, PKT_LEN);			
					while (arrived[rev_begin % BUF_LEN]) {
						put_packet(rev_buf+(rev_begin % BUF_LEN) * PKT_LEN, PKT_LEN);
						arrived[rev_begin % BUF_LEN] = false;
						rev_begin = (rev_begin + 1) % MAX_S;
						rev_end = (rev_end + 1) % MAX_S;
						no_nak = true;
						//stop_ack_timer(); 
						start_ack_timer(ACK_TIMER);
					}
				}
				while (between(sender_begin, f.ack, sender_end)) {
					stop_timer(sender_begin);
					sender_begin = (sender_begin + 1) % MAX_S;
					buf_send--;
				}
				dbg_frame("Recv ACK  %d\n", f.ack);
				break;
			}
		case DATA_TIMEOUT:
			dbg_event("---- DATA %d timeout\n", id);
			send_kind_frame(FRAME_DATA, (rev_begin + MAX_SEQ) % MAX_S, id, send_buf + (id%BUF_LEN)*PKT_LEN);
			break;

		case ACK_TIMEOUT:
			dbg_event("---- ACK timeout\n");
			send_kind_frame(FRAME_ACK, (rev_begin + MAX_SEQ) % MAX_S, 0, NULL);
			break;
		}
		if (buf_send < BUF_LEN && phl_ready)
			enable_network_layer();
		else
			disable_network_layer();
	}

}



















