#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "udp.h"

#define MAX_PACKET_SIZE 1472 // max size for payload MTU - udp header
#define MAX_TIMEOUT 10 * 1000 * 1000 // 10 secs in microseconds
#define MAX_WINDOW_SIZE 64 // go back n, sliding window
#define MAX_RTT 80 * 1000 // 80 ms in microsecs

#define EOF_SEQ_NUM -5

typedef unsigned long long int ull64_t; 

/*** Sequence Utility Functions ***/
typedef short seq_t;
#define MAX_SEQ 256

seq_t safe_increment(seq_t current) {
	current = (current + 1) % MAX_SEQ;
	return current;
}

seq_t safe_subtract(seq_t a, seq_t b) {
	seq_t diff = (a - b) % MAX_SEQ;

	if (diff < 0) {
		diff += MAX_SEQ;
	}

	return diff;
}

int has_wrapped(seq_t next, seq_t curr) {
	return (next - curr) < 0;
}

/*** File Functions ***/

typedef struct file {
	FILE *fp;
} file_t;

file_t* file_create(char *filename) {
	FILE *fp = fopen(filename, "r"); // input files are read only
	if (fp == NULL) {
		perror("file_create");
		exit(1);
	}

	file_t *file = malloc(sizeof(file_t));
	file->fp = fp;

	return file;
}

ull64_t file_get_size(file_t *file) {

	fseek(file->fp, 0L, SEEK_END);
	long size = ftell(file->fp);
	fseek(file->fp, 0L, SEEK_SET);
	if (size < 0) {
		perror("file_get_size");
		return 0;
	}

	return size;
}

ull64_t file_get_position(file_t *file) {
	long position = ftell(file->fp);
	if (position < 0) {
		perror("file_get_position");
		return 0;
	}

	return position;
}

ull64_t file_read(file_t *file, char *buffer, size_t buffer_size) {
	size_t item_size = 1; // 1 byte
	size_t item_count = buffer_size;

	ssize_t bytes_read = fread(buffer, item_size, item_count, file->fp);
	// printf("reading file at location %ld\n", ftell(file->fp));
	if (bytes_read < 0) {
		perror("file_read");
		return 0;
	}

	// if (bytes_read == 0) {
	// 	printf("file_read: end of file\n");
	// }

	// if (bytes_read != buffer_size) {
	// 	printf("file_read: read %lu, less than what was requested %lu\n", bytes_read, buffer_size);
	// }

	return bytes_read;
}

void file_delete(file_t *file) {
	if (file != NULL) {
		fclose(file->fp);
		free(file);
	}
}

void file_moveto(file_t *file, ull64_t pos) {
	FILE *fp = file->fp;
	int fseek_result = fseek(fp ,pos, SEEK_SET);
	if (fseek_result == -1) {
		perror("file_moveto");
	}
}

void file_moveby(file_t *file, ull64_t offset) {
	FILE *fp = file->fp;
	int fseek_result = fseek(fp ,offset, SEEK_CUR);
	if (fseek_result == -1) {
		perror("file_moveby");
	}
}

/*** Packet Header ***/
typedef struct sender_packet_header {
	seq_t seq_num;
	struct timeval timestamp;
} sender_packet_header_t;

typedef struct recvr_packet_header {
	seq_t expected_seq_num;
	struct timeval timestamp;
	ull64_t window;
} recvr_packet_header_t;

const ull64_t max_file_chunk_size = MAX_PACKET_SIZE - sizeof(sender_packet_header_t);

void sender_packet_header_load(sender_packet_header_t* packet_header, seq_t seq_num) {
	packet_header->seq_num = seq_num;
	
	int result = gettimeofday(&packet_header->timestamp, NULL);
	if (result == -1) {
		perror("packet_header_load");
	}
}

/*** Sender Functions ***/

