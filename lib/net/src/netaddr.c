/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/netaddr.h"
#include "util/log_macros.h"
#include <stdio.h>

bool net_addr_is_rfc1918(const struct net_addr *a)
{
    return net_addr_is_ipv4(a) && (
        net_addr_get_byte(a, 3) == 10 ||
        (net_addr_get_byte(a, 3) == 192 && net_addr_get_byte(a, 2) == 168) ||
        (net_addr_get_byte(a, 3) == 172 && net_addr_get_byte(a, 2) >= 16 &&
         net_addr_get_byte(a, 2) <= 31));
}

bool net_addr_is_rfc2544(const struct net_addr *a)
{
    return net_addr_is_ipv4(a) && net_addr_get_byte(a, 3) == 198 &&
           (net_addr_get_byte(a, 2) == 18 || net_addr_get_byte(a, 2) == 19);
}

bool net_addr_is_rfc3927(const struct net_addr *a)
{
    return net_addr_is_ipv4(a) && net_addr_get_byte(a, 3) == 169 &&
           net_addr_get_byte(a, 2) == 254;
}

bool net_addr_is_rfc6598(const struct net_addr *a)
{
    return net_addr_is_ipv4(a) && net_addr_get_byte(a, 3) == 100 &&
           net_addr_get_byte(a, 2) >= 64 && net_addr_get_byte(a, 2) <= 127;
}

bool net_addr_is_rfc5737(const struct net_addr *a)
{
    return net_addr_is_ipv4(a) &&
        ((net_addr_get_byte(a, 3) == 192 && net_addr_get_byte(a, 2) == 0 &&
          net_addr_get_byte(a, 1) == 2) ||
         (net_addr_get_byte(a, 3) == 198 && net_addr_get_byte(a, 2) == 51 &&
          net_addr_get_byte(a, 1) == 100) ||
         (net_addr_get_byte(a, 3) == 203 && net_addr_get_byte(a, 2) == 0 &&
          net_addr_get_byte(a, 1) == 113));
}

bool net_addr_is_rfc3964(const struct net_addr *a)
{
    return net_addr_get_byte(a, 15) == 0x20 && net_addr_get_byte(a, 14) == 0x02;
}

bool net_addr_is_rfc6052(const struct net_addr *a)
{
    static const unsigned char pchRFC6052[] =
        {0,0x64,0xFF,0x9B,0,0,0,0,0,0,0,0};
    return memcmp(a->ip, pchRFC6052, sizeof(pchRFC6052)) == 0;
}

bool net_addr_is_rfc4380(const struct net_addr *a)
{
    return net_addr_get_byte(a, 15) == 0x20 && net_addr_get_byte(a, 14) == 0x01 &&
           net_addr_get_byte(a, 13) == 0 && net_addr_get_byte(a, 12) == 0;
}

bool net_addr_is_rfc4862(const struct net_addr *a)
{
    static const unsigned char pchRFC4862[] = {0xFE,0x80,0,0,0,0,0,0};
    return memcmp(a->ip, pchRFC4862, sizeof(pchRFC4862)) == 0;
}

bool net_addr_is_rfc4193(const struct net_addr *a)
{
    return (net_addr_get_byte(a, 15) & 0xFE) == 0xFC;
}

bool net_addr_is_rfc6145(const struct net_addr *a)
{
    static const unsigned char pchRFC6145[] =
        {0,0,0,0,0,0,0,0,0xFF,0xFF,0,0};
    return memcmp(a->ip, pchRFC6145, sizeof(pchRFC6145)) == 0;
}

bool net_addr_is_rfc4843(const struct net_addr *a)
{
    return net_addr_get_byte(a, 15) == 0x20 && net_addr_get_byte(a, 14) == 0x01 &&
           net_addr_get_byte(a, 13) == 0x00 &&
           (net_addr_get_byte(a, 12) & 0xF0) == 0x10;
}

bool net_addr_is_local(const struct net_addr *a)
{
    if (net_addr_is_ipv4(a) &&
        (net_addr_get_byte(a, 3) == 127 || net_addr_get_byte(a, 3) == 0))
        return true;
    static const unsigned char pchLocal[16] =
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    if (memcmp(a->ip, pchLocal, 16) == 0)
        return true;
    return false;
}

