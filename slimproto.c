/* 
 *  Squeezelite - lightweight headless squeezeplay emulator for linux
 *
 *  (c) Adrian Smith 2012, triode1@btinternet.com
 *  
 *  Unreleased - license details to be added here...
 */

#include "squeezelite.h"
#include "slimproto.h"

static log_level loglevel;

#define PORT 3483
#define MAXBUF 4096

static int sock;
static in_addr_t slimproto_ip;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;

extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;

extern struct codec codecs[];

#define LOCK_S   pthread_mutex_lock(&streambuf->mutex)
#define UNLOCK_S pthread_mutex_unlock(&streambuf->mutex)
#define LOCK_O   pthread_mutex_lock(&outputbuf->mutex)
#define UNLOCK_O pthread_mutex_unlock(&outputbuf->mutex)

static struct {
	u32_t jiffies;
	u32_t stream_full;
	u32_t stream_size;
	u64_t stream_bytes;
	u32_t output_full;
	u32_t output_size;
	u32_t frames_played;
	u32_t current_sample_rate;
	u32_t last;
	stream_state stream_state;
} status;

int autostart;

void send_packet(u8_t *packet, size_t len) {
	u8_t *ptr = packet;
	size_t n;
	
	while (len) {
		n = send(sock, ptr, len, 0);
		if (n < 0) {
			LOG_WARN("failed writing to socket: %s", strerror(errno));
			return;
		}
		ptr += n;
		len -= n;
	}
}

void hexdump(u8_t *pack, int len) {
	char buf1[1024];
	char buf2[1024];
	char *ptr1 = buf1;
	char *ptr2 = buf2;
	len = min(1024/3 - 1, len);

	while (len--) {
		sprintf(ptr1, "%02X ", *pack);
		sprintf(ptr2, "%c  ", *pack > 32 ? *pack : ' ');
		ptr1 += 3;
		ptr2 += 3;
		pack++;
	}
	LOG_INFO("hex: %s", buf1);
	LOG_INFO("str: %s", buf2);
}

inline void packN(u32_t *dest, u32_t val) {
	u8_t *ptr = (u8_t *)dest;
	*(ptr)   = (val >> 24) & 0xFF; *(ptr+1) = (val >> 16) & 0xFF; *(ptr+2) = (val >> 8) & 0xFF;	*(ptr+3) = val & 0xFF;
}

inline void packn(u16_t *dest, u16_t val) {
	u8_t *ptr = (u8_t *)dest;
	*(ptr) = (val >> 8) & 0xFF; *(ptr+1) = val & 0xFF;
}

inline u32_t unpackN(u32_t *src) {
	u8_t *ptr = (u8_t *)src;
	return *(ptr) << 24 | *(ptr+1) << 16 | *(ptr+2) << 8 | *(ptr+3);
} 

inline u16_t unpackn(u16_t *src) {
	u8_t *ptr = (u8_t *)src;
	return *(ptr) << 8 | *(ptr+1);
} 

static void sendHELO(bool reconnect, const char *cap) {
	const char *capbase = "Model=squeezelite,ModelName=SqueezeLite,";

	struct HELO_packet pkt = {
		.opcode = "HELO",
		.length = htonl(sizeof(struct HELO_packet) - 8 + strlen(capbase) + strlen(cap)),
		.deviceid = 12, // squeezeplay
		.revision = 0, 
		.lang   = "EN", // FIXME - is this right?
	};
	packn(&pkt.wlan_channellist, reconnect ? 0x4000 : 0x0000);
	packN(&pkt.bytes_received_H, (u64_t)status.stream_bytes >> 32);
	packN(&pkt.bytes_received_L, (u64_t)status.stream_bytes & 0xffffffff);

	send_packet((u8_t *)&pkt, sizeof(pkt));
	send_packet((u8_t *)capbase, strlen(capbase));
	send_packet((u8_t *)cap, strlen(cap));
}

