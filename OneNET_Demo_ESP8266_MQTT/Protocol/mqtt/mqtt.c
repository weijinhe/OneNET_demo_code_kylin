#include "mqtt.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <stdio.h>


#define CMD_TOPIC_PREFIX "$SYS/cmdreq/"
#define CMD_TOPIC_PREFIX_LEN 12 // strlen(CMD_TOPIC_PREFIX)
#define FORMAT_TIME_STRING_SIZE 23

// range of int: (-2147483648  2147483648), and 1 byte for terminating null byte.
#define MAX_INTBUF_SIZE 12
#define MAX_DBLBUF_SIZE 320

#ifdef WIN32
#define snprintf _snprintf
#endif

static const char Mqtt_TrailingBytesForUTF8[256] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5
};

struct DataPointPktInfo
{
    int16_t tag;
    int16_t subobj_depth;
};

static const int16_t DATA_POINT_PKT_TAG = 0xc19c;

/**
 * 封装发布确认数据包
 * @param buf 存储数据包的缓冲区对象
 * @param pkt_id 被确认的发布数据数据包的ID
 * @return 成功返回MQTTERR_NOERROR
 */
static int Mqtt_PackPubAckPkt(struct MqttBuffer *buf, uint16_t pkt_id);
/**
 * 封装已接收数据包
 * @param buf 存储数据包的缓冲区对象
 * @param pkt_id 被确认的发布数据(Publish)数据包的ID
 * @return 成功返回MQTTERR_NOERROR
 */
static int Mqtt_PackPubRecPkt(struct MqttBuffer *buf, uint16_t pkt_id);
/**
 * 封装发布数据释放数据包
 * @param buf 存储数据包的缓冲区对象
 * @param pkt_id 被确认的已接收数据包(PubRec)的ID
 * @return 成功返回MQTTERR_NOERROR
 */
static int Mqtt_PackPubRelPkt(struct MqttBuffer *buf, uint16_t pkt_id);
/**
 * 封装发布数据完成数据包
 * @param buf 存储数据包的缓冲区对象
 * @param pkt_id 被确认的发布数据释放数据包(PubRel)的ID
 * @return 成功返回MQTTERR_NOERROR
 */
static int Mqtt_PackPubCompPkt(struct MqttBuffer *buf, uint16_t pkt_id);

uint16_t Mqtt_RB16(const char *v)
{
    const uint8_t *uv = (const uint8_t*)v;
    return (((uint16_t)(uv[0])) << 8) | uv[1];
}

uint64_t Mqtt_RB64(const char *v)
{
    const uint8_t *uv = (const uint8_t*)v;
    return ((((uint64_t)(uv[0])) << 56) |
            (((uint64_t)(uv[1])) << 48) |
            (((uint64_t)(uv[2])) << 40) |
            (((uint64_t)(uv[3])) << 32) |
            (((uint64_t)(uv[4])) << 24) |
            (((uint64_t)(uv[5])) << 16) |
            (((uint64_t)(uv[6])) << 8)  |
            uv[7]);

}

void Mqtt_WB16(uint16_t v, char *out)
{
    uint8_t *uo = (uint8_t*)out;
    uo[0] = (uint8_t)(v >> 8);
    uo[1] = (uint8_t)(v);
}

void Mqtt_WB32(uint32_t v, char *out)
{
    uint8_t *uo = (uint8_t*)out;
    uo[0] = (uint8_t)(v >> 24);
    uo[1] = (uint8_t)(v >> 16);
    uo[2] = (uint8_t)(v >> 8);
    uo[3] = (uint8_t)(v);
}

int Mqtt_ReadLength(const char *stream, int size, uint32_t *len)
{
    int i;
    const uint8_t *in = (const uint8_t*)stream;
    uint32_t multiplier = 1;

    *len = 0;
    for(i = 0; i < size; ++i)
    {
        *len += (in[i] & 0x7f) * multiplier;

        if(!(in[i] & 0x80))
        {
            return i + 1;
        }

        multiplier *= 128;
        if(multiplier >= 128 * 128 * 128)
        {
            return -2; // error, out of range
        }
    }

    return -1; // not complete
}

int Mqtt_DumpLength(size_t len, char *buf)
{
    int i;
    for(i = 1; i <= 4; ++i)
    {
        *((uint8_t*)buf) = len % 128;
        len /= 128;
        if(len > 0)
        {
            *buf |= 128;
            ++buf;
        }
        else
        {
            return i;
        }
    }

    return -1;
}
int Mqtt_AppendLength(struct MqttBuffer *buf, uint32_t len)
{
    struct MqttExtent *fix_head = buf->first_ext;
    uint32_t pkt_len;

    // assert(fix_head);

    if(Mqtt_ReadLength(fix_head->payload + 1, 4, &pkt_len) < 0)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    pkt_len += len;

    fix_head->len = Mqtt_DumpLength(pkt_len, fix_head->payload + 1) + 1;
    if(fix_head->len < 2)
    {
        return MQTTERR_PKT_TOO_LARGE;
    }

    return MQTTERR_NOERROR;
}

int Mqtt_EraseLength(struct MqttBuffer *buf, uint32_t len)
{
    struct MqttExtent *fix_head = buf->first_ext;
    uint32_t pkt_len;

    // assert(fix_head);

    if(Mqtt_ReadLength(fix_head->payload + 1, 4, &pkt_len) < 0)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    if(pkt_len < len)
    {
        // critical bug
        return MQTTERR_INTERNAL;
    }

    pkt_len -= len;
    buf->buffered_bytes -= len;

    fix_head->len = Mqtt_DumpLength(pkt_len, fix_head->payload + 1) + 1;
    // assert(fix_head->len >= 2);

    return MQTTERR_NOERROR;
}

void Mqtt_PktWriteString(char **buf, const char *str, uint16_t len)
{
    Mqtt_WB16(len, *buf);
    memcpy(*buf + 2, str, len);
    *buf += 2 + len;
}

int Mqtt_CheckClentIdentifier(const char *id)
{
    int len;
    for(len = 0; '\0' != id[len]; ++len)
    {
        if(!isalnum(id[len]))
        {
            return -1;
        }
    }

    return len;
}

static int Mqtt_IsLegalUtf8(const char *first, int len)
{
    unsigned char bv;
    const unsigned char *tail = (const unsigned char *)(first + len);

    switch(len)
    {
        default:
            return MQTTERR_NOT_UTF8;

        case 4:
            bv = *(--tail);
            if((bv < 0x80) || (bv > 0xBF))
            {
                return MQTTERR_NOT_UTF8;
            }
        case 3:
            bv = *(--tail);
            if((bv < 0x80) || (bv > 0xBF))
            {
                return MQTTERR_NOT_UTF8;
            }
        case 2:
            bv = *(--tail);
            if((bv < 0x80) || (bv > 0xBF))
            {
                return MQTTERR_NOT_UTF8;
            }
            switch(*(const unsigned char *)first)
            {
                case 0xE0:
                    if(bv < 0xA0)
                    {
                        return MQTTERR_NOT_UTF8;
                    }
                    break;

                case 0xED:
                    if(bv > 0x9F)
                    {
                        return MQTTERR_NOT_UTF8;
                    }
                    break;

                case 0xF0:
                    if(bv < 0x90)
                    {
                        return MQTTERR_NOT_UTF8;
                    }
                    break;

                case 0xF4:
                    if(bv > 0x8F)
                    {
                        return MQTTERR_NOT_UTF8;
                    }
                    break;

                default:
                    break;
            }
        case 1:
            if(((*first >= 0x80) && (*first < 0xC2)) || (*first > 0xF4))
            {
                return MQTTERR_NOT_UTF8;
            }
    }

    return MQTTERR_NOERROR;
}