typedef struct sender {
	udp_t *udp;
	file_t *file;
	
	ull64_t transfer_size;
	
	seq_t start_seq_num; // current sequence number
	seq_t end_seq_num;
	ull64_t start_file_pos;

	seq_t last_ack;
	ull64_t recvr_window;

	int window_size; // max amount of packets sent
	int optimal_window_size;

	int packets_sent; // actual amount of packets sent
	int packets_recv;

	long rtt_est; // estimated round trip time
	long rtt_dev;

	int cycle_count; // purely for debugging, 1 cycle = after each recv
} sender_t;

sender_t* sender_create(udp_t* udp, file_t* file, ull64_t transfer_size) {
	sender_t *sender = malloc(sizeof(sender_t));
	sender->udp = udp;
	sender->file = file;

	sender->transfer_size = transfer_size;
	
	// // if file is smaller than requested, just sends the file
	// if (file_get_size(file) < transfer_size) {
	// 	// printf("sender_create: adjusted transfer size\n");
	// 	sender->transfer_size = file_get_size(file);
	// }

	// TODO: random initial seqeunce number
	sender->start_seq_num = 0;
	sender->end_seq_num = 0;
	sender->start_file_pos = file_get_position(file);

	sender->last_ack = -1;
	sender->recvr_window = 0x0; // bit mask of recvrs window
	
	// implementing TCP slow start/congestion control
	sender->optimal_window_size = MAX_WINDOW_SIZE;
	sender->window_size = 1;

	sender->packets_sent = 0;
	sender->packets_recv = 0; // for debugging only

	sender->cycle_count = 0;

	// jacobsen algorithm
	sender->rtt_est = 1000 * 1000; //predicted rtt 30ms in microseconds
	sender->rtt_dev = 200; // predicted deviation for rtt
	return sender;
}

int is_transferred(ull64_t window, int offset) {
	return (window >> offset) & 1;
}

// gets the bytes left based on current file position
ull64_t get_bytes_left(sender_t *sender) {
	file_t *file = sender->file;
	ull64_t at_byte = file_get_position(file);
	ull64_t num_bytes = sender->transfer_size;
	
	// printf("get_bytes_left: at byte %llu out of %llu\n", at_byte, num_bytes);
	if (at_byte > num_bytes) {
		return 0;
	}

	return num_bytes - at_byte;
}

ull64_t send_chunk(sender_t *sender, seq_t seq_num) {
	// precondition: File should already be pointing at the chunk to send
	// must call moveto or moveby, otherwise sends at current position
	udp_t *udp = sender->udp;
	file_t *file = sender->file;

	char *msg = udp->msg_send;
	// prepare packet: load packet header
	sender_packet_header_t packet_header;
	sender_packet_header_load(&packet_header, seq_num);

	size_t packet_header_size = sizeof(sender_packet_header_t);
	memcpy(msg, &packet_header, packet_header_size);

	// prepare packet: load file chunk
	char *data_start = msg + packet_header_size;
	ull64_t bytes_left = get_bytes_left(sender); // !!! Must call before file_read
	ull64_t file_data_size = file_read(file, data_start, max_file_chunk_size);
	// printf("send chunk: file_data_size: %llu, bytes_left: %llu\n", file_data_size, bytes_left);

	int shouldTruncateFileData = max_file_chunk_size > bytes_left;
	if (shouldTruncateFileData) {
		// ex. size = 1000, file pos = 1004, 1000 - 1004 = 4 bytes, current file size is 4 btyes over 
		//long overflow_amount = get_bytes_left(sender);
		file_data_size = bytes_left;
		// printf("send_chunk: truncating\n");
	}

	// send packet
	udp->bytes_to_send = packet_header_size + file_data_size;
	udp_send(udp);

	// printf("send_chunk: sending seq num %d \n", seq_num);
	return file_data_size;
}

void sender_send_data(sender_t *sender) {
	udp_t* udp = sender->udp;
	file_t* file = sender->file;
	
	seq_t seq_num = sender->start_seq_num;

	int packets_sent = 0;
	int max_packets_to_send = sender->window_size;

	for (int i = 0; i < max_packets_to_send; i ++) {
		if (get_bytes_left(sender) == 0) {
			// printf("sender_send_data: no more bytes need to be sent\n");
			break;
		}

		// // move file to next chunk if recvr has this chunk (selective n sliding window)
		if (is_transferred(sender->recvr_window, i)) {
			// printf("sender_send_data: skip seq num %d\n", seq_num);
			file_moveby(file, max_file_chunk_size);
			seq_num = safe_increment(seq_num);
			continue;
		}

		send_chunk(sender, seq_num);
		seq_num = safe_increment(seq_num);
		packets_sent += 1;
	}
	
	sender->end_seq_num = seq_num;
	sender->packets_sent = packets_sent;
}