bool net_addr_is_routable(const struct net_addr *a)
{
    return net_addr_is_valid(a) &&
           !(net_addr_is_rfc1918(a) || net_addr_is_rfc2544(a) ||
             net_addr_is_rfc3927(a) || net_addr_is_rfc4862(a) ||
             net_addr_is_rfc6598(a) || net_addr_is_rfc5737(a) ||
             (net_addr_is_rfc4193(a) && !net_addr_is_tor(a)) ||
             net_addr_is_rfc4843(a) || net_addr_is_local(a));
}

size_t net_addr_get_group(const struct net_addr *a, unsigned char *out,
                          size_t out_size)
{
    int nClass = NET_IPV6;
    int nStartByte = 0;
    int nBits = 16;

    if (net_addr_is_local(a)) {
        nClass = 255;
        nBits = 0;
    } else if (!net_addr_is_routable(a)) {
        nClass = NET_UNROUTABLE;
        nBits = 0;
    } else if (net_addr_is_ipv4(a) || net_addr_is_rfc6145(a) ||
               net_addr_is_rfc6052(a)) {
        nClass = NET_IPV4;
        nStartByte = 12;
    } else if (net_addr_is_rfc3964(a)) {
        nClass = NET_IPV4;
        nStartByte = 2;
    } else if (net_addr_is_rfc4380(a)) {
        if (out_size < 3) return 0;
        out[0] = NET_IPV4;
        out[1] = net_addr_get_byte(a, 3) ^ 0xFF;
        out[2] = net_addr_get_byte(a, 2) ^ 0xFF;
        return 3;
    } else if (net_addr_is_tor(a)) {
        nClass = NET_ONION;
        nStartByte = 6;
        nBits = 4;
    } else if (net_addr_get_byte(a, 15) == 0x20 &&
               net_addr_get_byte(a, 14) == 0x01 &&
               net_addr_get_byte(a, 13) == 0x04 &&
               net_addr_get_byte(a, 12) == 0x70) {
        nBits = 36;
    } else {
        nBits = 32;
    }

    size_t pos = 0;
    if (pos >= out_size) return 0;
    out[pos++] = (unsigned char)nClass;
    while (nBits >= 8 && pos < out_size) {
        out[pos++] = net_addr_get_byte(a, 15 - nStartByte);
        nStartByte++;
        nBits -= 8;
    }
    if (nBits > 0 && pos < out_size)
        out[pos++] = net_addr_get_byte(a, 15 - nStartByte) |
                     (unsigned char)((1 << (8 - nBits)) - 1);
    return pos;
}

void net_service_get_key(const struct net_service *s, unsigned char out[18])
{
    memcpy(out, s->addr.ip, 16);
    out[16] = (unsigned char)(s->port >> 8);
    out[17] = (unsigned char)(s->port & 0xFF);
}

int net_addr_to_string(const struct net_addr *a, char *out, size_t out_size)
{
    if (net_addr_is_ipv4(a)) {
        return snprintf(out, out_size, "%u.%u.%u.%u",
                        a->ip[12], a->ip[13], a->ip[14], a->ip[15]);
    }

    if (a->has_torv3) {
        return snprintf(out, out_size, "[torv3]");
    }

    return snprintf(out, out_size,
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                    a->ip[0], a->ip[1], a->ip[2], a->ip[3],
                    a->ip[4], a->ip[5], a->ip[6], a->ip[7],
                    a->ip[8], a->ip[9], a->ip[10], a->ip[11],
                    a->ip[12], a->ip[13], a->ip[14], a->ip[15]);
}

int net_service_to_string(const struct net_service *s, char *out, size_t out_size)
{
    char addr_str[128];
    net_addr_to_string(&s->addr, addr_str, sizeof(addr_str));

    if (net_addr_is_ipv6(&s->addr) && !s->addr.has_torv3) {
        return snprintf(out, out_size, "[%s]:%u", addr_str, s->port);
    }
    return snprintf(out, out_size, "%s:%u", addr_str, s->port);
}
