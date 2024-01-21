/*
 *  Copyright (c) 2009-2024, Peter Haag
 *  Copyright (c) 2004-2008, SWITCH - Teleinformatikdienste fuer Lehre und Forschung
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *   * Neither the name of the author nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "nflowcache.h"

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

#include "blocksort.h"
#include "config.h"
#include "exporter.h"
#include "khash.h"
#include "klist.h"
#include "maxmind.h"
#include "memhandle.h"
#include "nfdump.h"
#include "nffile.h"
#include "nfxV3.h"
#include "output.h"
#include "util.h"

typedef struct aggregate_param_s {
    uint32_t extID;   // extension ID
    uint32_t offset;  // offset in master record
    uint32_t length;  // size of parameter in bytes
} aggregate_param_t;

#define MaxMaskArraySize 16
static struct maskArray_s {
    uint32_t v4Mask;
    uint64_t v6Mask[2];
} maskArray[MaxMaskArraySize] = {0};
uint32_t maskIndex = 0;

static struct aggregationElement_s {
    char *aggrElement;        // name of aggregation parameter
    aggregate_param_t param;  // the parameter array
    uint8_t active;           // this entry will be applied
    uint8_t geoLookup;        // may require geolookup, if empty
    uint8_t netmaskID;        // index into mask array for mask to apply
                              // 255 : use srcMask, dstMask from flow record
    uint8_t allowMask;        // element may have a netmask -> /prefix
    char *fmt;                // for automatic output format generation
} aggregationTable[] = {
    {"proto", {EXgenericFlowID, OFFproto, SIZEproto}, 0, 0, 0, 0, "%pr"},
    {"srcip4", {EXipv4FlowID, OFFsrc4Addr, SIZEsrc4Addr}, 0, 0, 0, 1, "%sa"},
    {"dstip4", {EXipv4FlowID, OFFdst4Addr, SIZEdst4Addr}, 0, 0, 0, 1, "%da"},
    {"srcip6", {EXipv6FlowID, OFFsrc6Addr, SIZEsrc4Addr}, 0, 0, 0, 1, "%sa"},
    {"dstip6", {EXipv6FlowID, OFFdst6Addr, SIZEdst6Addr}, 0, 0, 0, 1, "%da"},
    {"srcip", {EXipv4FlowID, OFFsrc4Addr, SIZEsrc4Addr}, 0, 0, 0, 1, "%sa"},
    {"srcip", {EXipv6FlowID, OFFsrc6Addr, SIZEsrc6Addr}, 0, 0, 0, 1, NULL},
    {"dstip", {EXipv4FlowID, OFFdst4Addr, SIZEdst4Addr}, 0, 0, 0, 1, "%da"},
    {"dstip", {EXipv6FlowID, OFFdst6Addr, SIZEdst6Addr}, 0, 0, 0, 1, NULL},
    {"srcport", {EXgenericFlowID, OFFsrcPort, SIZEsrcPort}, 0, 0, 0, 0, "%sp"},
    {"dstport", {EXgenericFlowID, OFFdstPort, SIZEdstPort}, 0, 0, 0, 0, "%dp"},
    /*
                           {"srcnet", {8, OffsetSrcIPv6a, MaskIPv6, ShiftIPv6}, -1, 0, 0, "%sn"},
                           {"srcnet", {8, OffsetSrcIPv6b, MaskIPv6, ShiftIPv6}, -1, 0, 0, NULL},
                           {"dstnet", {8, OffsetDstIPv6a, MaskIPv6, ShiftIPv6}, -1, 0, 0, "%dn"},
                           {"dstnet", {8, OffsetDstIPv6b, MaskIPv6, ShiftIPv6}, -1, 0, 0, NULL},
                           {"xsrcip", {8, OffsetXLATESRCv6a, MaskIPv6, ShiftIPv6}, -1, 0, 0, "%xsa"},
                           {"xsrcip", {8, OffsetXLATESRCv6b, MaskIPv6, ShiftIPv6}, -1, 0, 0, NULL},
                           {"xdstip", {8, OffsetXLATEDSTv6a, MaskIPv6, ShiftIPv6}, -1, 0, 0, "%xda"},
                           {"xdstip", {8, OffsetXLATEDSTv6b, MaskIPv6, ShiftIPv6}, -1, 0, 0, NULL},
                           {"xsrcport", {2, OffsetXLATEPort, MaskXLATESRCPORT, ShiftXLATESRCPORT}, -1, 0, 0, "%xsp"},
                           {"xdstport", {2, OffsetXLATEPort, MaskXLATEDSTPORT, ShiftXLATEDSTPORT}, -1, 0, 0, "%xdp"},
                           {"dstip4", {8, OffsetDstIPv6b, MaskIPv6, ShiftIPv6}, 1, 0, 0, NULL},
                           {"dstip6", {8, OffsetDstIPv6a, MaskIPv6, ShiftIPv6}, 0, 0, 0, "%da"},
                           {"dstip6", {8, OffsetDstIPv6b, MaskIPv6, ShiftIPv6}, 1, 0, 0, NULL},
                           {"next", {8, OffsetNexthopv6a, MaskIPv6, ShiftIPv6}, -1, 0, 0, "%nh"},
                           {"next", {8, OffsetNexthopv6b, MaskIPv6, ShiftIPv6}, -1, 0, 0, NULL},
                           {"bgpnext", {8, OffsetBGPNexthopv6a, MaskIPv6, ShiftIPv6}, -1, 0, 0, "%nhb"},
                           {"bgpnext", {8, OffsetBGPNexthopv6b, MaskIPv6, ShiftIPv6}, -1, 0, 0, NULL},
                           {"router", {8, OffsetRouterv6a, MaskIPv6, ShiftIPv6}, -1, 0, 0, "%ra"},
                           {"router", {8, OffsetRouterv6b, MaskIPv6, ShiftIPv6}, -1, 0, 0, NULL},
                           {"insrcmac", {8, OffsetInSrcMAC, MaskMac, ShiftIPv6}, -1, 0, 0, "%ismc"},
                           {"outdstmac", {8, OffsetOutDstMAC, MaskMac, ShiftIPv6}, -1, 0, 0, "%odmc"},
                           {"indstmac", {8, OffsetInDstMAC, MaskMac, ShiftIPv6}, -1, 0, 0, "%idmc"},
                           {"outsrcmac", {8, OffsetOutSrcMAC, MaskMac, ShiftIPv6}, -1, 0, 0, "%osmc"},
                           {"srcas", {4, OffsetAS, MaskSrcAS, ShiftSrcAS}, -1, 0, 1, "%sas"},
                           {"dstas", {4, OffsetAS, MaskDstAS, ShiftDstAS}, -1, 0, 1, "%das"},
                           {"nextas", {4, OffsetBGPadj, MaskBGPadjNext, ShiftBGPadjNext}, -1, 0, 0, "%nas"},
                           {"prevas", {4, OffsetBGPadj, MaskBGPadjPrev, ShiftBGPadjPrev}, -1, 0, 0, "%pas"},
                           {"inif", {4, OffsetInOut, MaskInput, ShiftInput}, -1, 0, 0, "%in"},
                           {"outif", {4, OffsetInOut, MaskOutput, ShiftOutput}, -1, 0, 0, "%out"},
                           {"mpls1", {4, OffsetMPLS12, MaskMPLSlabelOdd, ShiftMPLSlabelOdd}, -1, 0, 0, "%mpls1"},
                           {"mpls2", {4, OffsetMPLS12, MaskMPLSlabelEven, ShiftMPLSlabelEven}, -1, 0, 0, "%mpls2"},
                           {"mpls3", {4, OffsetMPLS34, MaskMPLSlabelOdd, ShiftMPLSlabelOdd}, -1, 0, 0, "%mpls3"},
                           {"mpls4", {4, OffsetMPLS34, MaskMPLSlabelEven, ShiftMPLSlabelEven}, -1, 0, 0, "%mpls4"},
                           {"mpls5", {4, OffsetMPLS56, MaskMPLSlabelOdd, ShiftMPLSlabelOdd}, -1, 0, 0, "%mpls5"},
                           {"mpls6", {4, OffsetMPLS56, MaskMPLSlabelEven, ShiftMPLSlabelEven}, -1, 0, 0, "%mpls6"},
                           {"mpls7", {4, OffsetMPLS78, MaskMPLSlabelOdd, ShiftMPLSlabelOdd}, -1, 0, 0, "%mpls7"},
                           {"mpls8", {4, OffsetMPLS78, MaskMPLSlabelEven, ShiftMPLSlabelEven}, -1, 0, 0, "%mpls8"},
                           {"mpls9", {4, OffsetMPLS910, MaskMPLSlabelOdd, ShiftMPLSlabelOdd}, -1, 0, 0, "%mpls9"},
                           {"mpls10", {4, OffsetMPLS910, MaskMPLSlabelEven, ShiftMPLSlabelEven}, -1, 0, 0, "%mpls10"},
                           {"srcvlan", {2, OffsetVlan, MaskSrcVlan, ShiftSrcVlan}, -1, 0, 0, "%svln"},
                           {"dstvlan", {2, OffsetVlan, MaskDstVlan, ShiftDstVlan}, -1, 0, 0, "%dvln"},
                           {"srcmask", {1, OffsetMask, MaskSrcMask, ShiftSrcMask}, -1, 0, 0, "%smk"},
                           {"dstmask", {1, OffsetMask, MaskDstMask, ShiftDstMask}, -1, 0, 0, "%dmk"},
                           {"tos", {1, OffsetTos, MaskTos, ShiftTos}, -1, 0, 0, "%tos"},
                           {"srctos", {1, OffsetTos, MaskTos, ShiftTos}, -1, 0, 0, "%stos"},
                           {"dsttos", {1, OffsetDstTos, MaskDstTos, ShiftDstTos}, -1, 0, 0, "%dtos"},
                           {"odid", {1, OffsetObservationDomainID, MaskObservationDomainID, ShiftObservationDomainID}, -1, 0, 0, "%odid"},
                           {"opid", {1, OffsetObservationPointID, MaskObservationPointID, ShiftObservationPointID}, -1, 0, 0, "%opid"},
                           {"srcgeo", {4, OffsetGeo, MaskSrcGeo, ShiftSrcGeo}, -1, 0, 1, "%sc"},
                           {"dstgeo", {4, OffsetGeo, MaskDstGeo, ShiftDstGeo}, -1, 0, 1, "%dc"},
    */
    {NULL, {0, 0, 0}, 0, 0, 0, 0, NULL}};

