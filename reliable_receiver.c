#include <stdio.h>
#include <stdlib.h>

#include "udp.h"

#define MAX_PACKET_SIZE 1472 // max size for payload MTU - udp header
#define MAX_WINDOW_SIZE 64 // 64 frames (1/2 max seq), go back n - sliding window
#define MAX_TIMEOUT 10 // seconds for client losing connection

#define EOF_SEQ_NUM -5 // random negative value
#define TIMED_OUT 1
#define TRANSFER_COMPLETE 2
#define TRANSFER_IN_PROGRESS 3

/*** Sequence Utility Functions ***/
typedef short seq_t;
#define MAX_SEQ 256
typedef unsigned long long int ull64_t;

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

seq_t safe_add(int current, int amount) {
	current = (current + amount) % MAX_SEQ;
	return current;
}

int has_wrapped(seq_t next, seq_t curr) {
	return (next - curr) < 0;
}

/*** Packet Header ***/
typedef struct sender_packet_header {
	seq_t seq_num;
	struct timeval timestamp;
} sender_packet_header_t;

typedef struct recvr_packet_header {
	seq_t next_seq_num;
	struct timeval timestamp;
	ull64_t window;
} recvr_packet_header_t;

const ull64_t max_file_chunk_size = MAX_PACKET_SIZE - sizeof(sender_packet_header_t);

/*** Fwriter Functions ***/

typedef struct fwriter {
	FILE *fp;
} fwriter_t;

fwriter_t* fwriter_create(char *filename) {
	FILE *fp = fopen(filename, "w+"); // input files are read only
	if (fp == NULL) {
		perror("fwriter_create");
		exit(1);
	}

	fwriter_t *fwriter = malloc(sizeof(fwriter_t));
	fwriter->fp = fp;

	return fwriter;
}

void fwriter_offset_write(fwriter_t *fwriter, char *data, size_t data_size, ull64_t offset) {
	FILE *file = fwriter->fp;

	long current_file_position = ftell(file);
	if (current_file_position == -1) {
		perror("fwriter_offset_write: ftell");
	}
	ull64_t offset_file_position = offset * max_file_chunk_size;
	
	int seek_result = fseek(file, offset_file_position, SEEK_CUR);
	if (seek_result == -1) {
		perror("fwriter_offset_write: fseek offset");
	}
	// printf("writing file at location %ld\n", ftell(file));
	size_t bytes_written = fwrite(data, 1, data_size, file);
	if (bytes_written < data_size) {
		perror("fwriter_offset_write: fwrite");
	}

	seek_result = fseek(file, current_file_position, SEEK_SET);
	if (seek_result == -1) {
		perror("fwriter_offset_write: fseek reset");
	}
}

// offset from last seq num of file
void fwriter_set_position(fwriter_t *fwriter, seq_t offset) {
	FILE *file = fwriter->fp;
	// printf("moving file position from: %ld ", ftell(file));
	ull64_t offset_bytes = offset * max_file_chunk_size;
	
	int seek_result = fseek(file, offset_bytes, SEEK_CUR);
	if (seek_result == -1) {
		perror("fwriter_offset_write: fseek offset");
	}
	// printf(" to %ld\n", ftell(file));
}

void fwriter_delete(fwriter_t *fwriter) {
	if (fwriter != NULL) {
		fclose(fwriter->fp);
		free(fwriter);
	}
}

/*** Recvr Functions ***/

typedef struct recvr {
	udp_t *udp;
	fwriter_t* fwriter;

	seq_t next_seq_num; // expected seq_num
	sender_packet_header_t sender_header;

	ull64_t window; // bit mask for receive window

	int cycle_count; // for debugging only
	int client_connected;
} recvr_t;

recvr_t* recvr_create(udp_t *udp, fwriter_t* fwriter) {
	recvr_t *recvr = malloc(sizeof(recvr_t));
	recvr->udp = udp;
	recvr->fwriter = fwriter;
	
	recvr->next_seq_num = 0;
	
	recvr->window = 0x0;

	recvr->client_connected = 0;
	recvr->cycle_count = 0;
	return recvr;
}

int is_eof(recvr_t *recvr) {
	// last packet will have seq num of -1
	seq_t recv_seq_num = recvr->sender_header.seq_num;
	if (recv_seq_num == EOF_SEQ_NUM) {
		// printf("eof found\n");
		return 1;
	}
	return 0;
}

int is_timed_out(recvr_t *recvr) {
	ssize_t bytes_recv = recvr->udp->bytes_recv;
	if (bytes_recv == -1) {
		perror("is_timed_out");
		return 1;
	}
	return 0;
}

void parse_header(recvr_t *recvr) {
	udp_t *udp = recvr->udp;
	char *msg = udp->msg_recv;
	
	memcpy(&recvr->sender_header, msg, sizeof(sender_packet_header_t));

	seq_t seq_num = recvr->sender_header.seq_num;
	// printf("parse_header: got seq num %d, expected %d\n", seq_num, recvr->next_seq_num);
}

int recvr_listen(recvr_t *recvr) {
	udp_t *udp = recvr->udp;

	udp_recv(udp);
	if (is_timed_out(recvr)) {
		return TIMED_OUT;
	}

	parse_header(recvr);
	
	if (is_eof(recvr)) {
		return TRANSFER_COMPLETE;
	}
	return TRANSFER_IN_PROGRESS;
}

