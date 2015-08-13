#ifndef __CONTENT_H__
#define __CONTENT_H__
#include <stdbool.h>
#include "crypto.h"
#include "auth.h"
// Mode
#define TRANSPORT 	0x01
#define TUNNEL 		0x02

typedef enum {
	CONTENT_PROTOCOL,
	CONTENT_MODE,
	CONTENT_TUNNEL_SOURCE_ADDR,
	CONTENT_TUNNEL_DESTINATION_ADDR,
	CONTENT_CRYPTO_ALGORITHM,
	CONTENT_AUTH_ALGORITHM,
} CONTENT_ATTRIBUTES;

typedef struct _Content{
	uint8_t protocol;
	uint8_t mode;
} Content;

typedef struct _Content_AH_Transport {
	Content content;

        uint8_t auth_algorithm;
} Content_AH_Transport;

typedef struct _Content_AH_Tunnle {
	Content content;

        uint8_t auth_algorithm;

	uint32_t t_src_ip;
	uint32_t t_dst_ip;
} Content_AH_Tunnel;

typedef struct _Content_ESP_Transport {
	Content content;

	bool iv_mode;
 	uint8_t crypto_algorithm;
        uint8_t auth_algorithm;
} Content_ESP_Transport;

typedef struct _Content_ESP_Tunnel {
	Content content;

	bool iv_mode;
 	uint8_t crypto_algorithm;
        uint8_t auth_algorithm;

	uint32_t t_src_ip;
	uint32_t t_dst_ip;
} Content_ESP_Tunnel;

Content* content_alloc(uint64_t* attrs);
void content_free(Content* content);
#endif /*__CONTENT_H__*/
