#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include "mod_webfw2.h"
#include "thrasher.h"

apr_socket_t   *
thrasher_connect(apr_pool_t * pool, webfw2_config_t * config)
{
    /*
     * generic connect() function using thrasher configuration
     * directives 
     */
    apr_socket_t   *sock;
    apr_sockaddr_t *sockaddr;

    sock = NULL;

    if (apr_sockaddr_info_get(&sockaddr,
                              config->thrasher_host, APR_INET,
                              config->thrasher_port, 0,
                              pool) != APR_SUCCESS)
        return NULL;

    if (apr_socket_create(&sock, sockaddr->family,
                          SOCK_STREAM, APR_PROTO_TCP, pool) != APR_SUCCESS)
        return NULL;

    if (apr_socket_timeout_set(sock,
                               config->thrasher_timeout) != APR_SUCCESS) {
        apr_socket_close(sock);
        return NULL;
    }

    if (apr_socket_opt_set(sock, APR_SO_KEEPALIVE, 1) != APR_SUCCESS) {
        apr_socket_close(sock);
        return NULL;
    }

    if (apr_socket_connect(sock, sockaddr) != APR_SUCCESS) {
        apr_socket_close(sock);
        return NULL;
    }

    return sock;
}

int
thrasher_is_connected(apr_socket_t * sock)
{
    return sock ? 1 : 0;
}

void
thrasher_err_shutdown(webfw2_filter_t * filter)
{
    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, NULL,
                 "thrasher socket error, shutting down");

    if (filter->thrasher_sock)
        apr_socket_close(filter->thrasher_sock);

    filter->thrasher_sock = NULL;
    filter->thrasher_downed = time(NULL);
}

int
thrasher_should_retry(webfw2_config_t * config, webfw2_filter_t * filter)
{
    time_t          currtime;

    currtime = time(NULL);

    /*
     * if the thrasher daemon has been downed for a set amount of
     * time greater than the retry configuration directive we return
     * true 
     */

    if (currtime - filter->thrasher_downed > config->thrasher_retry)
        return 1;

    return 0;
}

static int
thrasher_recv_boolean(thrasher_pkt_t * pkt, apr_socket_t * sock)
{
    uint8_t         resp;
    apr_size_t      torecv;

    torecv = 1;

    if (apr_socket_recv(sock, (char *) &resp, &torecv) != APR_SUCCESS)
        return -1;

    if (resp > 1)
        return -1;

    return (int) resp;
}

static thrasher_pkt_t *
thrasher_create_v1_pkt(apr_pool_t * pool,
                       char *host, char *uri, uint32_t addr,
                       uint16_t hlen, uint16_t urilen)
{
    apr_size_t      pktlen;
    thrasher_pkt_t *pkt;
    uint16_t        hlen_nbo,
                    urilen_nbo;

    if (!addr || !host || !uri || !hlen || !urilen)
        return NULL;

    if (!(pkt = apr_pcalloc(pool, sizeof(thrasher_pkt_t))))
        return NULL;

    hlen_nbo = htons(hlen);
    urilen_nbo = htons(urilen);

    pktlen = sizeof(uint8_t) +  /* packet type */
        sizeof(uint32_t) +      /* ip address  */
        sizeof(uint16_t) +      /* uri length  */
        sizeof(uint16_t) +      /* host length */
        hlen + urilen;          /* payloads    */

    if (!(pkt->packet = apr_pcalloc(pool, pktlen)))
        return NULL;

    memcpy(&pkt->packet[1], &addr, sizeof(uint32_t));
    memcpy(&pkt->packet[5], &urilen_nbo, sizeof(uint16_t));
    memcpy(&pkt->packet[7], &hlen_nbo, sizeof(uint16_t));
    memcpy(&pkt->packet[9], uri, urilen);
    memcpy(&pkt->packet[9 + urilen], host, hlen);

    pkt->len = pktlen;
    pkt->thrasher_recv_cb = thrasher_recv_boolean;

    return pkt;
}

static thrasher_pkt_t *
thrasher_create_v2_pkt(apr_pool_t * pool, uint32_t addr)
{
    apr_size_t      pktlen;
    thrasher_pkt_t *pkt;

    if (!addr)
        return NULL;

    if (!(pkt = apr_pcalloc(pool, sizeof(thrasher_pkt_t))))
        return NULL;

    pktlen = sizeof(uint32_t) + sizeof(uint8_t);

    if (!(pkt->packet = apr_pcalloc(pool, pktlen)))
        return NULL;

    *pkt->packet = 3;           /* type 1, v2 */
    memcpy(&pkt->packet[1], &addr, sizeof(uint32_t));

    pkt->len = pktlen;
    pkt->thrasher_recv_cb = thrasher_recv_boolean;

    return pkt;
}

