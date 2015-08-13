#include <net/ether.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/checksum.h>

#include "ah.h"
#include "socket.h"
#include "spd.h"
#include "sad.h"
#include "ike.h"
#include "mode.h"

bool ipsec_init() {
	if(!socket_init())
		return false;

	return true;
}

static bool ipsec_decrypt(Packet* packet, SA* sa) { 
	Ether* ether = (Ether*)(packet->buffer + packet->start);
        IP* ip = (IP*)ether->payload;
	//int origin_length = endian16(ip->length);

	// 2. Seq# Validation
	ESP* esp = (ESP*)ip->body;
	if(checkWindow(sa->window, esp->seq_num) < 0) {
		printf(" 2. Seq# Validation : Dicard Packet \n");
		return false;
	}

	int size;
	// 3. ICV Validation
	if(sa->iv_mode == true) {
		size = endian16(ip->length) - (ip->ihl * 4) - ICV_LEN; 
		
		if((size + ICV_LEN) > packet->end) {
			printf(" 3. ICV Validation : Discard Packet \n");
			return false;
		}

		uint8_t result[12];
		((Authentication*)(sa->auth))->authenticate(&(ip->body), size, result, sa);
		if(memcmp(result, ip->body + size, 12) != 0) {
			printf(" 3. ICV Validation : Discard Packet \n");
			return false;
		}
	} else {
		size = endian16(ip->length) - (ip->ihl * 4) - ESP_HEADER_LEN; 
	}

	// 4. Decrypt
	sa->crypto = get_cryptography(sa->esp_crypto_algorithm); //is not need is set when sa create
	((Cryptography*)(sa->crypto))->decrypt(esp, size, sa); 
	
	// 5. ESP Header & Trailer Deletion
	ESP_T* esp_trailer = (ESP_T*)(&esp->body[size-2]);
	if(sa->mode == TRANSPORT) {
		ip->protocol = esp_trailer->next_hdr;
		ip->ttl--;
		if(sa->iv_mode) {
			transport_unset(packet, ESP_HEADER_LEN, ICV_LEN);
		} else {
			transport_unset(packet, ESP_HEADER_LEN, 0);
		}
	} else if(sa->mode == TUNNEL) {
		if(sa->iv_mode) {
			tunnel_unset(packet, ESP_HEADER_LEN, ICV_LEN);
		} else {
			tunnel_unset(packet, ESP_HEADER_LEN, 0);
		}
	}

	return 0;
}

static bool ipsec_encrypt(Packet* packet, SA* sa) {
	Ether* ether = (Ether*)(packet->buffer + packet->start);
        IP* ip = (IP*)ether->payload;

	int padding_len = (endian16(ip->length) - (ip->ihl * 4) + 2) % 8;
	if(padding_len != 0)
		padding_len = 8 - padding_len;

	if(sa->mode == TRANSPORT) {
		if(sa->iv_mode) {
			if(!transport_set(packet, ESP_HEADER_LEN, padding_len + ICV_LEN))
				return false;
		} else  {
			if(!transport_set(packet, ESP_HEADER_LEN, padding_len))
				return false;
		}
	} else if(sa->mode == TUNNEL) {
		if(sa->iv_mode) {
			if(!tunnel_set(packet, ESP_HEADER_LEN, padding_len + ICV_LEN))
				return false;
		} else {
			if(!transport_set(packet, ESP_HEADER_LEN, padding_len))
				return false;
		}
	}

	ether = (Ether*)(packet->buffer + packet->start);
        ip = (IP*)ether->payload;
	//Set ESP Trailer
	ESP_T* esp_trailer = (ESP_T*)(ip->body + endian16(ip->length) + padding_len);
	esp_trailer->pad_len = padding_len;
	if(sa->mode == TRANSPORT) {
		esp_trailer->next_hdr = ip->protocol;
	} else if(sa->mode == TUNNEL) {
		esp_trailer->next_hdr = IP_PROTOCOL_IP;
	}
	ip->length = endian16(endian16(ip->length) + padding_len + ESP_TRAILER_LEN);

	ESP* esp = (ESP*)ip->body;
	// 5. Seq# Validation
	((Cryptography*)(sa->crypto))->encrypt(esp->body, endian16(ip->length) - ESP_HEADER_LEN, sa);		
	esp->seq_num = endian32(++sa->window->seq_counter);
	esp->spi = endian32(sa->spi);
	esp->iv = sa->iv;	

	// 6. ICV Calculation
	if(sa->iv_mode == true) {
		int size = endian16(ip->length) - (ip->ihl * 4); 
		unsigned char* result = &(ip->body[size]);
		((Authentication*)(sa->auth))->authenticate(&(ip->body), size, result, sa);
		ip->length = endian16(endian16(ip->length) + ICV_LEN);
	}

	ip->protocol = IP_PROTOCOL_ESP;

	switch(sa->mode) {
		case TRANSPORT:
			ip->ttl--;
			ip->checksum = 0;
			ip->checksum = endian16(checksum(ip, ip->ihl * 4));
			break;
		case TUNNEL:
			ip->ttl = IP_TTL;
			ip->source = endian32(sa->t_src_ip);
			ip->destination = endian32(sa->t_dst_ip);
			ip->checksum = 0;
			ip->checksum = endian16(checksum(ip, ip->ihl * 4));
			break;
	}

	//packet->end += endian16(ip->length) - origin_length;
	
	return 0;
}