static int Mqtt_CheckUtf8(const char *str, size_t len)
{
    size_t i;

    for(i = 0; i < len;)
    {
        int ret;
        char utf8_char_len = Mqtt_TrailingBytesForUTF8[(uint8_t)str[i]] + 1;

        if(i + utf8_char_len > len)
        {
            return MQTTERR_NOT_UTF8;
        }

        ret = Mqtt_IsLegalUtf8(str, utf8_char_len);
        if(ret != MQTTERR_NOERROR)
        {
            return ret;
        }

        i += utf8_char_len;
        if('\0' == str[i])
        {
            break;
        }
    }

    return (int)i;
}

struct DataPointPktInfo *Mqtt_GetDataPointPktInfo(struct MqttBuffer *buf)
{
    struct MqttExtent *fix_head = buf->first_ext;
    struct MqttExtent *first_payload;
    struct DataPointPktInfo *info;

    if(!fix_head)
    {
        return NULL;
    }

    if(MQTT_PKT_PUBLISH != (((uint8_t)(fix_head->payload[0])) >> 4))
    {
        return NULL;
    }

    if(!(fix_head->next) || !(first_payload = fix_head->next->next) ||
       (MQTT_DPTYPE_TRIPLE != first_payload->payload[0]))
    {
        return NULL;
    }

    if(first_payload->len != 2 + sizeof(struct DataPointPktInfo))
    {
        return NULL;
    }

    info = (struct DataPointPktInfo*)(first_payload->payload + 2);
    if(DATA_POINT_PKT_TAG != info->tag)
    {
        return NULL;
    }

    return info;
}

int Mqtt_HasIllegalCharacter(const char *str, size_t len)
{
    // TODO:
    return 0;
}

int Mqtt_FormatTime(int64_t ts, char *out)
{
    int64_t millisecond = ts % 1000;
    struct tm *t;
    time_t tt = (time_t)(ts) / 1000;
    t = gmtime(&tt);
    if(!t)
    {
        return 0;
    }

    if(0 == strftime(out, 24, "%Y-%m-%d %H:%M:%S", t))
    {
        return 0;
    }

    sprintf(out + 19, ".%03d", (int)millisecond); // 19 bytes for %Y-%m-%dT%H:%M:%S
    return FORMAT_TIME_STRING_SIZE;
}

int Mqtt_HandlePingResp(struct MqttContext *ctx, char flags,
                        char *pkt, size_t size)
{
    if((0 != flags) || (0 != size))
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    return ctx->handle_ping_resp(ctx->handle_ping_resp_arg);
}

int Mqtt_HandleConnAck(struct MqttContext *ctx, char flags,
                       char *pkt, size_t size)
{
    char ack_flags, ret_code;
    ack_flags = pkt[0];
    ret_code = pkt[1];
    if((0 != flags) || (2 != size))
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    ack_flags = pkt[0];
    ret_code = pkt[1];
    if(((ack_flags & 0x01) && (0 != ret_code)) || (ret_code > 5))
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    return ctx->handle_conn_ack(ctx->handle_conn_ack_arg, ack_flags, ret_code);
}
extern struct MqttSampleContext smpctx[1];
static int Mqtt_HandlePublish(struct MqttContext *ctx, char flags,
                              char *pkt, size_t size)
{
    const char dup = flags & 0x08;
    const char qos = ((uint8_t)flags & 0x06) >> 1;
    const char retain = flags & 0x01;
    uint16_t topic_len, pkt_id = 0;
    size_t payload_len;
    char *payload;
    char *topic, *cursor;
    int err = MQTTERR_NOERROR;

    if(size < 2)
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    if(retain)
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    topic_len = Mqtt_RB16(pkt);
    if(size < (size_t)(2 + topic_len))
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    switch(qos)
    {
        case MQTT_QOS_LEVEL0: // qos0 have no packet identifier
            if(0 != dup)
            {
                return MQTTERR_ILLEGAL_PKT;
            }

            memmove(pkt, pkt + 2, topic_len); // reuse the space to store null terminate
            topic = pkt;

            payload_len = size - 2 - topic_len;
            payload = pkt + 2 + topic_len;
            break;

        case MQTT_QOS_LEVEL1:
        case MQTT_QOS_LEVEL2:
            topic = pkt + 2;
            if(topic_len + 4 > size)
            {
                return MQTTERR_ILLEGAL_PKT;
            }

            pkt_id = Mqtt_RB16(pkt + topic_len + 2);
            if(0 == pkt_id)
            {
                return MQTTERR_ILLEGAL_PKT;
            }
            payload_len = size - 4 - topic_len;
            payload = pkt + 4 + topic_len;
            break;

        default:
            return MQTTERR_ILLEGAL_PKT;
    }

    //assert(NULL != topic);
    topic[topic_len] = '\0';

    if(Mqtt_CheckUtf8(topic, topic_len) != topic_len)
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    cursor = topic;
    while('\0' != *cursor)
    {
        if(('+' == *cursor) || ('#' == *cursor))
        {
            return MQTTERR_ILLEGAL_PKT;
        }
        ++cursor;
    }

    if('$' == *topic)
    {
        if(topic == strstr(topic, CMD_TOPIC_PREFIX))
        {
            const char *cmdid = topic + CMD_TOPIC_PREFIX_LEN; // skip the $CMDREQ
            char *arg = payload + 1;
            size_t arg_len = payload_len - 1;
            int64_t ts = 0;
            char *desc = "";

            if((payload_len < 1) || ((*payload & 0x1f) != 0x5))
            {
                return MQTTERR_ILLEGAL_PKT;
            }

            if(*payload & 0x40)
            {
                if(arg_len < 8)
                {
                    return MQTTERR_ILLEGAL_PKT;
                }
                ts = (int64_t)Mqtt_RB64(arg);
                arg += 8;
                arg_len -= 8;
            }

            if(*payload & 0x20)
            {
                uint16_t desc_len;

                if(arg_len < 2)
                {
                    return MQTTERR_ILLEGAL_PKT;
                }

                desc_len = Mqtt_RB16(arg);
                if(arg_len < 2 + desc_len)
                {
                    return MQTTERR_ILLEGAL_PKT;
                }

                memmove(arg, arg + 2, desc_len);
                desc = arg;
                desc[desc_len] = '\0';

                arg += desc_len + 2;
                arg_len -= desc_len - 2;
            }

            err = ctx->handle_cmd(ctx->handle_cmd_arg, pkt_id, cmdid,
                                  ts, desc, arg, arg_len, dup,
                                  (enum MqttQosLevel)qos);
        }
    }
    else
    {
        err = ctx->handle_publish(ctx->handle_publish_arg, pkt_id, topic,
                                  payload, payload_len, dup,
                                  (enum MqttQosLevel)qos);
    }

    // send the publish response.
    if(err >= 0)
    {
        struct MqttBuffer response[1];
        MqttBuffer_Init(response);

        switch(qos)
        {
            case MQTT_QOS_LEVEL2:
                //assert(0 != pkt_id);
                err = Mqtt_PackPubRecPkt(response, pkt_id);
                smpctx->publish_state = 0;
                break;

            case MQTT_QOS_LEVEL1:
                // assert(0 != pkt_id);
                err = Mqtt_PackPubAckPkt(response, pkt_id);
                smpctx->publish_state = 0;
                break;

            default:
                break;
        }

        if((MQTTERR_NOERROR == err) && (MQTT_QOS_LEVEL0 != qos))
        {
            if(Mqtt_SendPkt(ctx, response, 0) != response->buffered_bytes)
            {
                err = MQTTERR_FAILED_SEND_RESPONSE;
            }
        }
        else
        {
            err = MQTTERR_FAILED_SEND_RESPONSE;
        }

        MqttBuffer_Destroy(response);
    }

    return err;
}