// source: https://www.gnu.org/software/libc/manual/html_node/Elapsed-Time.html
int timeval_subtract (struct timeval *result, struct timeval *x, struct timeval *y) {
  /* Perform the carry for the later subtraction by updating y. */
  if (x->tv_usec < y->tv_usec) {
    int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
    y->tv_usec -= 1000000 * nsec;
    y->tv_sec += nsec;
  }
  if (x->tv_usec - y->tv_usec > 1000000) {
    int nsec = (x->tv_usec - y->tv_usec) / 1000000;
    y->tv_usec += 1000000 * nsec;
    y->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_usec is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_usec = x->tv_usec - y->tv_usec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}

void update_rtt(sender_t* sender, recvr_packet_header_t *header) {
	struct timeval now;

	int result = gettimeofday(&now, NULL);
	if (result == -1) {
		perror("update_rtt");
	}

	struct timeval rtt;
	struct timeval before = header->timestamp;
	int sign = timeval_subtract(&rtt, &now, &before);
	if (sign == 1) {
		fprintf(stderr, "update_rtt: negative result\n");
	}
	// assume all RTT are less than 1 sec
	// printf("rtt is %ld secs and %d microsecs\n", rtt.tv_sec, rtt.tv_usec);
	
	// jacobson's algorithm for time out value
	float a = 0.125f;
	float b = 0.25f;
	long rtt_est = sender->rtt_est;
	long rtt_sample = rtt.tv_usec;

	long diff = labs(rtt_sample - rtt_est);
	long rtt_dev = sender->rtt_dev;

	rtt_est = (a * rtt_est) + ((1 - a) * rtt_sample);
	rtt_dev = (b * rtt_dev) + ((1 - b) * diff);
	
	sender->rtt_est = rtt_est;
	sender->rtt_dev = rtt_dev;
	// printf("update rtt: new avg is %ld\n", rtt_est);
}

void increase_rtt_timeout(sender_t *sender) {
	sender->rtt_est = sender->rtt_est * 2;
	if (sender->rtt_est > MAX_TIMEOUT) {
		fprintf(stderr, "increase_rtt_timeout: timed out\n");
		exit(1);
	}
}

void fast_retransmit(sender_t *sender, recvr_packet_header_t *header) {
	// fast retransmit
	// setting file position for the chunk to be retransmitted
	seq_t start_seq_num = sender->start_seq_num;
	seq_t seq_num_to_retranmist = header->expected_seq_num;

	seq_t diff = safe_subtract(seq_num_to_retranmist, start_seq_num);
	
	ull64_t offset = diff * max_file_chunk_size;
	ull64_t file_position = sender->start_file_pos + offset;

	file_t *file = sender->file;
	file_moveto(file, file_position);

	send_chunk(sender, seq_num_to_retranmist);
	// printf("fast_retransmit: seq_num %d\n", seq_num_to_retranmist);
}

void cc_fast_recovery(sender_t *sender) {
	int window_size = sender->window_size / 2;
	if (window_size == 0) {
		window_size = 1;
	}
	sender->optimal_window_size = window_size;
	sender->window_size = window_size;
	// printf("cc_fast_recovery: window size: %d, optimal window size: %d\n", sender->window_size, sender->optimal_window_size);
}

// cogestion control exponential increase 
void cc_incr(sender_t *sender) {
	int window_size = sender->window_size;
	int optimal_window_size = sender->optimal_window_size;

	// additive incr: //additive incr if over congestion threshold (optimal window size)
	if (window_size >=  optimal_window_size) {
		window_size += 1;
	}
	// exp incr if under congestion threshold (optimal window size)
	else {
		window_size *= 2;
		// max window size can be is the optimal window size, when doing exponential increase
		// if (window_size > optimal_window_size) {
		// 	window_size = optimal_window_size;
		// }
	}

	// don't increase past max window size
	if (window_size > MAX_WINDOW_SIZE) {
		window_size = MAX_WINDOW_SIZE; 
	}

	sender->window_size = window_size;
	// printf("cc_incr: window size: %d, optimal window size: %d\n", sender->window_size, sender->optimal_window_size);
}

// TCP slow start 
void cc_slow_start(sender_t *sender) {
	// start window size at 1
	
	// half congestion window size threshold (optimal_window_size)
	int optimal_window_size = sender->optimal_window_size / 2;
	
	if (optimal_window_size == 0) {
		optimal_window_size = 1; // don't let it get to 0
	}

	sender->window_size = 1;
	sender->optimal_window_size = optimal_window_size;
	// printf("cc_slow_start: window size: %d, optimal window size: %d\n", sender->window_size, sender->optimal_window_size);
}

void update_last_ack(sender_t* sender, recvr_packet_header_t *header) {
	seq_t prev_ack = sender->last_ack;
	seq_t next_ack = header->expected_seq_num;
	//seq_t start_seq = sender->start_seq_num;
	//offset from the start_seq
	// printf("update_last_ack: prev ack: %d, next ack: %d\n", prev_ack, next_ack);
	
	int window_size = sender->window_size;
	int diff = safe_subtract(next_ack, prev_ack);
	
	// if (diff >= MAX_WINDOW_SIZE && prev_ack != -1) {
	// 	// printf("update_last_ack: out of window, diff %d\n", diff);
	// 	// fprintf(stderr, "update_last_ack: out of window\n");
	// 	return; // this ack is out of the window/not most up to date
	// 	// TODO: maybe update recvr_window...?
	// }

	sender->last_ack = next_ack;
	sender->recvr_window = header->window;
	// printf("last ack is now: %d\n", sender->last_ack);
}

void sender_recv_acks(sender_t *sender) {
	udp_t *udp = sender->udp;

	int max_packets_to_recv = sender->packets_sent;
	int packets_recv = 0;
	int should_slow_start = 0;
	int should_recover_fast = 0;
	int dup_count = 0;
	// reads incoming acks, up to the amount sent
	for (int i = 0; i < max_packets_to_recv; i ++) {
		udp_recv(udp);

		// if there's a timeout
		if (udp->bytes_recv == -1) {
			//perror("sender_recv_acks");
			// int packets_lost = max_packets_to_recv - packets_recv;
			// printf("sender_recv_acks: %d packets lost\n", packets_lost);
			should_slow_start = 1;
			break;
		}

		packets_recv += 1;

		char *msg = udp->msg_recv;
		recvr_packet_header_t header;
		memcpy(&header, msg, sizeof(recvr_packet_header_t));

		seq_t prev_ack = sender->last_ack;
		seq_t next_ack = header.expected_seq_num;
		if (prev_ack == next_ack) {
			// most likely a dropped packet
			// should recv data/packet
			// printf("sender_recv_acks: duplicate ack (either out of order or dropped)\n");	
			
			dup_count += 1;
			if (dup_count == 2) {
				fast_retransmit(sender, &header);
				should_recover_fast = 1;
			}
			// max_packets_to_recv += 1;
			// should_slow_start = 1;
		} else {
			dup_count = 0;
		}
		update_last_ack(sender, &header);
		update_rtt(sender, &header);
		// if (next_ack == sender->end_seq_num) {
		// 	break;
		// }
	}

	
	if (should_slow_start) {
		cc_slow_start(sender);
		// increase_rtt_timeout(sender);
	} else if (should_recover_fast) {
		cc_fast_recovery(sender);
	} else {
		cc_incr(sender);
	}

	sender->packets_recv = packets_recv;
}

void sender_set_timeout(sender_t *sender) {
	// jacobson's algorithm for time out value
	suseconds_t microsecs = (4 * sender->rtt_dev) + sender->rtt_est;
	
	// will error out if microseconds > 1 sec, must split into seconds and microseconds
	long microsecs_in_sec = 1000*1000;

	time_t secs = microsecs / microsecs_in_sec;
	suseconds_t adjusted_microsecs = microsecs % microsecs_in_sec; 

	// sets the timeout
	struct timeval tv;
	tv.tv_sec = secs;
	tv.tv_usec = adjusted_microsecs;

	// printf("sender_set_timeout: timeout set to: %ld s , %d ms\n", tv.tv_sec, tv.tv_usec / 1000);

	if (setsockopt(sender->udp->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv,sizeof(tv)) < 0) {
		perror("sender_set_timeout: setting timeout failed");
	}
}

int sender_is_complete(sender_t *sender) {
	return sender->start_file_pos >= sender->transfer_size;
}

void sender_send_eof(sender_t *sender) {
	udp_t *udp = sender->udp;
	char *msg = udp->msg_send;

	//prepare packet: load eof seq num
	seq_t seq_num = EOF_SEQ_NUM;
	sender_packet_header_t packet_header;
	sender_packet_header_load(&packet_header, seq_num);
	
	memcpy(msg, &packet_header, sizeof(sender_packet_header_t));

	udp->bytes_to_send = sizeof(sender_packet_header_t);
	udp_send(udp);
	udp_send(udp);
	udp_send(udp);
	udp_send(udp);
}

void sender_reset(sender_t *sender) {
	seq_t start_seq_num = sender->start_seq_num;
	seq_t next_seq_num = sender->last_ack;
	if (next_seq_num == -1) {
		next_seq_num = 0;
	}
	seq_t diff = safe_subtract(next_seq_num, start_seq_num);

	ull64_t offset = diff * max_file_chunk_size;
	ull64_t next_file_position = sender->start_file_pos + offset;
	
	file_t *file = sender->file;
	file_moveto(file, next_file_position);

	sender->start_file_pos = file_get_position(file);
	
	sender->start_seq_num = next_seq_num;
	// printf("sender_reset: start seq num: %d, file pos: %llu\n", sender->start_seq_num, sender->start_file_pos);
	// if (has_wrapped(next_seq_num, start_seq_num)) {
	
	// 	printf("Begin Round %d\n", sender->cycle_count);
	// 	printf("File Position %llu\n", file_get_position(sender->file));

	// 	ull64_t bytes_left = get_bytes_left(sender);
	// 	float mB_left = bytes_left / (1000.0f * 1000.0f);
	// 	printf("sender_send_data: %0.6f mB left\n\n", mB_left);
	// 	sender->cycle_count += 1;
	// }
}

void sender_delete(sender_t *sender) {
	free(sender);
}

/*** Main Loop ***/

void transfer(sender_t *sender) {
	// initial timeouts based on predicitions
	sender_set_timeout(sender);
	// printf("max chunk size is %llu\n", max_file_chunk_size);
	// printf("\n\n");
	while (!sender_is_complete(sender)) {
		sender_send_data(sender);

		sender_recv_acks(sender);

		sender_set_timeout(sender);

		sender_reset(sender);
		// printf("\n");
	}

	sender_send_eof(sender);
	// printf("file transfer complete\n");
}

int main(int argc, char** argv) {

	if(argc != 5) {
		fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
		exit(1);
	}
	// parsing args
	int port = atoi(argv[2]);
	ull64_t transfer_size = atoll(argv[4]);
	char *address = argv[1];
	char *filename = argv[3];


	// udp
	char *udp_port = "0"; // 0 for any open port
	size_t recv_buffer_size = MAX_PACKET_SIZE;
	size_t send_buffer_size = MAX_PACKET_SIZE;

	udp_t *udp = udp_create(udp_port, send_buffer_size, recv_buffer_size);
	udp_set_server_addr(udp, address, port); 

	file_t *file = file_create(filename);

	sender_t *sender = sender_create(udp, file, transfer_size);

	// main loop
	transfer(sender);

	// clean up
	file_delete(file);
	udp_delete(udp);
	sender_delete(sender);

	return 0;
}