/* Element of the flow hash ( cache ) */
typedef struct FlowHashRecord {
    // record chain - for FlowList
    union {
        struct FlowHashRecord *next;
        uint8_t *hashkey;
    };
    uint32_t hash;     // the full 32bit hash value - cached for khash resize
    uint16_t hashLen;  // lenhth of hashKey
    uint8_t inFlags;   // tcp in flags
    uint8_t outFlags;  // tcp out flags

    // flow counter parameters for FLOWS, INPACKETS, INBYTES, OUTPACKETS, OUTBYTES
    uint64_t counter[5];

    // time info in msec
    uint64_t msecFirst;
    uint64_t msecLast;

    recordHeaderV3_t *flowrecord;
} FlowHashRecord_t;

// printing order definitions
enum CntIndices { FLOWS = 0, INPACKETS, INBYTES, OUTPACKETS, OUTBYTES };
enum FlowDir { IN = 0, OUT, INOUT };

typedef uint64_t (*order_proc_record_t)(FlowHashRecord_t *, int);

// prototypes for order functions
static inline uint64_t null_record(FlowHashRecord_t *record, int inout);
static inline uint64_t flows_record(FlowHashRecord_t *record, int inout);
static inline uint64_t packets_record(FlowHashRecord_t *record, int inout);
static inline uint64_t bytes_record(FlowHashRecord_t *record, int inout);
static inline uint64_t pps_record(FlowHashRecord_t *record, int inout);
static inline uint64_t bps_record(FlowHashRecord_t *record, int inout);
static inline uint64_t bpp_record(FlowHashRecord_t *record, int inout);
static inline uint64_t tstart_record(FlowHashRecord_t *record, int inout);
static inline uint64_t tend_record(FlowHashRecord_t *record, int inout);
static inline uint64_t duration_record(FlowHashRecord_t *record, int inout);

#define ASCENDING 1
#define DESCENDING 0
static struct order_mode_s {
    char *string;                            // Stat name
    int inout;                               // use IN or OUT or INOUT packets/bytes
    int direction;                           // ascending or descending
    order_proc_record_t record_function;     // Function to call for record stats
} order_mode[] = {{"-", 0, 0, null_record},  // empty entry 0
                  {"flows", IN, DESCENDING, flows_record},
                  {"packets", INOUT, DESCENDING, packets_record},
                  {"ipkg", IN, DESCENDING, packets_record},
                  {"opkg", OUT, DESCENDING, packets_record},
                  {"bytes", INOUT, DESCENDING, bytes_record},
                  {"ibyte", IN, DESCENDING, bytes_record},
                  {"obyte", OUT, DESCENDING, bytes_record},
                  {"pps", INOUT, DESCENDING, pps_record},
                  {"ipps", IN, DESCENDING, pps_record},
                  {"opps", OUT, DESCENDING, pps_record},
                  {"bps", INOUT, DESCENDING, bps_record},
                  {"ibps", IN, DESCENDING, bps_record},
                  {"obps", OUT, DESCENDING, bps_record},
                  {"bpp", INOUT, DESCENDING, bpp_record},
                  {"ibpp", IN, DESCENDING, bpp_record},
                  {"obpp", OUT, DESCENDING, bpp_record},
                  {"tstart", 0, ASCENDING, tstart_record},
                  {"tend", 0, ASCENDING, tend_record},
                  {"duration", 0, DESCENDING, duration_record},
                  {NULL, 0, 0, NULL}};

static uint32_t FlowStat_order = 0;  // bit field for multiple print orders
static uint32_t PrintOrder = 0;      // -O selected print order - index into order_mode
static uint32_t PrintDirection = 0;
static uint32_t GuessDirection = 0;
static uint32_t doGeoLookup = 0;

typedef struct FlowKey_s {
    uint64_t srcAddr[2];
    uint64_t dstAddr[2];
    uint16_t srcPort;
    uint16_t dstPort;
    uint32_t proto;
} FlowKey_t;

typedef struct FlowKeyV4_s {
    uint16_t af;
    uint16_t proto;
    uint16_t srcPort;
    uint16_t dstPort;
    uint32_t srcAddr;
    uint32_t dstAddr;
} FlowKeyV4_t;

// definitions for khash flow cache
typedef const uint8_t *hashkey_t;  // hash key - byte sequence
static size_t hashKeyLen = 0;      // length of hash_key

static inline uint32_t SuperFastHash(const char *data, int len);

/*
// hash func - reduce byte sequence to kh_int
static kh_inline khint_t __HashFunc(const FlowHashRecord_t record) {
        return SuperFastHash((char *)record.hashkey, hashKeyLen);
}
*/
#define __HashFunc(k) (k).hash

// compare func - compare two hash keys
static kh_inline khint_t __HashEqual(FlowHashRecord_t r1, FlowHashRecord_t r2) {
    return r1.hash == r2.hash && memcmp((void *)r1.hashkey, (void *)r2.hashkey, hashKeyLen) == 0;
}
// insert FlowHash definitions/code
KHASH_INIT(FlowHash, FlowHashRecord_t, char, 0, __HashFunc, __HashEqual)
// FlowHash var
static khash_t(FlowHash) *FlowHash = NULL;
sig_atomic_t lock = 0;

// linear FlowList
static struct FlowList_s {
    FlowHashRecord_t *head;
    FlowHashRecord_t **tail;
    size_t NumRecords;
} FlowList;

#define MaxAggrStackSize 64
int aggregateInfo[MaxAggrStackSize] = {0};

static uint32_t bidir_flows = 0;

#include "heapsort_inline.c"
#include "memhandle.c"
#include "nfdump_inline.c"
#include "nffile_inline.c"