int Mqtt_HandlePubAck(struct MqttContext *ctx, char flags,
                      char *pkt, size_t size)
{
    uint16_t pkt_id;

    if((0 != flags) || (2 != size))
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    pkt_id = Mqtt_RB16(pkt);
    if(0 == pkt_id)
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    return ctx->handle_pub_ack(ctx->handle_pub_ack_arg, pkt_id);
}

int Mqtt_HandlePubRec(struct MqttContext *ctx, char flags,
                      char *pkt, size_t size)
{
    uint16_t pkt_id;
    int err;

    if((0 != flags) || (2 != size))
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    pkt_id = Mqtt_RB16(pkt);
    if(0 == pkt_id)
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    err = ctx->handle_pub_rec(ctx->handle_pub_rec_arg, pkt_id);
    if(err >= 0)
    {
        struct MqttBuffer response[1];
        MqttBuffer_Init(response);

        err = Mqtt_PackPubRelPkt(response, pkt_id);
        if(MQTTERR_NOERROR == err)
        {
            if(Mqtt_SendPkt(ctx, response, 0) != response->buffered_bytes)
            {
                err = MQTTERR_FAILED_SEND_RESPONSE;
            }
        }

        MqttBuffer_Destroy(response);
    }

    return err;
}
int Mqtt_HandlePubRel(struct MqttContext *ctx, char flags,
                      char *pkt, size_t size)
{
    uint16_t pkt_id;
    int err;

    if((2 != flags) || (2 != size))
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    pkt_id = Mqtt_RB16(pkt);
    if(0 == pkt_id)
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    err = ctx->handle_pub_rel(ctx->handle_pub_rel_arg, pkt_id);
    if(err >= 0)
    {
        struct MqttBuffer response[1];
        MqttBuffer_Init(response);
        err = Mqtt_PackPubCompPkt(response, pkt_id);
        if(MQTTERR_NOERROR == err)
        {
            if(Mqtt_SendPkt(ctx, response, 0) != response->buffered_bytes)
            {
                err = MQTTERR_FAILED_SEND_RESPONSE;
            }
        }

        MqttBuffer_Destroy(response);
    }

    return err;
}
int Mqtt_HandlePubComp(struct MqttContext *ctx, char flags,
                       char *pkt, size_t size)
{
    uint16_t pkt_id;

    if((0 != flags) || (2 != size))
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    pkt_id = Mqtt_RB16(pkt);
    if(0 == pkt_id)
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    return ctx->handle_pub_comp(ctx->handle_pub_comp_arg, pkt_id);
}

int Mqtt_HandleSubAck(struct MqttContext *ctx, char flags,
                      char *pkt, size_t size)
{
    uint16_t pkt_id;
    char *code;

    if((0 != flags) || (size < 2))
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    pkt_id = Mqtt_RB16(pkt);
    if(0 == pkt_id)
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    for(code = pkt + 2; code < pkt + size; ++code )
    {
        if(*code & 0x7C)
        {
            return MQTTERR_ILLEGAL_PKT;
        }
    }

    return ctx->handle_sub_ack(ctx->handle_sub_ack_arg, pkt_id, pkt + 2, size - 2);
}

int Mqtt_HandleUnsubAck(struct MqttContext *ctx, char flags,
                        char *pkt, size_t size)
{
    uint16_t pkt_id;

    if((0 != flags) || (2 != size))
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    pkt_id = Mqtt_RB16(pkt);
    if(0 == pkt_id)
    {
        return MQTTERR_ILLEGAL_PKT;
    }

    return ctx->handle_unsub_ack(ctx->handle_unsub_ack_arg, pkt_id);
}

static int Mqtt_Dispatch(struct MqttContext *ctx, char fh,  unsigned char *pkt, size_t size)
{
    const char flags = fh & 0x0F;
    switch(((uint8_t)fh) >> 4)
    {
        case MQTT_PKT_PINGRESP:

            return Mqtt_HandlePingResp(ctx, flags, pkt, size);

        case MQTT_PKT_CONNACK:
            return Mqtt_HandleConnAck(ctx, flags, pkt, size);

        case MQTT_PKT_PUBLISH:
            return Mqtt_HandlePublish(ctx, flags, pkt, size);

        case MQTT_PKT_PUBACK:
            return Mqtt_HandlePubAck(ctx, flags, pkt, size);

        case MQTT_PKT_PUBREC:
            return Mqtt_HandlePubRec(ctx, flags, pkt, size);

        case MQTT_PKT_PUBREL:
            return Mqtt_HandlePubRel(ctx, flags, pkt, size);

        case MQTT_PKT_PUBCOMP:
            return Mqtt_HandlePubComp(ctx, flags, pkt, size);

        case MQTT_PKT_SUBACK:
            return Mqtt_HandleSubAck(ctx, flags, pkt, size);

        case MQTT_PKT_UNSUBACK:
            return Mqtt_HandleUnsubAck(ctx, flags, pkt, size);

        default:
            break;
    }
    printf("%s %d\n", __func__, __LINE__);
    return MQTTERR_ILLEGAL_PKT;
}

int Mqtt_InitContext(struct MqttContext *ctx, uint32_t buf_size)
{
    memset(ctx, 0, sizeof(*ctx));

    ctx->bgn = (unsigned char*)malloc(buf_size);
    if(NULL == ctx->bgn)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    ctx->end = ctx->bgn + buf_size;
    ctx->pos = ctx->bgn;

    return MQTTERR_NOERROR;
}

void Mqtt_DestroyContext(struct MqttContext *ctx)
{
    free(ctx->bgn);
    memset(ctx, 0, sizeof(*ctx));
}