static void sendSTAT(const char *event, u32_t server_timestamp) {
	assert(strlen(event) == 4);
	struct STAT_packet pkt;

	memset(&pkt, 0, sizeof(struct STAT_packet));
	memcpy(&pkt.opcode, "STAT", 4);
	pkt.length = htonl(sizeof(struct STAT_packet) - 8);
	memcpy(&pkt.event, event, 4);
	// num_crlf
	// mas_initialized; mas_mode;
	packN(&pkt.stream_buffer_fullness, status.stream_full);
	packN(&pkt.stream_buffer_size, status.stream_size);
	packN(&pkt.bytes_received_H, (u64_t)status.stream_bytes >> 32);
	packN(&pkt.bytes_received_L, (u64_t)status.stream_bytes & 0xffffffff);
	pkt.signal_strength = 0xffff;
	packN(&pkt.jiffies, status.jiffies);
	packN(&pkt.output_buffer_size, status.output_size);
	packN(&pkt.output_buffer_fullness, status.output_full);
	packN(&pkt.elapsed_seconds, status.current_sample_rate ? status.frames_played / status.current_sample_rate : 0);
	// voltage;
	packN(&pkt.elapsed_milliseconds, 
		  status.current_sample_rate ? (u32_t)((u64_t)status.frames_played * (u64_t)1000 / (u64_t)status.current_sample_rate) : 0);
	pkt.server_timestamp = server_timestamp; // keep this is server format - don't unpack/pack
	// error_code;

	LOG_INFO("STAT: %s", event);

	send_packet((u8_t *)&pkt, sizeof(pkt));
}

static void sendDSCO(disconnect_code disconnect) {
	struct DSCO_packet pkt;

	memset(&pkt, 0, sizeof(pkt));
	memcpy(&pkt.opcode, "DSCO", 4);
	pkt.length = htonl(sizeof(pkt) - 8);
	pkt.reason = disconnect & 0xFF;

	LOG_INFO("DSCO: %d", disconnect);

	send_packet((u8_t *)&pkt, sizeof(pkt));
}

static void sendRESP(const char *header, size_t len) {
	struct RESP_header pkt_header;

	memset(&pkt_header, 0, sizeof(pkt_header));
	memcpy(&pkt_header.opcode, "RESP", 4);
	pkt_header.length = htonl(sizeof(pkt_header) + len - 8);

	LOG_INFO("RESP");

	send_packet((u8_t *)&pkt_header, sizeof(pkt_header));
	send_packet((u8_t *)header, len);
}

static void process_strm(u8_t *pkt, int len) {
	struct strm_packet *strm = (struct strm_packet *)pkt;

	LOG_INFO("strm command %c", strm->command);

	switch(strm->command) {
	case 't':
		sendSTAT("STMt", strm->replay_gain); // STMt replay_gain is no longer used to track latency, but support it
		break;
	case 'q':
		stream_disconnect();
		buf_flush(streambuf);
		buf_flush(outputbuf);
		break;
	case 'f':
		stream_disconnect();
		buf_flush(streambuf);
		buf_flush(outputbuf);
		break;
		// FIXME - should output buf be flushed here?
	case 'p':
		LOCK_O;
		output.state = OUTPUT_STOPPED;
		UNLOCK_O;
		break;
		// FIXME - implement jiffies delay?
	case 'a':
		// FIXME - implement ahead
		break;
	case 'u':
		LOCK_O;
		output.state = OUTPUT_RUNNING;
		decode.state = DECODE_RUNNING;
		UNLOCK_O;
		sendSTAT("STMr", 0);
		break;
		// FIXME - implement jiffies delay?
	case 's':
		LOG_INFO("strm s autostart: %c", strm->autostart);

		unsigned header_len = len - sizeof(struct strm_packet);
		char *header = (char *)(pkt + sizeof(struct strm_packet));
		in_addr_t ip = strm->server_ip; // keep in network byte order
		u16_t port = strm->server_port; // keep in network byte order
		if (ip == 0) ip = slimproto_ip; 

		LOCK_O;
		output.state = OUTPUT_RUNNING;
		UNLOCK_O;
		sendSTAT("STMf", 0);
		codec_open(strm->format, strm->pcm_sample_size, strm->pcm_sample_rate, strm->pcm_channels, strm->pcm_endianness);
		stream_sock(ip, port, header, header_len);
		status.jiffies = gettime_ms();
		sendSTAT("STMc", 0);
		autostart = strm->autostart - '0';
		break;
	default:
		LOG_INFO("************** Unhandled strm command %c", strm->command);
		break;
	}
}