#undef get16bits
#if (defined(__GNUC__) && defined(__i386__)) || defined(__WATCOMC__) || defined(_MSC_VER) || defined(__BORLANDC__) || defined(__TURBOC__)
#define get16bits(d) (*((const uint16_t *)(d)))
#endif

#if !defined(get16bits)
#define get16bits(d) ((((uint32_t)(((const uint8_t *)(d))[1])) << 8) + (uint32_t)(((const uint8_t *)(d))[0]))
#endif

static inline void *New_HashKey(void *keymem, recordHandle_t *recordHandle, int swap_flow);

static SortElement_t *GetSortList(size_t *size);

static void ApplyNetMaskBits(recordHandle_t *recordHandle, struct aggregationElement_s *aggregationElement);

static void SetNetMaskBits(EXipv4Flow_t *EXipv4Flow, EXipv6Flow_t *EXipv6Flow, EXflowMisc_t *EXflowMisc, int apply_netbits);

static void PrintSortList(SortElement_t *SortList, uint32_t maxindex, outputParams_t *outputParams, int GuessFlowDirection,
                          RecordPrinter_t print_record, int ascending);

static inline uint32_t SuperFastHash(const char *data, int len) {
    uint32_t hash = len;

    if (len <= 0 || data == NULL) return 0;

    int rem = len & 3;
    len >>= 2;

    /* Main loop */
    uint32_t tmp;
    for (; len > 0; len--) {
        hash += get16bits(data);
        tmp = (get16bits(data + 2) << 11) ^ hash;
        hash = (hash << 16) ^ tmp;
        data += 2 * sizeof(uint16_t);
        hash += hash >> 11;
    }

    /* Handle end cases */
    switch (rem) {
        case 3:
            hash += get16bits(data);
            hash ^= hash << 16;
            hash ^= data[sizeof(uint16_t)] << 18;
            hash += hash >> 11;
            break;
        case 2:
            hash += get16bits(data);
            hash ^= hash << 11;
            hash += hash >> 17;
            break;
        case 1:
            hash += *data;
            hash ^= hash << 10;
            hash += hash >> 1;
    }

    /* Force "avalanching" of final 127 bits */
    hash ^= hash << 3;
    hash += hash >> 5;
    hash ^= hash << 4;
    hash += hash >> 17;
    hash ^= hash << 25;
    hash += hash >> 6;

    return hash;
}

static inline void *New_HashKey(void *keymem, recordHandle_t *recordHandle, int swap_flow) {
    EXipv4Flow_t *ipv4Flow = (EXipv4Flow_t *)recordHandle->extensionList[EXipv4FlowID];
    EXipv6Flow_t *ipv6Flow = (EXipv6Flow_t *)recordHandle->extensionList[EXipv6FlowID];
    EXgenericFlow_t *genericFlow = (EXgenericFlow_t *)recordHandle->extensionList[EXgenericFlowID];

    dbg_printf("NewHash: %d\n", aggregateInfo[0]);
    if (aggregateInfo[0] >= 0) {
        // custom user aggregation
        for (int i = 0; aggregateInfo[i] >= 0; i++) {
            // apply src/dst mask bits if requested
            uint32_t tableIndex = aggregateInfo[i];
            if (aggregationTable[tableIndex].netmaskID == 0xFF) {
                ApplyNetMaskBits(recordHandle, &aggregationTable[tableIndex]);
            }
            aggregate_param_t *param = &aggregationTable[tableIndex].param;
            if (recordHandle->extensionList[param->extID] == NULL) continue;
            void *inPtr = recordHandle->extensionList[param->extID] + param->offset;
            switch (param->length) {
                case 0:
                    break;
                case 1: {
                    *((uint8_t *)keymem) = *((uint8_t *)inPtr);
                    keymem += sizeof(uint8_t);
                } break;
                case 2: {
                    *((uint16_t *)keymem) = *((uint16_t *)inPtr);
                    dbg_printf("val16: %u\n", *((uint16_t *)inPtr));
                    keymem += sizeof(uint16_t);
                } break;
                case 4: {
                    *((uint32_t *)keymem) = *((uint32_t *)inPtr);
                    keymem += sizeof(uint32_t);
                } break;
                case 8: {
                    *((uint64_t *)keymem) = *((uint64_t *)inPtr);
                    keymem += sizeof(uint64_t);
                } break;
                case 16: {
                    ((uint64_t *)keymem)[0] = ((uint64_t *)inPtr)[0];
                    ((uint64_t *)keymem)[1] = ((uint64_t *)inPtr)[1];
                    keymem += sizeof(uint64_t);
                } break;
                default:
                    memcpy((void *)keymem, inPtr, param->length);
            }
        }

    } else if (swap_flow) {
        // default 5-tuple aggregation for bidirectional flows
        FlowKey_t *keyptr = (FlowKey_t *)keymem;
        keymem += sizeof(FlowKey_t);

        if (ipv4Flow) {
            keyptr->srcAddr[0] = 0;
            keyptr->srcAddr[1] = ipv4Flow->dstAddr;
            keyptr->dstAddr[0] = 0;
            keyptr->dstAddr[1] = ipv4Flow->srcAddr;
        } else if (ipv6Flow) {
            keyptr->srcAddr[0] = ipv6Flow->dstAddr[0];
            keyptr->srcAddr[1] = ipv6Flow->dstAddr[1];
            keyptr->dstAddr[0] = ipv6Flow->srcAddr[0];
            keyptr->dstAddr[1] = ipv6Flow->srcAddr[1];
        }
        if (genericFlow) {
            keyptr->srcPort = genericFlow->dstPort;
            keyptr->dstPort = genericFlow->srcPort;
            keyptr->proto = genericFlow->proto;
        }
    } else {
        // default 5-tuple aggregation
        FlowKey_t *keyptr = (FlowKey_t *)keymem;
        keymem += sizeof(FlowKey_t);
        if (ipv4Flow) {
            keyptr->srcAddr[0] = 0;
            keyptr->srcAddr[1] = ipv4Flow->srcAddr;
            keyptr->dstAddr[0] = 0;
            keyptr->dstAddr[1] = ipv4Flow->dstAddr;
        } else if (ipv6Flow) {
            keyptr->srcAddr[0] = ipv6Flow->srcAddr[0];
            keyptr->srcAddr[1] = ipv6Flow->srcAddr[1];
            keyptr->dstAddr[0] = ipv6Flow->dstAddr[0];
            keyptr->dstAddr[1] = ipv6Flow->dstAddr[1];
        }
        if (genericFlow) {
            keyptr->srcPort = genericFlow->srcPort;
            keyptr->dstPort = genericFlow->dstPort;
            keyptr->proto = genericFlow->proto;
        }
    }

    return keymem;

}  // End of New_HashKey

static uint64_t null_record(FlowHashRecord_t *record, int inout) { return 0; }

static uint64_t flows_record(FlowHashRecord_t *record, int inout) { return record->counter[FLOWS]; }

static uint64_t packets_record(FlowHashRecord_t *record, int inout) {
    if (NeedSwap(GuessDirection, (FlowKey_t *)record->hashkey)) {
        if (inout == IN)
            inout = OUT;
        else if (inout == OUT)
            inout = IN;
    }
    if (inout == IN)
        return record->counter[INPACKETS];
    else if (inout == OUT)
        return record->counter[OUTPACKETS];
    else
        return record->counter[INPACKETS] + record->counter[OUTPACKETS];
}

static uint64_t bytes_record(FlowHashRecord_t *record, int inout) {
    if (NeedSwap(GuessDirection, (FlowKey_t *)record->hashkey)) {
        if (inout == IN)
            inout = OUT;
        else if (inout == OUT)
            inout = IN;
    }
    if (inout == IN)
        return record->counter[INBYTES];
    else if (inout == OUT)
        return record->counter[OUTBYTES];
    else
        return record->counter[INBYTES] + record->counter[OUTBYTES];
}

static uint64_t pps_record(FlowHashRecord_t *record, int inout) {
    /* duration in msec */
    uint64_t duration = record->msecLast - record->msecFirst;
    if (duration == 0)
        return 0;
    else {
        uint64_t packets = packets_record(record, inout);
        return (1000LL * packets) / duration;
    }
}  // End of pps_record