int Mqtt_RecvPkt(struct MqttContext *ctx)
{
    int bytes;
    uint32_t remaining_len = 0;
    unsigned char *pkt, *cursor;
    int length_sec_bytes = 0;
    int errcode = -1;
    cursor = ctx->bgn;
    bytes = ctx->read_func(ctx->pos);
    if(0 == bytes)
    {
        ctx->pos = ctx->bgn; // clear the buffer
        return MQTTERR_ENDOFFILE;
    }

    if(bytes < 0)
    {
        return MQTTERR_IO;
    }

    length_sec_bytes = Mqtt_ReadLength(ctx->pos + 1, bytes - 1, &remaining_len);

    errcode = Mqtt_Dispatch(ctx, ctx->pos[0], ctx->pos + length_sec_bytes + 1, remaining_len);
    if(errcode < 0)
    {
        printf("%s %d errcode=%d\n", __func__, __LINE__, errcode);
    }
    return errcode;

#if 0
    ctx->pos += bytes;
    if(ctx->pos > ctx->end)
    {
        return MQTTERR_BUF_OVERFLOW;
    }

    cursor = ctx->bgn;
    while(1)
    {
        int errcode;

        if(ctx->pos - cursor  < 2)
        {
            break;
        }

        bytes = Mqtt_ReadLength(cursor + 1, ctx->pos - cursor - 1, &remaining_len);

        printf("%s %d bytes=%d\n", __func__, __LINE__, bytes);
        if(-1 == bytes)
        {
            break;
        }
        else if(-2 == bytes)
        {
            return MQTTERR_ILLEGAL_PKT;
        }

        // one byte for the fixed header
        if(cursor + remaining_len + bytes + 1 > ctx->pos)
        {
            break;
        }

        pkt = cursor + bytes + 1;
        printf("%s %d fn=0x%x,fun1=0x%x\n", __func__, __LINE__, cursor[0], *ctx->pos);
        errcode = Mqtt_Dispatch(ctx, cursor[0], pkt, remaining_len);
        if(errcode < 0)
        {
            return errcode;
        }

        cursor += bytes + 1 + remaining_len;
    }

    if(cursor > ctx->bgn)
    {
        size_t movebytes = cursor - ctx->bgn;
        memmove(ctx->bgn, cursor, movebytes);
        ctx->pos -= movebytes;

        //assert(ctx->pos >= ctx->bgn);
    }

    return MQTTERR_NOERROR;
#endif
}

int Mqtt_SendPkt(struct MqttContext *ctx, const struct MqttBuffer *buf, uint32_t offset)
{
    const struct MqttExtent *cursor;
    const struct MqttExtent *first_ext;
    uint32_t bytes;
    int ext_count;
    int i;
    struct iovec *iov;

    if(offset >= buf->buffered_bytes)
    {
        return 0;
    }

    cursor = buf->first_ext;
    bytes = 0;
    while(cursor && bytes < offset)
    {
        bytes += cursor->len;
        cursor = cursor->next;
    }

    first_ext = cursor;
    ext_count = 0;
    for(; cursor; cursor = cursor->next)
    {
        ++ext_count;
    }

    if(0 == ext_count)
    {
        return 0;
    }

    //assert(first_ext);

    iov = (struct iovec*)malloc(sizeof(struct iovec) * ext_count);
    if(!iov)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    iov[0].iov_base = first_ext->payload + (offset - bytes);
    iov[0].iov_len = first_ext->len - (offset - bytes);

    i = 1;
    for(cursor = first_ext->next; cursor; cursor = cursor->next)
    {
        iov[i].iov_base = cursor->payload;
        iov[i].iov_len = cursor->len;
        ++i;
    }

    i = ctx->writev_func(ctx->writev_func_arg, iov, ext_count);
    free(iov);

    return i;
}

int Mqtt_PackConnectPkt(struct MqttBuffer *buf, uint16_t keep_alive, const char *id,
                        int clean_session, const char *will_topic,
                        const char *will_msg, uint16_t msg_len,
                        enum MqttQosLevel qos, int will_retain, const char *user,
                        const char *password, uint16_t pswd_len)
{
    int ret, id_len = -1;
    uint16_t  wt_len, user_len;
    size_t total_len;
    char flags = 0;
    struct MqttExtent *fix_head, *variable_head, *payload;
    char *cursor;

    fix_head = MqttBuffer_AllocExtent(buf, 5);
    if(NULL == fix_head)
    {
        printf("%s %d avail bytes:%d\n", __func__, __LINE__, buf->available_bytes);
        return MQTTERR_OUTOFMEMORY;
    }

    variable_head = MqttBuffer_AllocExtent(buf, 10);
    if(NULL == variable_head)
    {
        printf("%s %d avail bytes:%d\n", __func__, __LINE__, buf->available_bytes);
        return MQTTERR_OUTOFMEMORY;
    }

    total_len = 10; // length of the variable header
    id_len = Mqtt_CheckClentIdentifier(id);
    if(id_len < 0)
    {
        return MQTTERR_ILLEGAL_CHARACTER;
    }
    total_len += id_len + 2;

    if(clean_session)
    {
        flags |= MQTT_CONNECT_CLEAN_SESSION;
    }

    if(will_msg && !will_topic)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    wt_len = 0;
    if(will_topic)
    {
        flags |= MQTT_CONNECT_WILL_FLAG;
        wt_len = strlen(will_topic);
        if(Mqtt_CheckUtf8(will_topic, wt_len) != wt_len)
        {
            return MQTTERR_NOT_UTF8;
        }
    }

    switch(qos)
    {
        case MQTT_QOS_LEVEL0:
            flags |= MQTT_CONNECT_WILL_QOS0;
            break;
        case MQTT_QOS_LEVEL1:
            flags |= (MQTT_CONNECT_WILL_FLAG | MQTT_CONNECT_WILL_QOS1);
            break;
        case MQTT_QOS_LEVEL2:
            flags |= (MQTT_CONNECT_WILL_FLAG | MQTT_CONNECT_WILL_QOS2);
            break;
        default:
            return MQTTERR_INVALID_PARAMETER;
    }

    if(will_retain)
    {
        flags |= (MQTT_CONNECT_WILL_FLAG | MQTT_CONNECT_WILL_RETAIN);
    }

    if(flags & MQTT_CONNECT_WILL_FLAG)
    {
        total_len += 4 + wt_len + msg_len;
    }

    if(!user && password)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    user_len = 0;
    if(user)
    {
        flags |= MQTT_CONNECT_USER_NAME;
        user_len = strlen(user);
        ret = Mqtt_CheckUtf8(user, user_len);
        if(user_len != ret)
        {
            return MQTTERR_NOT_UTF8;
        }

        total_len += user_len + 2;
    }

    if(password)
    {
        flags |= MQTT_CONNECT_PASSORD;
        total_len += pswd_len + 2;
    }

    payload = MqttBuffer_AllocExtent(buf, total_len - 10);
    fix_head->payload[0] = MQTT_PKT_CONNECT << 4;

    ret = Mqtt_DumpLength(total_len, fix_head->payload + 1);
    if(ret < 0)
    {
        return MQTTERR_PKT_TOO_LARGE;
    }
    fix_head->len = ret + 1; // ajust the length of the extent

    variable_head->payload[0] = 0;
    variable_head->payload[1] = 4;
    variable_head->payload[2] = 'M';
    variable_head->payload[3] = 'Q';
    variable_head->payload[4] = 'T';
    variable_head->payload[5] = 'T';
    variable_head->payload[6] = 4; // protocol level 4
    variable_head->payload[7] = flags;
    Mqtt_WB16(keep_alive, variable_head->payload + 8);

    cursor = payload->payload;
    Mqtt_PktWriteString(&cursor, id, id_len);

    if(flags & MQTT_CONNECT_WILL_FLAG)
    {
        if(!will_msg)
        {
            will_msg = "";
            msg_len = 0;
        }

        Mqtt_PktWriteString(&cursor, will_topic, wt_len);
        Mqtt_PktWriteString(&cursor, will_msg, msg_len);
    }

    if(flags & MQTT_CONNECT_USER_NAME)
    {
        Mqtt_PktWriteString(&cursor, user, user_len);
    }

    if(flags & MQTT_CONNECT_PASSORD)
    {
        Mqtt_PktWriteString(&cursor, password, pswd_len);
    }

    MqttBuffer_AppendExtent(buf, fix_head);
    MqttBuffer_AppendExtent(buf, variable_head);
    MqttBuffer_AppendExtent(buf, payload);

    return MQTTERR_NOERROR;
}