static void process_cont(u8_t *pkt, int len) {
	// ignore any params from cont as we don't yet suport icy meta
	if (autostart > 1) {
		autostart -= 2;
	}
}

static void process_audg(u8_t *pkt, int len) {
	struct audg_packet *audg = (struct audg_packet *)pkt;
	audg->gainL = unpackN(&audg->gainL);
	audg->gainR = unpackN(&audg->gainR);

	LOG_INFO("audg gainL: %u gainR: %u fixed: %u", audg->gainL, audg->gainR);

	LOCK_O;
	output.gainL = audg->gainL;
	output.gainR = audg->gainR;
	UNLOCK_O;
}

struct handler {
	char opcode[5];
	void (*handler)(u8_t *, int);
};

static struct handler handlers[] = {
	{ "strm", process_strm },
	{ "cont", process_cont },
	// aude - ignore for the moment - enable/disable audio output from S:P:Squeezebox2
	{ "audg", process_audg },
	{ "",     NULL  },
};

static void process(u8_t *pack, int len) {
	struct handler *h = handlers;
	while (h->handler && strncmp((char *)pack, h->opcode, 4)) { h++; }

	if (h->handler) {
		LOG_INFO("%s", h->opcode);
		h->handler(pack, len);
	} else {
		pack[4] = '\0';
		LOG_INFO("unhandled %s", (char *)pack);
	}
}

static void slimproto_run() {
	struct pollfd pollinfo = { .fd = sock, .events = POLLIN };
	static u8_t buffer[MAXBUF];
	int  expect = 0;
	int  got    = 0;

	while (true) {

		// timeout of 100ms to ensure the playback state machine is run at this frequency
		if (poll(&pollinfo, 1, 100)) {

			if (pollinfo.revents) {

				if (expect > 0) {
					int n = recv(sock, buffer + got, expect, 0);
					if (n <= 0) {
						LOG_WARN("error reading from socket: %s", n ? strerror(errno) : "closed");
						break;
					}
					expect -= n;
					got += n;
					if (expect == 0) {
						process(buffer, got);
						got = 0;
					}
				} else if (expect == 0) {
					int n = recv(sock, buffer + got, 2 - got, 0);
					if (n <= 0) {
						LOG_WARN("error reading from socket: %s", n ? strerror(errno) : "closed");
						break;
					}
					got += n;
					if (got == 2) {
						expect = buffer[0] << 8 | buffer[1]; // length pack 'n'
						got = 0;
						if (expect > MAXBUF) {
							LOG_ERROR("FATAL: slimproto packet too big: %d > %d", expect, MAXBUF);
							break;
						}
					}
				} else {
					LOG_ERROR("FATAL: negative expect");
					break;
				}

			}
		}

		// update playback state every 100ms
		u32_t now  = gettime_ms();

		if (now - status.jiffies > 100) {

			status.jiffies = now;

			bool _sendSTMs = false;
			bool _sendDSCO = false;
			bool _sendRESP = false;
			bool _sendSTMd = false;
			bool _sendSTMt = false;
			bool _sendSTMl = false;
			bool _sendSTMu = false;
			bool _sendSTMo = false;
			disconnect_code disconnect;
			static char header[MAX_HEADER];
			size_t header_len;

			LOCK_S;
			status.stream_full = _buf_used(streambuf);
			status.stream_size = streambuf->size;
			status.stream_bytes = stream.bytes;
			status.stream_state = stream.state;
						
			if (stream.state == DISCONNECT) {
				disconnect = stream.disconnect;
				stream.state = STOPPED;
				_sendDSCO = true;
			}
			if (stream.state == STREAMING_HTTP && !stream.sent_headers) {
				header_len = stream.header_len;
				memcpy(header, stream.header, header_len);
				_sendRESP = true;
				stream.sent_headers = true;
			}
			UNLOCK_S;
			
			LOCK_O;
			status.output_full = _buf_used(outputbuf);
			status.output_size = outputbuf->size;
			status.frames_played = output.frames_played;
			status.current_sample_rate = output.current_sample_rate;
			
			if (output.track_started) {
				_sendSTMs = true;
				output.track_started = false;
			}
			if (decode.state == DECODE_COMPLETE) {
				_sendSTMd = true;
				decode.state = DECODE_STOPPED;
			}
			if (status.stream_state == STREAMING_HTTP && decode.state == DECODE_STOPPED) {
				// FIXME check buffer threshold...
				if (autostart == 0) {
					_sendSTMl = true;
				} else if (autostart == 1) {
					decode.state = DECODE_RUNNING;
				}
				// autostart 2 and 3 require cont to be received first
			}
			if (decode.state == DECODE_RUNNING && now - status.last > 1000) {
				_sendSTMt = true;
				status.last = now;
			}

			// FIXME - need to send STMu or STMo on underrun

			UNLOCK_O;
		
			// send packets once locks released as packet sending can block
			if (_sendDSCO) sendDSCO(disconnect);
			if (_sendSTMs) sendSTAT("STMs", 0);
			if (_sendSTMd) sendSTAT("STMd", 0);
			if (_sendSTMt) sendSTAT("STMt", 0);
			if (_sendSTMl) sendSTAT("STMl", 0);
			if (_sendSTMu) sendSTAT("STMu", 0);
			if (_sendSTMo) sendSTAT("STMo", 0);
			if (_sendRESP) {
				sendRESP(header, header_len);
			}
		}
	}
}