//Check auth
static bool ipsec_proof(Packet* packet, SA* sa) {
	Ether* ether = (Ether*)(packet->buffer + packet->start);
        IP* ip = (IP*)ether->payload;
	AH* ah = (AH*)ip->body;

	ah->seq_num = endian32(++sa->window->seq_counter);

	uint8_t ecn = ip->ecn;
	uint8_t dscp = ip->dscp;
	uint16_t flags_offset = ip->flags_offset;
	uint8_t ttl = ip->ttl;
	uint8_t auth_data[ICV_LEN];
	memcpy(auth_data, ah->auth_data, ICV_LEN);

	//Authenticate
	ip->ecn = 0; //tos
	ip->dscp = 0; //tos
	ip->ttl = 0;
	ip->flags_offset = 0;
	memset(ah->auth_data, 0, ICV_LEN);
	((Authentication*)(sa->auth))->authenticate(ip, endian16(ip->length), ah->auth_data, sa);

	if(memcmp(auth_data, ah->auth_data, ICV_LEN)) {
		ip->ecn = ecn;
		ip->dscp = dscp;
		ip->ttl = ttl;
		memcpy(ah->auth_data, auth_data, ICV_LEN);

		return false;
	}

	if(ah->next_hdr == IP_PROTOCOL_IP)
		//Tunnel mode
		tunnel_unset(packet, AH_HEADER_LEN, 0);
	else {
		//Transport mode
		ip->protocol = ah->next_hdr;
		transport_unset(packet, AH_HEADER_LEN, 0);
		ip->ecn = ecn;
		ip->dscp = dscp;
		ip->ttl = ttl;
		ip->flags_offset = flags_offset;
	}

	return true;
}

static bool ipsec_auth(Packet* packet, SA* sa) {
	Ether* ether = NULL;
        IP* ip = NULL;
	AH* ah = NULL;

	if(sa->mode == TRANSPORT) {
		if(!transport_set(packet, AH_HEADER_LEN, 0))
			return false;

		ether = (Ether*)(packet->buffer + packet->start);
		ip = (IP*)ether->payload;
		ah = (AH*)ip->body;

		ip->length = endian16(endian16(ip->length) + AH_HEADER_LEN + ICV_LEN);
		ah->next_hdr = ip->protocol;
	} else if(sa->mode == TUNNEL) {
		if(!tunnel_set(packet, AH_HEADER_LEN, 0))
			return false;

		ether = (Ether*)(packet->buffer + packet->start);
		ip = (IP*)ether->payload;
		ah = (AH*)ip->body;

		ip->length = endian16(endian16(ip->length) + IP_LEN + AH_HEADER_LEN + ICV_LEN);
		ah->next_hdr = IP_PROTOCOL_IP;
	}

	ah->len = 4; //check
	ah->spi = endian32(sa->spi);
	ah->seq_num = endian32(++sa->window->seq_counter);

	uint8_t ecn = ip->ecn;
	uint8_t dscp = ip->dscp;
	uint8_t ttl = ip->ttl;
	uint16_t flags_offset = ip->flags_offset;

	ip->ecn = 0;
	ip->dscp = 0;
	ip->ttl = 0;
	ip->protocol = IP_PROTOCOL_AH;
	ip->flags_offset = 0;
	memset(ah->auth_data, 0, ICV_LEN);

	((Authentication*)(sa->auth))->authenticate(ip, endian16(ip->length), ah->auth_data, sa);

	ip->ecn = ecn;
	ip->dscp = dscp;
	ip->ttl = ttl;
	ip->flags_offset = flags_offset;

	switch(sa->mode) {
		case TRANSPORT:
			ip->ttl--;
			ip->checksum = 0;
			ip->checksum = endian16(checksum(ip, ip->ihl * 4));
			break;
		case TUNNEL:
			ip->ttl = IP_TTL;
			ip->source = endian32(sa->t_src_ip);
			ip->destination = endian32(sa->t_dst_ip);
			ip->checksum = 0;
			ip->checksum = endian16(checksum(ip, ip->ihl * 4));
			break;
	}

	return true;
}