int Mqtt_PackPublishPkt(struct MqttBuffer *buf, uint16_t pkt_id, const char *topic,
                        const char *payload, uint32_t size,
                        enum MqttQosLevel qos, int retain, int own)
{
    int ret;
    size_t topic_len, total_len;
    struct MqttExtent *fix_head, *variable_head;
    char *cursor;

    if(0 == pkt_id)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    for(topic_len = 0; '\0' != topic[topic_len]; ++topic_len)
    {
        if(('#' == topic[topic_len]) || ('+' == topic[topic_len]))
        {
            return MQTTERR_INVALID_PARAMETER;
        }
    }

    if(Mqtt_CheckUtf8(topic, topic_len) != topic_len)
    {
        return MQTTERR_NOT_UTF8;
    }

    fix_head = MqttBuffer_AllocExtent(buf, 5);
    if(NULL == fix_head)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    fix_head->payload[0] = MQTT_PKT_PUBLISH << 4;

    if(retain)
    {
        fix_head->payload[0] |= 0x01;
    }

    total_len = topic_len + size + 2;
    switch(qos)
    {
        case MQTT_QOS_LEVEL0:
            break;
        case MQTT_QOS_LEVEL1:
            fix_head->payload[0] |= 0x02;
            total_len += 2;
            break;
        case MQTT_QOS_LEVEL2:
            fix_head->payload[0] |= 0x04;
            total_len += 2;
            break;
        default:
            return MQTTERR_INVALID_PARAMETER;
    }

    ret = Mqtt_DumpLength(total_len, fix_head->payload + 1);
    if(ret < 0)
    {
        return MQTTERR_PKT_TOO_LARGE;
    }
    fix_head->len = ret + 1;

    variable_head = MqttBuffer_AllocExtent(buf, total_len - size);
    if(NULL == variable_head)
    {
        return MQTTERR_OUTOFMEMORY;
    }
    cursor = variable_head->payload;

    Mqtt_PktWriteString(&cursor, topic, topic_len);
    if(MQTT_QOS_LEVEL0 != qos)
    {
        Mqtt_WB16(pkt_id, cursor);
    }

    MqttBuffer_AppendExtent(buf, fix_head);
    MqttBuffer_AppendExtent(buf, variable_head);
    if(0 != size)
    {
        MqttBuffer_Append(buf, (char*)payload, size, own);
    }

    return MQTTERR_NOERROR;
}

int Mqtt_SetPktDup(struct MqttBuffer *buf)
{
    struct MqttExtent *fix_head = buf->first_ext;
    uint8_t pkt_type = ((uint8_t)buf->first_ext->payload[0]) >> 4;
    if(!fix_head || (MQTT_PKT_PUBLISH != pkt_type))
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    buf->first_ext->payload[0] |= 0x08;
    return MQTTERR_NOERROR;
}

static int Mqtt_PackPubAckPkt(struct MqttBuffer *buf, uint16_t pkt_id)
{
    struct MqttExtent *ext;

    if(0 == pkt_id)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    ext = MqttBuffer_AllocExtent(buf, 4);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    ext->payload[0] = MQTT_PKT_PUBACK << 4;
    ext->payload[1] = 2;
    Mqtt_WB16(pkt_id, ext->payload + 2);
    MqttBuffer_AppendExtent(buf, ext);

    return MQTTERR_NOERROR;
}

static int Mqtt_PackPubRecPkt(struct MqttBuffer *buf, uint16_t pkt_id)
{
    struct MqttExtent *ext;

    if(0 == pkt_id)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    ext = MqttBuffer_AllocExtent(buf, 4);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    ext->payload[0] = MQTT_PKT_PUBREC << 4;
    ext->payload[1] = 2;
    Mqtt_WB16(pkt_id, ext->payload + 2);
    MqttBuffer_AppendExtent(buf, ext);

    return MQTTERR_NOERROR;
}

int Mqtt_PackPubRelPkt(struct MqttBuffer *buf, uint16_t pkt_id)
{

    struct MqttExtent *ext;
    printf("%s %d\n", __func__, __LINE__);
    if(0 == pkt_id)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    ext = MqttBuffer_AllocExtent(buf, 4);
    ext->payload[0] = MQTT_PKT_PUBREL << 4 | 0x02;
    ext->payload[1] = 2;
    Mqtt_WB16(pkt_id, ext->payload + 2);
    MqttBuffer_AppendExtent(buf, ext);

    return MQTTERR_NOERROR;
}

int Mqtt_PackPubCompPkt(struct MqttBuffer *buf, uint16_t pkt_id)
{
    struct MqttExtent *ext;
    printf("%s %d\n", __func__, __LINE__);
    if(0 == pkt_id)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    ext = MqttBuffer_AllocExtent(buf, 4);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    ext->payload[0] = MQTT_PKT_PUBCOMP << 4;
    ext->payload[1] = 2;
    Mqtt_WB16(pkt_id, ext->payload + 2);
    MqttBuffer_AppendExtent(buf, ext);

    return MQTTERR_NOERROR;
}