in_addr_t discover_server(void) {
    struct sockaddr_in d;
    struct sockaddr_in s;

	int disc_sock = socket(AF_INET, SOCK_DGRAM, 0);

	int enable = 1;
	setsockopt(disc_sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));

	memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET;
	d.sin_port = htons(PORT);
    d.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	char *buf = "e";

	struct pollfd pollinfo = { .fd = disc_sock, .events = POLLIN };

	do {

		LOG_INFO("sending discovery");
		memset(&s, 0, sizeof(s));

		if (sendto(disc_sock, buf, 1, 0, (struct sockaddr *)&d, sizeof(d)) < 0) {
			LOG_WARN("error sending disovery");
		}

		if (poll(&pollinfo, 1, 5000)) {
			char readbuf[10];
			socklen_t slen = sizeof(s);
			recvfrom(disc_sock, readbuf, 10, 0, (struct sockaddr *)&s, &slen);
			LOG_INFO("got response from: %s:%d", inet_ntoa(s.sin_addr), ntohs(s.sin_port));
		}

	} while (s.sin_addr.s_addr == 0);

	close(disc_sock);

	return s.sin_addr.s_addr;
}

void slimproto(log_level level, const char *addr) {
    struct sockaddr_in serv_addr;
	static char buf[128];
	bool reconnect = false;

	loglevel = level;

	if (addr) {
		slimproto_ip = inet_addr(addr);
	} else {
		slimproto_ip = discover_server();
	}

	LOCK_O;
	sprintf(buf, "MaxSampleRate=%u", output.max_sample_rate); 
	int i = 0;
	while (codecs[i].id && strlen(buf) < 128 - 10) {
		strcat(buf, ",");
		strcat(buf, codecs[i].types);
		i++;
	}
	UNLOCK_O;

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = slimproto_ip;
	serv_addr.sin_port = htons(PORT);

	LOG_INFO("connecting to %s:%d", inet_ntoa(serv_addr.sin_addr), ntohs(serv_addr.sin_port));
	LOG_DEBUG("cap: %s", buf);

	while (true) {

		sock = socket(AF_INET, SOCK_STREAM, 0);

		if (connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {

			LOG_INFO("unable to connect to server");
			sleep(5);

		} else {
		
			LOG_INFO("connected");

			sendHELO(reconnect, buf);
			reconnect = true;

			slimproto_run();
		}

		close(sock);
	}
}