static int
thrasher_recv_v3_pkt(thrasher_pkt_t * pkt, apr_socket_t * sock)
{
    uint8_t         allowed;
    uint32_t        ident;
    apr_size_t      torecv;
    char            recv_data[5];


    /*
     * recv 4 byte ident along with the boolean on whether
     * the address is allowed or not 
     */
    torecv = 5;
    if (apr_socket_recv(sock, recv_data, &torecv) != APR_SUCCESS)
        return -1;


    memcpy(&ident, recv_data, sizeof(uint32_t));
    allowed = (uint8_t) recv_data[4];

    if (pkt->ident != ntohl(ident))
        /*
         * our identifiers did not match, we must
         * return an error, something very odd 
         * happened here 
         */
        return -1;

    if (allowed > 1)
        return -1;

    return allowed;
}

static thrasher_pkt_t *
thrasher_create_v3_pkt(apr_pool_t * pool, uint32_t ident,
                       char *host, char *uri, uint32_t addr,
                       uint16_t hlen, uint16_t urilen)
{
    apr_size_t      pktlen;
    thrasher_pkt_t *pkt;
    uint16_t        hlen_nbo,
                    urilen_nbo;
    uint32_t        ident_nbo;

    if (!addr || !host || !uri || !hlen || !urilen)
        return NULL;

    if (!(pkt = apr_pcalloc(pool, sizeof(thrasher_pkt_t))))
        return NULL;

    hlen_nbo = htons(hlen);
    urilen_nbo = htons(urilen);
    ident_nbo = htonl(ident);

    pktlen = sizeof(uint8_t) +  /* type  */
        sizeof(uint32_t) +      /* ident */
        sizeof(uint32_t) +      /* addr  */
        sizeof(uint16_t) +      /* uri len */
        sizeof(uint16_t) +      /* host len */
        hlen + urilen;          /* payloads */

    if (!(pkt->packet = apr_pcalloc(pool, pktlen)))
        return NULL;

    *pkt->packet = TYPE_THRESHOLD_v3;

    memcpy(&pkt->packet[1], &ident_nbo, sizeof(uint32_t));
    memcpy(&pkt->packet[5], &addr, sizeof(uint32_t));
    memcpy(&pkt->packet[9], &urilen_nbo, sizeof(uint16_t));
    memcpy(&pkt->packet[11], &hlen_nbo, sizeof(uint16_t));
    memcpy(&pkt->packet[13], uri, urilen);
    memcpy(&pkt->packet[13 + urilen], host, hlen);

    pkt->len = pktlen;
    pkt->thrasher_recv_cb = thrasher_recv_v3_pkt;
    pkt->ident = ident;

    return pkt;
}

static thrasher_pkt_t *
thrasher_create_v4_pkt(apr_pool_t * pool, uint32_t ident,
                       char *host, char *uri, uint32_t addr,
                       uint16_t hlen, uint16_t urilen, char *reason, uint16_t rlen)
{
    apr_size_t      pktlen;
    thrasher_pkt_t *pkt;
    uint16_t        hlen_nbo,
                    urilen_nbo,
                    rlen_nbo;
    uint32_t        ident_nbo;

    if (!addr || !host || !uri || !hlen || !urilen)
        return NULL;

    if (!(pkt = apr_pcalloc(pool, sizeof(thrasher_pkt_t))))
        return NULL;

    hlen_nbo = htons(hlen);
    urilen_nbo = htons(urilen);
    ident_nbo = htonl(ident);
    rlen_nbo = htons(rlen);

    pktlen = sizeof(uint8_t) +  /* type  */
        sizeof(uint32_t) +      /* ident */
        sizeof(uint32_t) +      /* addr  */
        sizeof(uint16_t) +      /* uri len */
        sizeof(uint16_t) +      /* host len */
        sizeof(uint16_t) +      /* reason len */
        hlen + urilen + rlen;   /* payloads */

    if (!(pkt->packet = apr_pcalloc(pool, pktlen)))
        return NULL;

    *pkt->packet = TYPE_THRESHOLD_v4;

    memcpy(&pkt->packet[1], &rlen_nbo, sizeof(uint16_t));
    memcpy(&pkt->packet[3], &ident_nbo, sizeof(uint32_t));
    memcpy(&pkt->packet[7], &addr, sizeof(uint32_t));
    memcpy(&pkt->packet[11], &urilen_nbo, sizeof(uint16_t));
    memcpy(&pkt->packet[13], &hlen_nbo, sizeof(uint16_t));
    memcpy(&pkt->packet[15], uri, urilen);
    memcpy(&pkt->packet[15 + urilen], host, hlen);
    memcpy(&pkt->packet[15 + urilen + hlen], reason, rlen);

    pkt->len = pktlen;
    pkt->thrasher_recv_cb = thrasher_recv_v3_pkt;
    pkt->ident = ident;

    return pkt;
}