int Mqtt_PackSubscribePkt(struct MqttBuffer *buf, uint16_t pkt_id,
                          const char *topic, enum MqttQosLevel qos)
{
    int ret;
    size_t topic_len, remaining_len;
    struct MqttExtent *fixed_head, *ext;
    char *cursor;

    if((0 == pkt_id) || (NULL == topic))
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    topic_len = strlen(topic);
    if(Mqtt_CheckUtf8(topic, topic_len) != topic_len)
    {
        return MQTTERR_NOT_UTF8;
    }

    fixed_head = MqttBuffer_AllocExtent(buf, 5);
    if(NULL == fixed_head)
    {
        return MQTTERR_OUTOFMEMORY;
    }
    fixed_head->payload[0] = (char)((MQTT_PKT_SUBSCRIBE << 4) | 0x02);

    remaining_len = 5 + topic_len;  // 2 bytes packet id, 1 bytes qos and topic length
    ext = MqttBuffer_AllocExtent(buf, remaining_len);
    if(NULL == ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    ret = Mqtt_DumpLength(remaining_len, fixed_head->payload + 1);
    if(ret < 0)
    {
        return MQTTERR_PKT_TOO_LARGE;
    }
    fixed_head->len = ret + 1;

    cursor = ext->payload;
    Mqtt_WB16(pkt_id, cursor);
    cursor += 2;

    Mqtt_PktWriteString(&cursor, topic, topic_len);
    cursor[0] = qos;

    MqttBuffer_AppendExtent(buf, fixed_head);
    MqttBuffer_AppendExtent(buf, ext);

    return MQTTERR_NOERROR;
}

int Mqtt_AppendSubscribeTopic(struct MqttBuffer *buf, const char *topic, enum MqttQosLevel qos)
{
    struct MqttExtent *fixed_head = buf->first_ext;
    struct MqttExtent *ext;
    size_t topic_len;
    uint32_t remaining_len;
    char *cursor;
    int ret;
    const char sub_type = (char)(MQTT_PKT_SUBSCRIBE << 4 | 0x02);
    if(!fixed_head || (sub_type != fixed_head->payload[0]) || !topic)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    topic_len = strlen(topic);
    ext = MqttBuffer_AllocExtent(buf, topic_len + 3);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    cursor = ext->payload;
    Mqtt_PktWriteString(&cursor, topic, topic_len);
    cursor[0] = qos;

    if(Mqtt_ReadLength(fixed_head->payload + 1, 4, &remaining_len) < 0)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    remaining_len += topic_len + 3;
    ret = Mqtt_DumpLength(remaining_len, fixed_head->payload + 1);
    if(ret < 0)
    {
        return MQTTERR_PKT_TOO_LARGE;
    }

    fixed_head->len = ret + 1;
    MqttBuffer_AppendExtent(buf, ext);
    return MQTTERR_NOERROR;
}

int Mqtt_PackUnsubscribePkt(struct MqttBuffer *buf, uint16_t pkt_id, const char *topic)
{
    struct MqttExtent *fixed_head, *ext;
    size_t topic_len;
    uint32_t remaining_len;
    char *cursor;
    int ret;

    if((0 == pkt_id) || !topic)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    topic_len = strlen(topic);
    if(Mqtt_CheckUtf8(topic, topic_len) != topic_len)
    {
        return MQTTERR_NOT_UTF8;
    }
    remaining_len = topic_len + 4; // 2 bytes for packet id

    fixed_head = MqttBuffer_AllocExtent(buf, 5);
    if(!fixed_head)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    fixed_head->payload[0] = (char)(MQTT_PKT_UNSUBSCRIBE << 4 | 0x02);
    ret = Mqtt_DumpLength(remaining_len, fixed_head->payload + 1);
    if(ret < 0)
    {
        return MQTTERR_PKT_TOO_LARGE;
    }
    fixed_head->len = ret + 1;

    ext = MqttBuffer_AllocExtent(buf, remaining_len);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    cursor = ext->payload;
    Mqtt_WB16(pkt_id, cursor);
    cursor += 2;

    Mqtt_PktWriteString(&cursor, topic, topic_len);
    MqttBuffer_AppendExtent(buf, fixed_head);
    MqttBuffer_AppendExtent(buf, ext);

    return MQTTERR_NOERROR;
}

int Mqtt_AppendUnsubscribeTopic(struct MqttBuffer *buf, const char *topic)
{
    struct MqttExtent *fixed_head = buf->first_ext;
    struct MqttExtent *ext;
    size_t topic_len;
    uint32_t remaining_len;
    char *cursor;
    int ret;
    const char unsub_type = (char)(MQTT_PKT_UNSUBSCRIBE << 4 | 0x02);
    if(!fixed_head || (unsub_type != fixed_head->payload[0]) || !topic)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    topic_len = strlen(topic);
    ext = MqttBuffer_AllocExtent(buf, topic_len + 2);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    cursor = ext->payload;
    Mqtt_PktWriteString(&cursor, topic, topic_len);

    if(Mqtt_ReadLength(fixed_head->payload + 1, 4, &remaining_len) < 0)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    remaining_len += topic_len + 2;
    ret = Mqtt_DumpLength(remaining_len, fixed_head->payload + 1);
    if(ret < 0)
    {
        return MQTTERR_PKT_TOO_LARGE;
    }
    fixed_head->len = ret + 1;

    MqttBuffer_AppendExtent(buf, ext);
    return MQTTERR_NOERROR;
}

int Mqtt_PackPingReqPkt(struct MqttBuffer *buf)
{
    struct MqttExtent *ext = MqttBuffer_AllocExtent(buf, 2);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    ext->payload[0] = (char)(MQTT_PKT_PINGREQ << 4);
    ext->payload[1] = 0;
    MqttBuffer_AppendExtent(buf, ext);

    return MQTTERR_NOERROR;
}

int Mqtt_PackDisconnectPkt(struct MqttBuffer *buf)
{
    struct MqttExtent *ext = MqttBuffer_AllocExtent(buf, 2);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    ext->payload[0] = (char)(MQTT_PKT_DISCONNECT << 4);
    ext->payload[1] = 0;
    MqttBuffer_AppendExtent(buf, ext);

    return MQTTERR_NOERROR;
}

int Mqtt_PackCmdRetPkt(struct MqttBuffer *buf, uint16_t pkt_id, const char *cmdid,
                       const char *ret, uint32_t ret_len,  int own)
{
    size_t cmdid_size = strlen(cmdid) + 1;
    struct MqttExtent *ext = MqttBuffer_AllocExtent(buf, cmdid_size + CMD_TOPIC_PREFIX_LEN);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    memcpy(ext->payload, CMD_TOPIC_PREFIX, CMD_TOPIC_PREFIX_LEN);
    strcpy(ext->payload + CMD_TOPIC_PREFIX_LEN, cmdid);

    return Mqtt_PackPublishPkt(buf, pkt_id, ext->payload, ret, ret_len,
                               MQTT_QOS_LEVEL1, 0, own);
}

int Mqtt_PackDataPointStart(struct MqttBuffer *buf, uint16_t pkt_id,
                            enum MqttQosLevel qos, int retain, int save)
{
    int err;
    struct MqttExtent *ext;
    struct DataPointPktInfo *info = NULL;
    if(buf->first_ext)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    if(save)
    {
        err = Mqtt_PackPublishPkt(buf, pkt_id, "$SYS/savedata", NULL, 0, qos, retain, 0);
    }
    else
    {
        err = Mqtt_PackPublishPkt(buf, pkt_id, "$SYS/passdata", NULL, 0, qos, retain, 0);
    }

    if(err != MQTTERR_NOERROR)
    {
        return err;
    }

    ext = MqttBuffer_AllocExtent(buf, 2 + sizeof(struct DataPointPktInfo));
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    ext->payload[0] = MQTT_DPTYPE_TRIPLE;
    ext->payload[1] = '{';

    info = (struct DataPointPktInfo*)(ext->payload + 2);
    info->tag = DATA_POINT_PKT_TAG;
    info->subobj_depth = 0;

    if(MQTTERR_NOERROR != (err = Mqtt_AppendLength(buf, ext->len)))
    {
        return err;
    }

    MqttBuffer_AppendExtent(buf, ext);
    return MQTTERR_NOERROR;
}

int Mqtt_PackDataPointStartNormal(struct MqttBuffer *buf, unsigned char *topic, uint16_t pkt_id,
                                  enum MqttQosLevel qos, int retain, int save)
{
    int err;
    struct MqttExtent *ext;

    if((buf->first_ext) || (topic == NULL))
    {
        return MQTTERR_INVALID_PARAMETER;
    }
    err = Mqtt_PackPublishPkt(buf, pkt_id, (const char *)topic, NULL, 0, qos, retain, 0);

    if(err != MQTTERR_NOERROR)
    {
        return err;
    }
    return MQTTERR_NOERROR;
}

static int Mqtt_AppendDP(struct MqttBuffer *buf, const char *dsid, int64_t ts,
                         const char *value, size_t value_len, int str)
{
    int err;
    size_t dsid_len, total_len;
    struct MqttExtent *ext;
    struct DataPointPktInfo *info;
    char *cursor;

    info = Mqtt_GetDataPointPktInfo(buf);
    if(!info)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    if(!dsid || (0 == (dsid_len = strlen(dsid))))
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    if(Mqtt_CheckUtf8(dsid, dsid_len) != dsid_len)
    {
        return MQTTERR_NOT_UTF8;
    }

    if(Mqtt_HasIllegalCharacter(dsid, dsid_len))
    {
        return MQTTERR_ILLEGAL_CHARACTER;
    }

    if(!value)
    {
        value_len = 4;
    }

    total_len = dsid_len + 9 + (ts > 0 ? FORMAT_TIME_STRING_SIZE : 0) +
                value_len + (str ? 2 : 0);

    ext = MqttBuffer_AllocExtent(buf, (uint32_t)total_len);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }
    cursor = ext->payload;

    *(cursor++) = '\"';
    memcpy(cursor, dsid, dsid_len);
    cursor += dsid_len;
    *(cursor++) = '\"';
    *(cursor++) = ':';
    *(cursor++) = '{';
    *(cursor++) = '\"';

    if(ts > 0)
    {
        if(0 == Mqtt_FormatTime(ts, cursor))
        {
            return MQTTERR_INTERNAL;
        }
        cursor += FORMAT_TIME_STRING_SIZE;
    }

    *(cursor++) = '\"';
    *(cursor++) = ':';

    if(str)
    {
        *(cursor++) = '\"';
    }

    if(value)
    {
        memcpy(cursor, value, value_len);
        cursor += value_len;
    }
    else
    {
        *(cursor++) = 'n';
        *(cursor++) = 'u';
        *(cursor++) = 'l';
        *(cursor++) = 'l';
    }

    if(str)
    {
        *(cursor++) = '\"';
    }
    *(cursor++) = '}';
    *(cursor++) = ',';

    if(MQTTERR_NOERROR != (err = Mqtt_AppendLength(buf, ext->len)))
    {
        return err;
    }

    MqttBuffer_AppendExtent(buf, ext);
    return MQTTERR_NOERROR;
}