static uint64_t bps_record(FlowHashRecord_t *record, int inout) {
    uint64_t duration = record->msecLast - record->msecFirst;
    if (duration == 0)
        return 0;
    else {
        uint64_t bytes = bytes_record(record, inout);
        return (8000LL * bytes) / duration; /* 8 bits per Octet - x 1000 for msec */
    }
}  // End of bps_record

static uint64_t bpp_record(FlowHashRecord_t *record, int inout) {
    uint64_t packets = packets_record(record, inout);
    uint64_t bytes = bytes_record(record, inout);

    return packets ? bytes / packets : 0;
}  // End of bpp_record

static uint64_t tstart_record(FlowHashRecord_t *record, int inout) { return record->msecFirst; }  // End of tstart_record

static uint64_t tend_record(FlowHashRecord_t *record, int inout) { return record->msecLast; }  // End of tend_record

static uint64_t duration_record(FlowHashRecord_t *record, int inout) { return record->msecLast - record->msecFirst; }  // End of duration_record

static void ApplyNetMaskBits(recordHandle_t *recordHandle, struct aggregationElement_s *aggregationElement) {
    EXipv4Flow_t *ipv4Flow = (EXipv4Flow_t *)recordHandle->extensionList[EXipv4FlowID];
    EXipv6Flow_t *ipv6Flow = (EXipv6Flow_t *)recordHandle->extensionList[EXipv6FlowID];
    EXflowMisc_t *flowMisc = (EXflowMisc_t *)recordHandle->extensionList[EXflowMiscID];

    uint32_t srcMask = 0;
    uint32_t dstMask = 0;
    if (flowMisc) {
        srcMask = flowMisc->srcMask;
        dstMask = flowMisc->dstMask;
    }

    if (ipv4Flow && aggregationElement->param.extID == EXipv4FlowID) {
        if (aggregationElement->param.offset == OFFsrc4Addr) {
            uint32_t srcmask = 0xffffffff << (32 - srcMask);
            ipv4Flow->srcAddr &= srcmask;
        }
        if (aggregationElement->param.offset == OFFdst4Addr) {
            uint32_t dstmask = 0xffffffff << (32 - dstMask);
            ipv4Flow->dstAddr &= dstmask;
        }

    } else if (ipv6Flow && aggregationElement->param.extID == EXipv6FlowID) {
        if (aggregationElement->param.offset == OFFsrc6Addr) {
            uint32_t mask_bits = srcMask;
            if (mask_bits > 64) {
                uint64_t mask = 0xffffffffffffffffLL << (128 - mask_bits);
                ipv6Flow->srcAddr[1] &= mask;
            } else {
                uint64_t mask = 0xffffffffffffffffLL << (64 - mask_bits);
                ipv6Flow->srcAddr[0] &= mask;
                ipv6Flow->srcAddr[1] = 0;
            }
        }
        if (aggregationElement->param.offset == OFFdst6Addr) {
            uint32_t mask_bits = dstMask;
            if (mask_bits > 64) {
                uint64_t mask = 0xffffffffffffffffLL << (128 - mask_bits);
                ipv6Flow->dstAddr[1] &= mask;
            } else {
                uint64_t mask = 0xffffffffffffffffLL << (64 - mask_bits);
                ipv6Flow->dstAddr[0] &= mask;
                ipv6Flow->dstAddr[1] = 0;
            }
        }
    }

}  // End of ApplyNetMaskBits

static void SetNetMaskBits(EXipv4Flow_t *EXipv4Flow, EXipv6Flow_t *EXipv6Flow, EXflowMisc_t *EXflowMisc, int apply_netbits) {
    if (EXipv6Flow) {  // IPv6
        if (apply_netbits & 1) {
            uint64_t mask;
            uint32_t mask_bits = EXflowMisc->srcMask;
            if (mask_bits > 64) {
                mask = 0xffffffffffffffffLL << (128 - mask_bits);
                EXipv6Flow->srcAddr[1] &= mask;
            } else {
                mask = 0xffffffffffffffffLL << (64 - mask_bits);
                EXipv6Flow->srcAddr[0] &= mask;
                EXipv6Flow->srcAddr[1] = 0;
            }
        }
        if (apply_netbits & 2) {
            uint64_t mask;
            uint32_t mask_bits = EXflowMisc->dstMask;

            if (mask_bits > 64) {
                mask = 0xffffffffffffffffLL << (128 - mask_bits);
                EXipv6Flow->dstAddr[1] &= mask;
            } else {
                mask = 0xffffffffffffffffLL << (64 - mask_bits);
                EXipv6Flow->dstAddr[0] &= mask;
                EXipv6Flow->dstAddr[1] = 0;
            }
        }
    } else if (EXipv4Flow) {  // IPv4
        if (apply_netbits & 1) {
            uint32_t srcmask = 0xffffffff << (32 - EXflowMisc->srcMask);
            EXipv4Flow->srcAddr &= srcmask;
        }
        if (apply_netbits & 2) {
            uint32_t dstmask = 0xffffffff << (32 - EXflowMisc->dstMask);
            EXipv4Flow->dstAddr &= dstmask;
        }
    }

}  // End of SetNetMaskBits