static thrasher_pkt_t *
thrasher_create_v6_pkt(apr_pool_t * pool, uint32_t ident,
                       char *host, char *uri, const char *addr,
                       uint16_t hlen, uint16_t urilen, char *reason, uint16_t rlen)
{
    apr_size_t      pktlen;
    thrasher_pkt_t *pkt;
    uint16_t        hlen_nbo,
                    urilen_nbo,
                    rlen_nbo;
    uint32_t        ident_nbo;
    unsigned char   s6addr[16];

    if (!addr || !host || !uri || !hlen || !urilen)
        return NULL;

    if (!(pkt = apr_pcalloc(pool, sizeof(thrasher_pkt_t))))
        return NULL;

    hlen_nbo = htons(hlen);
    urilen_nbo = htons(urilen);
    ident_nbo = htonl(ident);
    rlen_nbo = htons(rlen);

    pktlen = sizeof(uint8_t) +  /* type  */
        sizeof(uint32_t) +      /* ident */
        sizeof(uint32_t) +      /* s6addr */
        sizeof(uint16_t) +      /* uri len */
        sizeof(uint16_t) +      /* host len */
        sizeof(uint16_t) +      /* reason len */
        hlen + urilen + rlen;   /* payloads */

    if (!(pkt->packet = apr_pcalloc(pool, pktlen)))
        return NULL;

    *pkt->packet = TYPE_THRESHOLD_v6;

    memcpy(&pkt->packet[1], &rlen_nbo, sizeof(uint16_t));
    memcpy(&pkt->packet[3], &ident_nbo, sizeof(uint32_t));
    memcpy(&pkt->packet[7], s6addr, 16);
    memcpy(&pkt->packet[23], &urilen_nbo, sizeof(uint16_t));
    memcpy(&pkt->packet[25], &hlen_nbo, sizeof(uint16_t));
    memcpy(&pkt->packet[27], uri, urilen);
    memcpy(&pkt->packet[27 + urilen], host, hlen);
    memcpy(&pkt->packet[27 + urilen + hlen], reason, rlen);

    pkt->len = pktlen;
    pkt->thrasher_recv_cb = thrasher_recv_v3_pkt;
    pkt->ident = ident;

    return pkt;
}

int
thrasher_query(request_rec * rec, webfw2_config_t * config,
               webfw2_filter_t * filter, thrasher_pkt_type type,
               const char *srcaddr, uint32_t ident, char *reason)
{
    /*
     * returns 0 if the host is allowed, 
     * returns 1 if the host has been denied,
     * returns -1 if there was an error 
     */
    int             ret;
    thrasher_pkt_t *pkt;

    pkt = NULL;
    ret = 0;

    if (!thrasher_is_connected(filter->thrasher_sock))
        return -1;

    switch (type) {
    case TYPE_THRESHOLD_v1:
        pkt = thrasher_create_v1_pkt(rec->pool,
                                     (char *) rec->hostname,
                                     (char *) rec->uri, inet_addr(srcaddr),
                                     strlen(rec->hostname),
                                     strlen(rec->uri));
        break;
    case TYPE_THRESHOLD_v2:
        pkt = thrasher_create_v2_pkt(rec->pool, inet_addr(srcaddr));
        break;
    case TYPE_THRESHOLD_v3:
        pkt = thrasher_create_v3_pkt(rec->pool,
                                     ident, (char *) rec->hostname,
                                     rec->uri, inet_addr(srcaddr),
                                     strlen(rec->hostname),
                                     strlen(rec->uri));
        break;
    case TYPE_THRESHOLD_v4:
        pkt = thrasher_create_v4_pkt(rec->pool,
                                     ident, (char *) rec->hostname,
                                     rec->uri, inet_addr(srcaddr),
                                     strlen(rec->hostname),
                                     strlen(rec->uri),
                                     reason, strlen(reason));
        break;
    case TYPE_THRESHOLD_v6:
        pkt = thrasher_create_v6_pkt(rec->pool,
                                     ident, (char *) rec->hostname,
                                     rec->uri, srcaddr,
                                     strlen(rec->hostname),
                                     strlen(rec->uri),
                                     reason, strlen(reason));
        break;
    default:
        return -1;
    }

    if (!pkt)
        return -1;

    if (apr_socket_send(filter->thrasher_sock,
                        (const char *) pkt->packet,
                        &pkt->len) != APR_SUCCESS)
        return -1;

    return pkt->thrasher_recv_cb(pkt, filter->thrasher_sock);
}