int is_window_complete(recvr_t *recvr) {
	ull64_t full_window = 0xFFFFFFFFFFFFFFFF; //0xFFFF FFFF FFFF FFFF -- 64 bits of 1s
	ull64_t current_window = recvr->window;

	if (current_window == full_window) {
		return 1; // window is full
	}

	return 0;
}

void mark_written(recvr_t *recvr, int offset) {
	ull64_t bit_mask = 1 << offset;
	ull64_t current_window = recvr->window;
	ull64_t next_window = current_window | bit_mask;
	recvr->window = next_window;
	// printf("mark_written: bitmask: %llx , window & bitmask: %llx\n", bit, recvr->window);
}

int is_written(recvr_t *recvr, int offset) {
	ull64_t bit = (recvr->window >> offset) & 1;
	return bit;
}

seq_t move_window(recvr_t *recvr) {
	seq_t move_amount = 0;
	for (int i = 0; i < MAX_WINDOW_SIZE; i++) {
		if (!is_written(recvr, i)) {
			break;
		}
		move_amount += 1;
	}
	ull64_t current_window = recvr->window;
	ull64_t next_window = current_window >> move_amount;
	recvr->window = next_window;
	// printf("move_window: a - %016llx\n", recvr->window);
	// printf("move_window: moved by %d\n", move_amount);
	return move_amount;
}

void recvr_save_data(recvr_t *recvr) {
	udp_t *udp = recvr->udp;
	fwriter_t *fwriter = recvr->fwriter;

	seq_t recv_seq_num = recvr->sender_header.seq_num;
	seq_t next_seq_num = recvr->next_seq_num;

	char *data_start = udp->msg_recv + sizeof(sender_packet_header_t);
	size_t data_size = udp->bytes_recv - sizeof(sender_packet_header_t);
	// printf("recvr_save_data: data bytes recv : %zu\n", data_size);

	seq_t offset = safe_subtract(recv_seq_num, next_seq_num);
	if (offset >= MAX_WINDOW_SIZE) {
		// printf("recvr_save_data: recv seq num: %d, next seq num: %d, offset: %d\n", recv_seq_num, next_seq_num, offset);
		// printf("recvr_save_data: out of window - discarding\n");
		// fprintf(stderr, "recvr_save_data: out of window - discarding\n");
		return;
	}

	if (is_written(recvr, offset)) {
		// printf("recvr_save_data: already buffered for seq num %d\n", recv_seq_num);
		return;
	}
	
	if (recv_seq_num == next_seq_num) {
		recvr->window |= 1;
		fwriter_offset_write(fwriter, data_start, data_size, 0);
		seq_t move_amount = move_window(recvr);
		fwriter_set_position(fwriter, move_amount); // set file to position based on window
		next_seq_num = safe_add(next_seq_num, move_amount);
		// if (has_wrapped(next_seq_num, recv_seq_num)) {
		// 	printf("Begin Round %d\n", recvr->cycle_count);
		// 	printf("File Position %lu\n", ftell(fwriter->fp));
		// 	recvr->cycle_count += 1;
		// }
		recvr->next_seq_num = next_seq_num;
	} else {
		fwriter_offset_write(fwriter, data_start, data_size, offset);
		mark_written(recvr, offset);
	}
	
}

void recvr_respond(recvr_t *recvr) {
	udp_t *udp = recvr->udp;
	
	if (!recvr->client_connected) {
		recvr->client_connected = 1;
		// sets it to the client address if not set already
		udp_set_server_addr(udp, NULL, -1);
	}

	// prepares packet header: same timestamp but with expected seq num
	// TODO: increase performance by caching the whole sender_packet_header
	sender_packet_header_t sender_header;
	memcpy(&sender_header, udp->msg_recv, sizeof(sender_packet_header_t));
	
	recvr_packet_header_t recvr_header;
	recvr_header.next_seq_num = recvr->next_seq_num;
	recvr_header.timestamp = sender_header.timestamp; // original timestamp
	recvr_header.window = recvr->window;

	char *msg = udp->msg_send;
	memcpy(msg, &recvr_header, sizeof(recvr_packet_header_t));
	udp->bytes_to_send = sizeof(recvr_packet_header_t);
	udp_send(udp);
	// printf("recvr_respond: sent next seq num: %d\n", recvr->next_seq_num);
}

void recvr_delete(recvr_t *recvr) {
	free(recvr);
}

void receive(recvr_t* recvr) {
	
	// printf("\n\n");
	while(1) {
		int listen_result = recvr_listen(recvr);

		if (listen_result == TIMED_OUT) {
			break;
		}

		if (listen_result == TRANSFER_COMPLETE) {
			break;
		}

		recvr_save_data(recvr);
		recvr_respond(recvr);
		// printf("\n");
	}
	
}

int main(int argc, char** argv)
{	
	if(argc != 3)
	{
		fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
		exit(1);
	}
	
	char *port = argv[1];
	char *filename = argv[2];

	// creating the udp
	size_t recv_buffer_size = MAX_PACKET_SIZE;
	size_t send_buffer_size = MAX_PACKET_SIZE;
	udp_t *udp = udp_create(port, send_buffer_size, recv_buffer_size);

	// set max timeout for recv
	struct timeval tv;
	tv.tv_sec = MAX_TIMEOUT;
	tv.tv_usec = 0;
	
	if (setsockopt(udp->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		perror("main: setting timeout failed");
	}

	fwriter_t *fwriter = fwriter_create(filename);

	recvr_t *recvr = recvr_create(udp, fwriter);

	receive(recvr);

	// clean up
	udp_delete(udp);
	fwriter_delete(fwriter);
	recvr_delete(recvr);
	
	return 0;
}