// return a linear list of aggregated/listed flows for later sorting
static SortElement_t *GetSortList(size_t *size) {
    dbg_printf("Enter %s\n", __func__);
    SortElement_t *list;

    size_t hashSize = kh_size(FlowHash);
    if (hashSize) {  // aggregated flows in khash
        list = (SortElement_t *)calloc(hashSize, sizeof(SortElement_t));
        if (!list) {
            LogError("malloc() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno));
            *size = 0;
            return NULL;
        }

        int c = 0;
        for (khiter_t k = kh_begin(FlowHash); k != kh_end(FlowHash); ++k) {  // traverse
            if (kh_exist(FlowHash, k)) {
                FlowHashRecord_t *r = &kh_key(FlowHash, k);
                list[c++].record = (void *)r;
            }
        }
        *size = hashSize;

    } else {  // linear flow list
        size_t listSize = FlowList.NumRecords;
        if (!listSize) {
            *size = 0;
            return NULL;
        }
        list = (SortElement_t *)calloc(listSize, sizeof(SortElement_t));
        if (!list) {
            LogError("malloc() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno));
            *size = 0;
            return NULL;
        }

        FlowHashRecord_t *r = FlowList.head;
        for (int i = 0; i < listSize; i++) {
            list[i].record = (void *)r;
            r = r->next;
        }
        *size = listSize;
    }

    return list;

}  // End of GetSortList

int Init_FlowCache(void) {
    if (!nfalloc_Init(0)) return 0;

    if (!hashKeyLen) hashKeyLen = sizeof(FlowKey_t);

    FlowHash = kh_init(FlowHash);

    FlowList.head = NULL;
    FlowList.tail = &FlowList.head;
    FlowList.NumRecords = 0;

    return 1;

}  // End of Init_FlowCache

void Dispose_FlowTable(void) { nfalloc_free(); }  // End of Dispose_FlowTable

// Parse flow cache print order -O
int Parse_PrintOrder(char *order) {
    dbg_printf("Enter %s\n", __func__);
    int direction = -1;
    char *r = strchr(order, ':');
    if (r) {
        *r++ = 0;
        switch (*r) {
            case 'a':
                direction = ASCENDING;
                break;
            case 'd':
                direction = DESCENDING;
                break;
            default:
                return -1;
        }
    }

    PrintOrder = 0;
    while (order_mode[PrintOrder].string) {
        if (strcasecmp(order_mode[PrintOrder].string, order) == 0) break;
        PrintOrder++;
    }
    if (!order_mode[PrintOrder].string) {
        PrintOrder = 0;
        return -1;
    }

    PrintDirection = direction >= 0 ? direction : order_mode[PrintOrder].direction;

    return PrintOrder;

}  // End of Parse_PrintOrder

// set sort order is given by -s record - parsed in nfstat.c
// multiple sort orders may be given - each order adds the
// corresoponding bit
void Add_FlowStatOrder(uint32_t order, uint32_t direction) {
    dbg_printf("Enter %s\n", __func__);
    FlowStat_order |= order;
    PrintDirection = direction;
}  // End of Add_FlowStatOrder

char *ParseAggregateMask(char *arg, int hasGeoDB) {
    dbg_printf("Enter %s\n", __func__);
    if (bidir_flows) {
        LogError("Can not set custom aggregation in bidir mode");
        return NULL;
    }

    uint32_t elementCount = 0;
    aggregateInfo[0] = -1;

    hashKeyLen = 0;
    memset((void *)&aggregateInfo, 0, sizeof(aggregateInfo));

    size_t fmt_len = 0;
    for (int i = 0; aggregationTable[i].aggrElement != NULL; i++) {
        if (hasGeoDB && aggregationTable[i].fmt) {
            if (strcmp(aggregationTable[i].fmt, "%sa") == 0) aggregationTable[i].fmt = "%gsa";
            if (strcmp(aggregationTable[i].fmt, "%da") == 0) aggregationTable[i].fmt = "%gda";
        }
        if (aggregationTable[i].fmt) fmt_len += (strlen(aggregationTable[i].fmt) + 1);
    }
    fmt_len++;  // max fmt string len incl. trailing '\0'

    char *aggr_fmt = malloc(fmt_len);
    if (!aggr_fmt) {
        LogError("malloc() error in %s line %d: %s", __FILE__, __LINE__, strerror(errno));
        return 0;
    }
    aggr_fmt[0] = '\0';

    uint32_t has_mask = 0;
    uint32_t v4Mask = 0xffffffff;
    uint64_t v6Mask[2] = {0xffffffffffffffffLL, 0xffffffffffffffffLL};
    // separate tokens
    char *p = strtok(arg, ",");
    while (p) {
        // check for subnet bits
        char *q = strchr(p, '/');
        if (q) {
            char *n;

            *q = 0;
            uint32_t subnet = atoi(q + 1);

            // get IP version
            n = &(p[strlen(p) - 1]);
            if (*n == '4') {
                // IPv4
                if (subnet < 1 || subnet > 32) {
                    LogError("Subnet mask length '%d' out of range for IPv4", subnet);
                    return NULL;
                }
                v4Mask = 0xffffffff << (32 - subnet);
                has_mask = 1;

            } else if (*n == '6') {
                // IPv6
                if (subnet < 1 || subnet > 128) {
                    LogError("Subnet mask length '%d' out of range for IPv4", subnet);
                    return NULL;
                }

                if (subnet > 64) {
                    v6Mask[0] = 0xffffffffffffffffLL;
                    v6Mask[1] = 0xffffffffffffffffLL << (64 - subnet);
                } else {
                    v6Mask[0] = 0xffffffffffffffffLL << (64 - subnet);
                    v6Mask[1] = 0;
                }
                has_mask = 1;
            } else {
                // rubbish
                *q = '/';
                LogError("Need src4/dst4 or src6/dst6 for IPv4 or IPv6 to aggregate with explicit netmask: '%s'", p);
                return NULL;
            }
        }

        int index = 0;
        while (aggregationTable[index].aggrElement && (strcasecmp(p, aggregationTable[index].aggrElement) != 0)) index++;

        if (aggregationTable[index].aggrElement == NULL) {
            LogError("Unknown aggregation field '%s'", p);
            return NULL;
        }

        if (aggregationTable[index].active) {
            LogError("Duplicate aggregation element: %s", p);
            return NULL;
        }

        if (has_mask) {
            if (aggregationTable[index].allowMask == 0) {
                LogError("Element %s does not take any netmask", p);
                return NULL;
            }
        }

        if (aggregationTable[index].fmt != NULL) {
            strncat(aggr_fmt, aggregationTable[index].fmt, fmt_len);
            fmt_len -= strlen(aggregationTable[index].fmt);
            strncat(aggr_fmt, " ", fmt_len);
            fmt_len -= 1;
        }

        if (hasGeoDB) {
            doGeoLookup += aggregationTable[index].geoLookup;
        }

        size_t len = 0;
        do {
            // loop over alternate extensions and select longest length for hashkey
            if (has_mask) {
                maskArray[maskIndex].v4Mask = v4Mask;
                maskArray[maskIndex].v6Mask[0] = v6Mask[0];
                maskArray[maskIndex].v6Mask[1] = v6Mask[1];
                aggregationTable[index].netmaskID = maskIndex;
                maskIndex++;
            }
            aggregationTable[index].active = 1;
            aggregateInfo[elementCount++] = index;
            if (aggregationTable[index].param.length > len) len = aggregationTable[index].param.length;
            index++;
        } while (aggregationTable[index].aggrElement && (strcasecmp(p, aggregationTable[index].aggrElement) == 0));

        hashKeyLen += len;

        p = strtok(NULL, ",");
    }

    if (elementCount == 0) {
        LogError("No aggregation specified!");
        return NULL;
    }
    aggregateInfo[elementCount] = -1;

#ifdef DEVEL
    printf("Aggregate key len: %zu bytes\n", hashKeyLen);
    printf("Aggregate format string: '%s'\n", aggr_fmt);

    printf("Aggregate stack:\n");
    for (int i = 0; aggregateInfo[i] >= 0; i++) {
        int32_t index = aggregateInfo[i];
        printf("Slot: %d, Element: %s, ExtID: %u, Offset: %u, Length: %u\n", index, aggregationTable[index].aggrElement,
               aggregationTable[index].param.extID, aggregationTable[index].param.offset, aggregationTable[index].param.length);
        printf("Has IP mask: %i\n", aggregationTable[index].netmaskID);
        if (aggregationTable[index].netmaskID && aggregationTable[index].netmaskID != 0xFF) {
            uint32_t maskIndex = aggregationTable[index].netmaskID;
            printf("v4mask  : 0x%x\n", maskArray[maskIndex].v4Mask);
            printf("v6mask  : 0x%llx 0x%llx\n", maskArray[maskIndex].v6Mask[0], maskArray[maskIndex].v6Mask[1]);
        }
    }

#endif

    // XXX aggregateInfo.mask = SetAggregateMask();

    return aggr_fmt;
}  // End of ParseAggregateMask

void InsertFlow(recordHandle_t *recordHandle) {
    dbg_printf("Enter %s\n", __func__);
    EXgenericFlow_t *genericFlow = (EXgenericFlow_t *)recordHandle->extensionList[EXgenericFlowID];
    if (!genericFlow) return;

    recordHeaderV3_t *recordHeaderV3 = recordHandle->recordHeaderV3;

    FlowHashRecord_t *record = nfmalloc(sizeof(FlowHashRecord_t));
    record->flowrecord = nfmalloc(recordHeaderV3->size);
    memcpy((void *)record->flowrecord, (void *)recordHeaderV3, recordHeaderV3->size);

    record->msecFirst = genericFlow->msecFirst;
    record->msecLast = genericFlow->msecLast;

    EXcntFlow_t *cntFlow = (EXcntFlow_t *)recordHandle->extensionList[EXcntFlowID];

    record->counter[INBYTES] = genericFlow->inBytes;
    record->counter[INPACKETS] = genericFlow->inPackets;
    if (cntFlow) {
        record->counter[OUTBYTES] = cntFlow->outBytes;
        record->counter[OUTPACKETS] = cntFlow->outPackets;
        record->counter[FLOWS] = cntFlow->flows;
    } else {
        record->counter[OUTBYTES] = 0;
        record->counter[OUTPACKETS] = 0;
        record->counter[FLOWS] = 1;
    }
    record->inFlags = genericFlow->tcpFlags;
    record->outFlags = 0;
    FlowList.NumRecords++;

    record->next = NULL;
    *FlowList.tail = record;
    FlowList.tail = &(record->next);

}  // End of InsertFlow

static void AddBidirFlow(recordHandle_t *recordHandle) {
    dbg_printf("Enter %s\n", __func__);
    recordHeaderV3_t *record = recordHandle->recordHeaderV3;
    EXgenericFlow_t *genericFlow = (EXgenericFlow_t *)recordHandle->extensionList[EXgenericFlowID];
    EXcntFlow_t *cntFlow = (EXcntFlow_t *)recordHandle->extensionList[EXcntFlowID];
    uint64_t inPackets = genericFlow->inPackets;
    uint64_t inBytes = genericFlow->inBytes;
    uint64_t outBytes = 0;
    uint64_t outPackets = 0;
    uint64_t aggrFlows = 1;
    if (cntFlow) {
        outPackets = cntFlow->outPackets;
        outBytes = cntFlow->outPackets;
        aggrFlows = cntFlow->flows;
    }

    static void *keymem = NULL;
    static void *bidirkeymem = NULL;
    FlowHashRecord_t r;

    if (keymem == NULL) {
        keymem = nfmalloc(hashKeyLen);
    }
    New_HashKey(keymem, recordHandle, 0);
    uint32_t forwardHash = SuperFastHash(keymem, hashKeyLen);
    r.hashkey = keymem;
    r.hash = forwardHash;

    int ret;
    khiter_t k = kh_get(FlowHash, FlowHash, r);
    if (k != kh_end(FlowHash)) {
        // flow record found - best case! update all fields
        kh_key(FlowHash, k).counter[INBYTES] += inBytes;
        kh_key(FlowHash, k).counter[INPACKETS] += inPackets;
        kh_key(FlowHash, k).counter[OUTBYTES] += outBytes;
        kh_key(FlowHash, k).counter[OUTPACKETS] += outPackets;
        kh_key(FlowHash, k).inFlags |= genericFlow->tcpFlags;

        if (genericFlow->msecFirst < kh_key(FlowHash, k).msecFirst) {
            kh_key(FlowHash, k).msecFirst = genericFlow->msecFirst;
        }
        if (genericFlow->msecLast > kh_key(FlowHash, k).msecLast) {
            kh_key(FlowHash, k).msecLast = genericFlow->msecLast;
        }

        kh_key(FlowHash, k).counter[FLOWS] += aggrFlows;
    } else if (genericFlow->proto != IPPROTO_TCP && genericFlow->proto != IPPROTO_UDP) {
        // no flow record found and no TCP/UDP bidir flows. Insert flow record into hash
        k = kh_put(FlowHash, FlowHash, r, &ret);
        kh_key(FlowHash, k).counter[INBYTES] = inBytes;
        kh_key(FlowHash, k).counter[INPACKETS] = inPackets;
        kh_key(FlowHash, k).counter[OUTBYTES] = outBytes;
        kh_key(FlowHash, k).counter[OUTPACKETS] = outPackets;
        kh_key(FlowHash, k).counter[FLOWS] = aggrFlows;
        kh_key(FlowHash, k).inFlags = genericFlow->tcpFlags;
        kh_key(FlowHash, k).outFlags = 0;

        kh_key(FlowHash, k).msecFirst = genericFlow->msecFirst;
        kh_key(FlowHash, k).msecLast = genericFlow->msecLast;

        void *p = malloc(record->size);
        if (!p) {
            LogError("malloc() error in %s line %d: %s", __FILE__, __LINE__, strerror(errno));
            exit(255);
        }
        memcpy((void *)p, record, record->size);
        kh_key(FlowHash, k).flowrecord = p;

        // keymen got part of the cache
        keymem = NULL;
    } else {
        // for bidir flows do

        // generate reverse hash key to search for bidir flow
        // we need it only to lookup
        if (bidirkeymem == NULL) {
            bidirkeymem = nfmalloc(hashKeyLen);
        }

        // generate the hash key for reverse record (bidir)
        New_HashKey(bidirkeymem, recordHandle, 1);
        r.hashkey = bidirkeymem;
        r.hash = SuperFastHash(bidirkeymem, hashKeyLen);

        k = kh_get(FlowHash, FlowHash, r);
        if (k != kh_end(FlowHash)) {
            // we found a corresponding flow - so update all fields in reverse direction
            kh_key(FlowHash, k).counter[OUTBYTES] += inBytes;
            kh_key(FlowHash, k).counter[OUTPACKETS] += inPackets;
            kh_key(FlowHash, k).counter[INBYTES] += outBytes;
            kh_key(FlowHash, k).counter[INPACKETS] += outPackets;
            kh_key(FlowHash, k).outFlags |= genericFlow->tcpFlags;

            if (genericFlow->msecFirst < kh_key(FlowHash, k).msecFirst) {
                kh_key(FlowHash, k).msecFirst = genericFlow->msecFirst;
            }
            if (genericFlow->msecLast > kh_key(FlowHash, k).msecLast) {
                kh_key(FlowHash, k).msecLast = genericFlow->msecLast;
            }

            kh_key(FlowHash, k).counter[FLOWS] += aggrFlows;
        } else {
            // no bidir flow found
            // insert original flow into the cache
            r.hashkey = keymem;
            r.hash = forwardHash;
            k = kh_put(FlowHash, FlowHash, r, &ret);
            kh_key(FlowHash, k).counter[INBYTES] = inBytes;
            kh_key(FlowHash, k).counter[INPACKETS] = inPackets;
            kh_key(FlowHash, k).counter[OUTBYTES] = outBytes;
            kh_key(FlowHash, k).counter[OUTPACKETS] = outPackets;
            kh_key(FlowHash, k).counter[FLOWS] = aggrFlows;
            kh_key(FlowHash, k).inFlags = genericFlow->tcpFlags;
            kh_key(FlowHash, k).outFlags = 0;

            kh_key(FlowHash, k).msecFirst = genericFlow->msecFirst;
            kh_key(FlowHash, k).msecLast = genericFlow->msecLast;

            void *p = malloc(record->size);
            if (!p) {
                LogError("malloc() error in %s line %d: %s", __FILE__, __LINE__, strerror(errno));
                exit(255);
            }
            memcpy((void *)p, record, record->size);
            kh_key(FlowHash, k).flowrecord = p;

            // keymen got part of the cache
            keymem = NULL;
        }
    }

}  // End of AddBidirFlow

void AddFlowCache(recordHandle_t *recordHandle) {
    dbg_printf("Enter %s\n", __func__);
    EXgenericFlow_t *genericFlow = (EXgenericFlow_t *)recordHandle->extensionList[EXgenericFlowID];
    if (!genericFlow) return;

    EXcntFlow_t *cntFlow = (EXcntFlow_t *)recordHandle->extensionList[EXcntFlowID];
    uint64_t inPackets = genericFlow->inPackets;
    uint64_t inBytes = genericFlow->inBytes;
    uint64_t outBytes = 0;
    uint64_t outPackets = 0;
    uint64_t aggrFlows = 1;
    if (cntFlow) {
        outPackets = cntFlow->outPackets;
        outBytes = cntFlow->outPackets;
        aggrFlows = cntFlow->flows;
    }

    recordHeaderV3_t *record = recordHandle->recordHeaderV3;
    static void *keymem = NULL;
    FlowHashRecord_t r;

    if (doGeoLookup && TestFlag(record->flags, V3_FLAG_ENRICHED) == 0) {
        EXipv4Flow_t *ipv4Flow = (EXipv4Flow_t *)recordHandle->extensionList[EXipv4FlowID];
        EXipv6Flow_t *ipv6Flow = (EXipv6Flow_t *)recordHandle->extensionList[EXipv6FlowID];
        EXasRouting_t *asRouting = (EXasRouting_t *)recordHandle->extensionList[EXasRoutingID];
        if (ipv4Flow) {
            LookupV4Country(ipv4Flow->srcAddr, &recordHandle->geo[0]);
            LookupV4Country(ipv4Flow->dstAddr, &recordHandle->geo[2]);
            if (asRouting) {
                if (asRouting->srcAS == 0) asRouting->srcAS = LookupV4AS(ipv4Flow->srcAddr);
                if (asRouting->dstAS == 0) asRouting->dstAS = LookupV4AS(ipv4Flow->dstAddr);
            }
        }
        if (ipv6Flow) {
            LookupV6Country(ipv6Flow->srcAddr, &recordHandle->geo[0]);
            LookupV6Country(ipv6Flow->dstAddr, &recordHandle->geo[2]);
            if (asRouting) {
                if (asRouting->srcAS == 0) asRouting->srcAS = LookupV6AS(ipv6Flow->srcAddr);
                if (asRouting->dstAS == 0) asRouting->dstAS = LookupV6AS(ipv6Flow->dstAddr);
            }
        }
        SetFlag(record->flags, V3_FLAG_ENRICHED);
    }
    if (bidir_flows) return AddBidirFlow(recordHandle);

    if (keymem == NULL) {
        keymem = nfmalloc(hashKeyLen);
    }
    void *endOfKey = New_HashKey(keymem, recordHandle, 0);
    dbg_printf("Key diff: %zu, len: %zu\n", endOfKey - keymem, hashKeyLen);
    /*
if ((keymem + hashKeyLen) < (endOfKey)) {
} else {

}
    */
    r.hashkey = keymem;
    r.hash = SuperFastHash(keymem, hashKeyLen);

    int ret;
    khiter_t k = kh_put(FlowHash, FlowHash, r, &ret);
    if (ret == 0) {
        // flow record found - best case! update all fields
        kh_key(FlowHash, k).counter[INBYTES] += inBytes;
        kh_key(FlowHash, k).counter[INPACKETS] += inPackets;
        kh_key(FlowHash, k).counter[OUTBYTES] += outBytes;
        kh_key(FlowHash, k).counter[OUTPACKETS] += outPackets;
        kh_key(FlowHash, k).inFlags |= genericFlow->tcpFlags;

        if (genericFlow->msecFirst < kh_key(FlowHash, k).msecFirst) {
            kh_key(FlowHash, k).msecFirst = genericFlow->msecFirst;
        }
        if (genericFlow->msecLast > kh_key(FlowHash, k).msecLast) {
            kh_key(FlowHash, k).msecLast = genericFlow->msecLast;
        }

        kh_key(FlowHash, k).counter[FLOWS] += aggrFlows;
    } else {
        // no flow record found and no TCP/UDP bidir flows. Insert flow record into hash
        kh_key(FlowHash, k).counter[INBYTES] = inBytes;
        kh_key(FlowHash, k).counter[INPACKETS] = inPackets;
        kh_key(FlowHash, k).counter[OUTBYTES] = outBytes;
        kh_key(FlowHash, k).counter[OUTPACKETS] = outPackets;
        kh_key(FlowHash, k).counter[FLOWS] = aggrFlows;
        kh_key(FlowHash, k).inFlags = genericFlow->tcpFlags;
        kh_key(FlowHash, k).outFlags = 0;

        kh_key(FlowHash, k).msecFirst = genericFlow->msecFirst;
        kh_key(FlowHash, k).msecLast = genericFlow->msecLast;

        void *p = nfmalloc(record->size);
        memcpy((void *)p, record, record->size);
        kh_key(FlowHash, k).flowrecord = p;

        // keymen got part of the cache
        keymem = NULL;
    }

}  // End of AddFlow

// print SortList - apply possible aggregation mask to zero out aggregated fields
static inline void PrintSortList(SortElement_t *SortList, uint32_t maxindex, outputParams_t *outputParams, int GuessFlowDirection,
                                 RecordPrinter_t print_record, int ascending) {
    dbg_printf("Enter %s\n", __func__);
    int max = maxindex;
    if (outputParams->topN && outputParams->topN < maxindex) max = outputParams->topN;
    for (int i = 0; i < max; i++) {
        int j = ascending ? i : maxindex - 1 - i;

        FlowHashRecord_t *r = (FlowHashRecord_t *)(SortList[j].record);
        recordHeaderV3_t *v3record = (r->flowrecord);

        recordHandle_t recordHandle = {0};
        EXcntFlow_t tmpCntFlow = {0};
        MapRecordHandle(&recordHandle, v3record, i);
        EXgenericFlow_t *genericFlow = (EXgenericFlow_t *)recordHandle.extensionList[EXgenericFlowID];
        EXipv4Flow_t *ipv4Flow = (EXipv4Flow_t *)recordHandle.extensionList[EXipv4FlowID];
        EXipv6Flow_t *ipv6Flow = (EXipv6Flow_t *)recordHandle.extensionList[EXipv6FlowID];
        EXcntFlow_t *cntFlow = (EXcntFlow_t *)recordHandle.extensionList[EXcntFlowID];
        EXasRouting_t *asRouting = (EXasRouting_t *)recordHandle.extensionList[EXasRoutingID];

        if (doGeoLookup) {
            if (ipv4Flow) {
                LookupV4Country(ipv4Flow->srcAddr, recordHandle.geo);
                LookupV4Country(ipv4Flow->dstAddr, &recordHandle.geo[2]);
            }
            if (ipv6Flow) {
                LookupV6Country(ipv6Flow->srcAddr, recordHandle.geo);
                LookupV6Country(ipv6Flow->dstAddr, &recordHandle.geo[2]);
            }
            if (asRouting) {
                if (asRouting->srcAS == 0) asRouting->srcAS = ipv4Flow ? LookupV4AS(ipv4Flow->srcAddr) : LookupV6AS(ipv6Flow->srcAddr);
                if (asRouting->dstAS == 0) asRouting->dstAS = ipv4Flow ? LookupV4AS(ipv4Flow->dstAddr) : LookupV6AS(ipv6Flow->dstAddr);
            }
            SetFlag(v3record->flags, V3_FLAG_ENRICHED);
        }
        genericFlow->inPackets = r->counter[INPACKETS];
        genericFlow->inBytes = r->counter[INBYTES];
        genericFlow->msecFirst = r->msecFirst;
        genericFlow->msecLast = r->msecLast;
        genericFlow->tcpFlags = r->inFlags;
        if (r->counter[OUTPACKETS]) {
            if (cntFlow) {
                cntFlow->outPackets = r->counter[OUTPACKETS];
                cntFlow->outBytes = r->counter[OUTBYTES];
                cntFlow->flows = r->counter[FLOWS];
            } else {
                recordHandle.extensionList[EXcntFlowID] = &tmpCntFlow;
                tmpCntFlow.outPackets = r->counter[OUTPACKETS];
                tmpCntFlow.outBytes = r->counter[OUTBYTES];
                tmpCntFlow.flows = r->counter[FLOWS];
            }
        }

        /*
        // XXX flow_record.revTcpFlags = r->outFlags;

        // apply IP mask from aggregation, to provide a pretty output
        if (aggregateInfo.has_masks) {
            flow_record.V6.srcaddr[0] &= aggregateInfo.IPmask[0];
            flow_record.V6.srcaddr[1] &= aggregateInfo.IPmask[1];
            flow_record.V6.dstaddr[0] &= aggregateInfo.IPmask[2];
            flow_record.V6.dstaddr[1] &= aggregateInfo.IPmask[3];
        }

        if (aggregateInfo.apply_netbits) ApplyNetMaskBits(&flow_record, aggregateInfo.apply_netbits);

        if (aggr_record_mask) ApplyAggrMask(&flow_record, aggr_record_mask);

        if (NeedSwap(GuessFlowDirection, &flow_record)) SwapFlow(&flow_record);
        */

        print_record(stdout, &recordHandle, outputParams->doTag);
    }

}  // End of PrintSortList

// export SortList - apply possible aggregation mask to zero out aggregated fields
static inline void ExportSortList(SortElement_t *SortList, uint32_t maxindex, nffile_t *nffile, int GuessFlowDirection, int ascending) {
    dbg_printf("Enter %s\n", __func__);
    for (int i = 0; i < maxindex; i++) {
        int j = ascending ? i : maxindex - 1 - i;

        FlowHashRecord_t *r = (FlowHashRecord_t *)(SortList[j].record);

        recordHeaderV3_t *recordHeaderV3 = (r->flowrecord);

        // check, if we need cntFlow extension
        int exCntSize = 0;
        if (r->counter[OUTPACKETS] || r->counter[OUTBYTES] || r->counter[FLOWS] != 1) {
            exCntSize = EXcntFlowSize;
        }

        if (!CheckBufferSpace(nffile, recordHeaderV3->size + exCntSize)) {
            return;
        }

        // write record
        memcpy(nffile->buff_ptr, (void *)recordHeaderV3, recordHeaderV3->size);
        // remap header to written memory
        recordHeaderV3 = nffile->buff_ptr;

        recordHandle_t recordHandle = {0};
        MapRecordHandle(&recordHandle, recordHeaderV3, i);

        // check if cntFlow already exists
        EXcntFlow_t *cntFlow = (EXcntFlow_t *)recordHandle.extensionList[EXcntFlowID];

        if (cntFlow == NULL && exCntSize) {
            PushExtension(recordHeaderV3, EXcntFlow, extPtr);
            cntFlow = extPtr;
            nffile->buff_ptr += recordHeaderV3->size;
            nffile->block_header->size += recordHeaderV3->size;
        } else {
            nffile->buff_ptr += recordHeaderV3->size;
            nffile->block_header->size += recordHeaderV3->size;
        }
        nffile->block_header->NumRecords++;

        EXgenericFlow_t *genericFlow = (EXgenericFlow_t *)recordHandle.extensionList[EXgenericFlowID];
        if (genericFlow) {
            genericFlow->inPackets = r->counter[INPACKETS];
            genericFlow->inBytes = r->counter[INBYTES];
            genericFlow->msecFirst = r->msecFirst;
            genericFlow->msecLast = r->msecLast;
            genericFlow->tcpFlags = r->inFlags;
        }
        if (cntFlow) {
            cntFlow->outPackets = r->counter[OUTPACKETS];
            cntFlow->outBytes = r->counter[OUTBYTES];
            cntFlow->flows = r->counter[FLOWS];
        }

        EXipv4Flow_t *ipv4Flow = (EXipv4Flow_t *)recordHandle.extensionList[EXipv4FlowID];
        EXipv6Flow_t *ipv6Flow = (EXipv6Flow_t *)recordHandle.extensionList[EXipv6FlowID];
        /* XXX
        // mask already applied when storing flow
        // apply IP mask from aggregation, to provide a pretty output
        if (aggregateInfo.has_masks) {
            if (ipv4Flow) {
                ipv4Flow->srcAddr &= aggregateInfo.srcV4Mask;
                ipv4Flow->dstAddr &= aggregateInfo.dstV4Mask;
            } else if (ipv6Flow) {
                ipv6Flow->srcAddr[0] &= aggregateInfo.srcV6Mask[0];
                ipv6Flow->srcAddr[1] &= aggregateInfo.srcV6Mask[1];
                ipv6Flow->dstAddr[0] &= aggregateInfo.dstV6Mask[0];
                ipv6Flow->dstAddr[1] &= aggregateInfo.dstV6Mask[1];
            }
        }
        */

        EXflowMisc_t *flowMisc = (EXflowMisc_t *)recordHandle.extensionList[EXflowMiscID];
        if (flowMisc) {
            flowMisc->revTcpFlags = r->outFlags;
            // XXX if (aggregateInfo.apply_netbits) SetNetMaskBits(ipv4Flow, ipv6Flow, flowMisc, aggregateInfo.apply_netbits);
        }
        EXasRouting_t *asRouting = (EXasRouting_t *)recordHandle.extensionList[EXasRoutingID];

        int needSwap = NeedSwap(GuessFlowDirection, genericFlow);
        if (needSwap) {
            SwapRawFlow(genericFlow, ipv4Flow, ipv6Flow, flowMisc, cntFlow, asRouting);
        }

        // Update statistics
        UpdateRawStat(nffile->stat_record, genericFlow, cntFlow);
    }

}  // End of ExportSortList

int SetBidirAggregation(void) {
    dbg_printf("Enter %s\n", __func__);
    if (aggregateInfo[0] != -1) {
        LogError("Can not set bidir mode with custom aggregation mask");
        return 0;
    }

    bidir_flows = 1;

    return 1;

}  // End of SetBidirAggregation

// print -s record/xx statistics with as many print orders as required
void PrintFlowStat(RecordPrinter_t print_record, outputParams_t *outputParams) {
    dbg_printf("Enter %s\n", __func__);
    size_t maxindex;

    // Get sort array
    SortElement_t *SortList = GetSortList(&maxindex);
    if (!SortList) {
        return;
    }

    // process all the remaining stats, if requested
    for (int order_index = 0; order_mode[order_index].string != NULL; order_index++) {
        unsigned int order_bit = 1 << order_index;
        if (FlowStat_order & order_bit) {
            for (int i = 0; i < maxindex; i++) {
                FlowHashRecord_t *r = (FlowHashRecord_t *)(SortList[i].record);
                /* if we have some different sort orders, which are not directly available in the FlowHashRecord_t
                 * we need to calculate this value first - such as bpp, bps etc.
                 */
                SortList[i].count = order_mode[order_index].record_function(r, order_mode[order_index].inout);
            }

            int direction = PrintDirection;
            if (maxindex > 2) {
                if (maxindex < 100) {
                    heapSort(SortList, maxindex, outputParams->topN, PrintDirection);
                    direction = 0;
                } else {
                    blocksort((SortRecord_t *)SortList, maxindex);
                }
            }
            if (!outputParams->quiet) {
                if (outputParams->mode == MODE_PLAIN) {
                    if (outputParams->topN != 0)
                        printf("Top %i flows ordered by %s:\n", outputParams->topN, order_mode[order_index].string);
                    else
                        printf("Top flows ordered by %s:\n", order_mode[order_index].string);
                }
            }
            PrintProlog(outputParams);
            PrintSortList(SortList, maxindex, outputParams, 0, print_record, direction);
        }
    }

}  // End of PrintFlowStat

// print Flow cache
void PrintFlowTable(RecordPrinter_t print_record, outputParams_t *outputParams, int GuessDir) {
    dbg_printf("Enter %s\n", __func__);
    GuessDirection = GuessDir;

    size_t maxindex;
    SortElement_t *SortList = GetSortList(&maxindex);
    if (!SortList) return;

    if (PrintOrder) {
        // for any -O print mode
        for (int i = 0; i < maxindex; i++) {
            FlowHashRecord_t *r = (FlowHashRecord_t *)(SortList[i].record);
            SortList[i].count = order_mode[PrintOrder].record_function(r, order_mode[PrintOrder].inout);
        }

        if (maxindex >= 2) {
            if (maxindex < 100) {
                heapSort(SortList, maxindex, 0, PrintDirection);
                PrintDirection = 0;
            } else {
                blocksort((SortRecord_t *)SortList, maxindex);
            }
        }

        PrintSortList(SortList, maxindex, outputParams, GuessDir, print_record, PrintDirection);
    } else {
        // for -a and no -O sorting required
        PrintSortList(SortList, maxindex, outputParams, GuessDir, print_record, PrintDirection);
    }
}  // End of PrintFlowTable

int ExportFlowTable(nffile_t *nffile, int aggregate, int bidir, int GuessDir) {
    dbg_printf("Enter %s\n", __func__);
    GuessDirection = GuessDir;

    ExportExporterList(nffile);

    size_t maxindex;
    SortElement_t *SortList = GetSortList(&maxindex);
    if (!SortList) return 0;

    if (PrintOrder) {
        // for any -O print mode
        for (int i = 0; i < maxindex; i++) {
            FlowHashRecord_t *r = (FlowHashRecord_t *)(SortList[i].record);
            SortList[i].count = order_mode[PrintOrder].record_function(r, order_mode[PrintOrder].inout);
        }

        if (maxindex >= 2) {
            if (maxindex < 100) {
                heapSort(SortList, maxindex, 0, PrintDirection);
                PrintDirection = 0;
            } else {
                blocksort((SortRecord_t *)SortList, maxindex);
            }
        }

        ExportSortList(SortList, maxindex, nffile, GuessDir, PrintDirection);
    } else {
        ExportSortList(SortList, maxindex, nffile, GuessDir, PrintDirection);
    }

    if (nffile->block_header->NumRecords) {
        if (WriteBlock(nffile) <= 0) {
            LogError("Failed to write output buffer to disk: '%s'", strerror(errno));
            return 0;
        }
    }

    return 1;

}  // End of ExportFlowTable