int Mqtt_AppendDPNull(struct MqttBuffer *buf, const char *dsid)
{
    return Mqtt_AppendDP(buf, dsid, 0, NULL, 0, 0);
}

int Mqtt_AppendDPInt(struct MqttBuffer *buf, const char *dsid,
                     int64_t ts, int value)
{
    char intbuf[MAX_INTBUF_SIZE];
    size_t bytes = (size_t)snprintf(intbuf, MAX_INTBUF_SIZE, "%d", value);
    return Mqtt_AppendDP(buf, dsid, ts, intbuf, bytes, 0);
}

int Mqtt_AppendDPDouble(struct MqttBuffer *buf, const char *dsid,
                        int64_t ts, double value)
{
    char dblbuf[MAX_DBLBUF_SIZE];
    size_t bytes = (size_t)snprintf(dblbuf, MAX_DBLBUF_SIZE, "%lf", value);
    return Mqtt_AppendDP(buf, dsid, ts, dblbuf, bytes, 0);
}

int Mqtt_AppendDPString(struct MqttBuffer *buf, const char *dsid,
                        int64_t ts, const char *value)
{
    size_t bytes;
    if(!value)
    {
        value = "";
    }

    bytes = strlen(value);
    if(Mqtt_CheckUtf8(value, bytes) != bytes)
    {
        return MQTTERR_NOT_UTF8;
    }

    if(Mqtt_HasIllegalCharacter(value, bytes))
    {
        return MQTTERR_ILLEGAL_CHARACTER;
    }

    return Mqtt_AppendDP(buf, dsid, ts, value, bytes, 1);
}

int Mqtt_AppendDPStartObject(struct MqttBuffer *buf, const char *dsid, int64_t ts)
{
    int err;
    char strtime[FORMAT_TIME_STRING_SIZE + 1];

    if(MQTTERR_NOERROR != (err = Mqtt_AppendDPStartSubobject(buf, dsid)))
    {
        return err;
    }

    if(ts > 0)
    {
        if(0 == Mqtt_FormatTime(ts, strtime))
        {
            return MQTTERR_INTERNAL;
        }
        strtime[FORMAT_TIME_STRING_SIZE] = '\0';
    }
    else
    {
        strtime[0] = '\0';
    }

    return Mqtt_AppendDPStartSubobject(buf, strtime);
}

int Mqtt_AppendDPFinishObject(struct MqttBuffer *buf)
{
    int err;

    if(MQTTERR_NOERROR != (err = Mqtt_AppendDPFinishSubobject(buf)))
    {
        return err;
    }

    return Mqtt_AppendDPFinishSubobject(buf);
}

static int Mqtt_AppendDPSubvalue(struct MqttBuffer *buf, const char *name,
                                 const char *value, size_t value_len, int str)
{
    int err;
    size_t name_len;
    size_t total_len;
    struct MqttExtent *ext;
    struct DataPointPktInfo *info;
    char *cursor;

    info = Mqtt_GetDataPointPktInfo(buf);
    if(!info)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    if(info->subobj_depth <= 0)
    {
        return MQTTERR_NOT_IN_SUBOBJECT;
    }

    // 1 byte for : and 1 byte for ,
    // if value is string 2 bytes for "" of value
    // format like this: "name":"value",
    total_len = value_len + 2 + (str ? 2 : 0);
    name_len = strlen(name);
    if(Mqtt_CheckUtf8(name, name_len) != name_len)
    {
        return MQTTERR_NOT_UTF8;
    }

    if(Mqtt_HasIllegalCharacter(name, name_len))
    {
        return MQTTERR_ILLEGAL_CHARACTER;
    }
    total_len += name_len + 2;

    ext = MqttBuffer_AllocExtent(buf, (uint32_t)total_len);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    cursor = ext->payload;
    *(cursor++) = '\"';
    memcpy(cursor, name, name_len);
    cursor += name_len;
    *(cursor++) = '\"';
    *(cursor++) = ':';

    if(str)
    {
        *(cursor++) = '\"';
    }

    memcpy(cursor, value, value_len);
    cursor += value_len;

    if(str)
    {
        *(cursor++) = '\"';
    }

    *(cursor++) = ',';
    if(MQTTERR_NOERROR != (err = Mqtt_AppendLength(buf, ext->len)))
    {
        return err;
    }

    MqttBuffer_AppendExtent(buf, ext);
    return MQTTERR_NOERROR;
}

int Mqtt_AppendDPSubvalueInt(struct MqttBuffer *buf, const char *name, int value)
{
    char intbuf[MAX_INTBUF_SIZE];
    size_t bytes = (size_t)snprintf(intbuf, MAX_INTBUF_SIZE, "%d", value);
    return Mqtt_AppendDPSubvalue(buf, name, intbuf, bytes, 0);
}

int Mqtt_AppendDPSubvalueDouble(struct MqttBuffer *buf, const char *name, double value)
{
    char dblbuf[MAX_DBLBUF_SIZE];
    size_t bytes = (size_t)snprintf(dblbuf, MAX_DBLBUF_SIZE, "%lf", value);
    return Mqtt_AppendDPSubvalue(buf, name, dblbuf, bytes, 0);
}

int Mqtt_AppendDPSubvalueString(struct MqttBuffer *buf, const char *name, const char *value)
{
    size_t value_len;

    if(!value)
    {
        value = "";
    }

    value_len = strlen(value);
    if(Mqtt_CheckUtf8(value, value_len) != value_len)
    {
        return MQTTERR_NOT_UTF8;
    }

    if(Mqtt_HasIllegalCharacter(value, value_len))
    {
        return MQTTERR_ILLEGAL_CHARACTER;
    }

    return Mqtt_AppendDPSubvalue(buf, name, value, value_len, 1);
}