static bool inbound_process(Packet* packet) {
	Ether* ether = (Ether*)(packet->buffer + packet->start);
        IP* ip = (IP*)ether->payload;
	// 1. SAD Lookup
	SA* sa = NULL;
	while((ip->protocol == IP_PROTOCOL_ESP) || (ip->protocol == IP_PROTOCOL_AH)) {
		switch(ip->protocol) {
			case IP_PROTOCOL_ESP: {
				ESP* esp = (ESP*)ip->body;
				if((sa = sad_sa_get(esp->spi, endian32(ip->destination), ip->protocol)) == NULL) {
					printf(" 1. SAD Lookup : Discard ip \n");
					return false;
				}
				ipsec_decrypt(packet, sa);
				break;
			}

			case IP_PROTOCOL_AH: {
				AH* ah = (AH*)ip->body;
				if((sa = sad_sa_get(ah->spi, endian32(ip->destination), ip->protocol)) == NULL) {
					printf(" 1. SAD Lookup : Discard ip \n");
					return false;
				}
				ipsec_proof(packet, sa);
				break;
			}
		}
	}

	// 6. SPD Lookup 
	if(spd_get(ip) == NULL) {
		printf(" 6. SPD Lookup : Discard ip \n");
		return false;
	}

	return true;
}

static bool outbound_process(Packet* packet) {
	Ether* ether = (Ether*)(packet->buffer + packet->start);
        IP* ip = (IP*)ether->payload;
	
	SP* sp = NULL;
	SA* sa = NULL;
	if(ip->protocol == IP_PROTOCOL_TCP) { //if protocol is tcp use socket pointer 
		TCP* tcp = (TCP*)ip->body;
		Socket* socket = socket_get(endian32(ip->source), endian16(tcp->source));
		if(socket) {
			/*This Packet Is TCP Packet*/
			sp = socket->sp;
			sa = socket->sa;
			if(tcp->fin) {
				//socket free
				//TODO timer event
				socket_delete(endian32(ip->source), endian16(tcp->source));
			}
			goto tcp_packet;
		}
	}

	if(sp == NULL) {
		// 1. SPD Lookup
		sp = spd_get(ip);
		if(sp == NULL) {
			printf(" 1. SPD Lookup : No Policy ip\n");
			return false;
		}
	}

tcp_packet:
	if(sp->action == BYPASS) {
		if((ip->protocol == IP_PROTOCOL_TCP)) {
			TCP* tcp = (TCP*)ip->body;
			if(!socket_exist(endian32(ip->source), endian16(tcp->source))) { //add check socket exist
				Socket* socket = socket_create(sp, NULL);
				socket_add(endian32(ip->source), endian16(tcp->source), socket);
			}
		}
		return 0;	
	}

	ListIterator iter;
	list_iterator_init(&iter, sp->contents);
	printf("contents count%d\n", list_size(sp->contents));
	Content* con;
	while((con = (Content*)list_iterator_next(&iter)) != NULL) {
		printf("contents protocol %d", con->protocol);
		if(sa == NULL) {
			if((sa = sp_sa_get(sp, con, ip, OUT)) == NULL) {
				if((sa = ike_sa_get(ip, con)) == NULL) {
					printf(" 2. SAD check : Discard ip\n");
					return false;
				}

				if(!sad_sa_add(sa)) {
					sa_free(sa);
					return false;
				}

				if(ip->protocol == IP_PROTOCOL_TCP) {
					TCP* tcp = (TCP*)ip->body;
					Socket* socket = socket_create(sp, sa);
					socket_add(endian32(ip->source), endian16(tcp->source), socket);
				}
			}
		}
		// 2. SAD Lookup
		switch(con->protocol) {
			case IP_PROTOCOL_ESP:
				printf("esp bef size: %d\n", endian16(ip->length));
				ipsec_encrypt(packet, sa);
				printf("esp after size: %d\n", endian16(ip->length));
				break;

			case IP_PROTOCOL_AH:
				printf("ah bef size: %d\n", endian16(ip->length));
				ipsec_auth(packet, sa);
				printf("ah after size: %d\n", endian16(ip->length));
				break;
		}
		sa = NULL;
	}

	return true;
}

bool ipsec_process(Packet* packet) {
	if(arp_process(packet))
		return true;

	if(icmp_process(packet))
		return true;

	Ether* ether = (Ether*)(packet->buffer + packet->start);
	if(endian16(ether->type) == ETHER_TYPE_IPv4) {
		if(inbound_process(packet)) {
			return true;
		}

		if(outbound_process(packet)) {
			return true;
		}

		return false;
	} else {
	}

	return false;
}