int Mqtt_AppendDPStartSubobject(struct MqttBuffer *buf, const char *name)
{
    int err;
    size_t name_len;
    struct MqttExtent *ext;
    struct DataPointPktInfo *info;
    char *cursor;

    if(!name)
    {
        name = "";
    }

    name_len = strlen(name);
    if(Mqtt_CheckUtf8(name, name_len) != name_len)
    {
        return MQTTERR_NOT_UTF8;
    }

    if(Mqtt_HasIllegalCharacter(name, name_len))
    {
        return MQTTERR_ILLEGAL_CHARACTER;
    }

    info = Mqtt_GetDataPointPktInfo(buf);
    if(!info)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    ++info->subobj_depth;

    // 2 bytes for "" of name, 1 byte for : and 1 byte for {
    ext = MqttBuffer_AllocExtent(buf, name_len + 4);
    if(!ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }
    cursor = ext->payload;
    *(cursor++) = '\"';
    memcpy(cursor, name, name_len);
    cursor += name_len;
    *(cursor++) = '\"';
    *(cursor++) = ':';
    *(cursor++) = '{';

    if(MQTTERR_NOERROR != (err = Mqtt_AppendLength(buf, ext->len)))
    {
        return err;
    }

    MqttBuffer_AppendExtent(buf, ext);
    return MQTTERR_NOERROR;
}

int Mqtt_AppendDPFinishSubobject(struct MqttBuffer *buf)
{
    int err;
    struct MqttExtent *ext;
    struct DataPointPktInfo *info;

    info = Mqtt_GetDataPointPktInfo(buf);
    if(!info)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    if(--info->subobj_depth < 0)
    {
        return MQTTERR_NOT_IN_SUBOBJECT;
    }

    if('{' == buf->last_ext->payload[buf->last_ext->len - 1])
    {
        ext = MqttBuffer_AllocExtent(buf, 2);
        if(!ext)
        {
            return MQTTERR_OUTOFMEMORY;
        }

        ext->payload[0] = '}';
        ext->payload[1] = ',';
    }
    else
    {
        buf->last_ext->payload[buf->last_ext->len - 1] = '}';
        ext = MqttBuffer_AllocExtent(buf, 1);
        if(!ext)
        {
            return MQTTERR_OUTOFMEMORY;
        }
        ext->payload[0] = ',';
    }

    if(MQTTERR_NOERROR != (err = Mqtt_AppendLength(buf, ext->len)))
    {
        return err;
    }

    MqttBuffer_AppendExtent(buf, ext);
    return MQTTERR_NOERROR;
}

int Mqtt_PackDataPointFinish(struct MqttBuffer *buf)
{
    struct DataPointPktInfo *info;
    struct MqttExtent *first_payload;
    int err;

    info = Mqtt_GetDataPointPktInfo(buf);
    if(!info)
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    if(info->subobj_depth > 0)
    {
        return MQTTERR_INCOMPLETE_SUBOBJECT;
    }

    first_payload = buf->first_ext->next->next;
    first_payload->len = 2;

    if(MQTTERR_NOERROR != (err = Mqtt_EraseLength(buf, sizeof(struct DataPointPktInfo))))
    {
        return err;
    }

    if(buf->last_ext != first_payload)
    {
        buf->last_ext->payload[buf->last_ext->len - 1] = '}';
    }
    else
    {
        struct MqttExtent *end_ext = MqttBuffer_AllocExtent(buf, 1);
        if(!end_ext)
        {
            return MQTTERR_OUTOFMEMORY;
        }

        end_ext->payload[0] = '}';
        MqttBuffer_AppendExtent(buf, end_ext);
        err = Mqtt_AppendLength(buf, end_ext->len);
        if(MQTTERR_NOERROR != err)
        {
            return err;
        }
    }

    return MQTTERR_NOERROR;
}

int Mqtt_PackDataPointByBinary(struct MqttBuffer *buf, uint16_t pkt_id, const char *dsid,
                               const char *desc, int64_t time, const char *bin, uint32_t size,
                               enum MqttQosLevel qos, int retain, int own, int save)
{
    int err, pack_time;
    struct MqttExtent *header_ext, *binary_ext;
    size_t dsid_len, desc_len, header_len;
    char *cursor;

    if(buf->first_ext || !dsid || !bin || (0 == size))
    {
        return MQTTERR_INVALID_PARAMETER;
    }

    if(save)
    {
        err = Mqtt_PackPublishPkt(buf, pkt_id, "$SAVEDATA", NULL, 0, qos, retain, 0);
    }
    else
    {
        err = Mqtt_PackPublishPkt(buf, pkt_id, "$PASSDATA", NULL, 0, qos, retain, 0);
    }

    if(err != MQTTERR_NOERROR)
    {
        return err;
    }

    dsid_len = strlen(dsid);
    header_len = dsid_len + 2 + 1; // 2 bytes for the length of the header, 1 byte for type

    if(Mqtt_CheckUtf8(dsid, dsid_len) != dsid_len)
    {
        return MQTTERR_NOT_UTF8;
    }

    pack_time = 0;
    desc_len = 0;
    if(desc)
    {
        desc_len = strlen(desc);
        header_len += desc_len + 1; // 1 byte for privous split ','

        if(Mqtt_CheckUtf8(desc, desc_len) != desc_len)
        {
            return MQTTERR_NOT_UTF8;
        }

        pack_time = 1;
    }

    if(time > 0)
    {
        pack_time = 1;
        header_len += FORMAT_TIME_STRING_SIZE + 1; // 1 byte for privous split ','
    }
    else if(pack_time)
    {
        header_len += 1;
    }

    header_len += 4; // 4 bytes is the length of the binary

    header_ext = MqttBuffer_AllocExtent(buf, header_len);
    if(!header_ext)
    {
        return MQTTERR_OUTOFMEMORY;
    }

    cursor = header_ext->payload;
    *(cursor++) = MQTT_DPTYPE_BINARY;
    Mqtt_WB16(header_len - 7, cursor);
    cursor += 2;
    memcpy(cursor, dsid, dsid_len);
    cursor += dsid_len;

    if(pack_time)
    {
        *(cursor++) = ',';
        if(time > 0)
        {
            Mqtt_FormatTime(time, cursor);
            cursor += FORMAT_TIME_STRING_SIZE;
        }
    }

    if(desc)
    {
        *(cursor++) = ',';
        memcpy(cursor, desc, desc_len);
        cursor += desc_len;
    }

    Mqtt_WB32(size, cursor);

    if(own)
    {
        binary_ext = MqttBuffer_AllocExtent(buf, size);
        if(!binary_ext)
        {
            return MQTTERR_OUTOFMEMORY;
        }

        memcpy(binary_ext->payload, bin, size);
    }
    else
    {
        binary_ext = MqttBuffer_AllocExtent(buf, 0);
        if(!binary_ext)
        {
            return MQTTERR_OUTOFMEMORY;
        }

        binary_ext->payload = (char*)bin;
        binary_ext->len = (uint32_t)size;
    }

    err = Mqtt_AppendLength(buf, header_len + size);
    if(MQTTERR_NOERROR != err)
    {
        return err;
    }

    MqttBuffer_AppendExtent(buf, header_ext);
    MqttBuffer_AppendExtent(buf, binary_ext);
    return MQTTERR_NOERROR;
}
