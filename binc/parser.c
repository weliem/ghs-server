/*
 *   Copyright (c) 2022 Martijn van Welie
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 */

#include "parser.h"
#include "math.h"
#include "logger.h"
#include <time.h>

#define MAX_CHAR_SIZE 512

// IEEE 11073 Reserved float values
typedef enum {
    MDER_POSITIVE_INFINITY = 0x007FFFFE,
    MDER_NaN = 0x007FFFFF,
    MDER_NRes = 0x00800000,
    MDER_RESERVED_VALUE = 0x00800001,
    MDER_NEGATIVE_INFINITY = 0x00800002
} ReservedFloatValues;

struct parser_instance {
    GByteArray *bytes;
    guint offset;
    int byteOrder;
};

static const double reserved_float_values[5] = {MDER_POSITIVE_INFINITY, MDER_NaN, MDER_NaN, MDER_NaN,
                                                MDER_NEGATIVE_INFINITY};

Parser *parser_create_empty(guint reserved_size, int byteOrder) {
    Parser *parser = g_new0(Parser, 1);
    parser->offset = 0;
    parser->byteOrder = byteOrder;
    parser->bytes = g_byte_array_sized_new(reserved_size);

    // Initialize all values so that valgrind doesn't complain anymore
    for(guint i=0;i<reserved_size;i++) {
        parser->bytes->data[i] = 0;
    }

    return parser;
}

Parser *parser_create(GByteArray *bytes, int byteOrder) {
    Parser *parser = g_new0(Parser, 1);
    parser->bytes = bytes;
    parser->offset = 0;
    parser->byteOrder = byteOrder;
    return parser;
}

void parser_free(Parser *parser) {
    g_assert(parser != NULL);
    parser->bytes = NULL;
    g_free(parser);
}

void parser_set_offset(Parser *parser, guint offset) {
    g_assert(parser != NULL);
    parser->offset = offset;
}

guint8 parser_get_uint8(Parser *parser) {
    g_assert(parser != NULL);
    g_assert(parser->offset < parser->bytes->len);

    guint8 result = parser->bytes->data[parser->offset];
    parser->offset = parser->offset + 1;
    return result;
}

gint8 parser_get_sint8(Parser *parser) {
    g_assert(parser != NULL);
    g_assert(parser->offset < parser->bytes->len);

    gint8 result = parser->bytes->data[parser->offset];
    parser->offset = parser->offset + 1;
    return result;
}

guint16 parser_get_uint16(Parser *parser) {
    g_assert(parser != NULL);
    g_assert((parser->offset + 1) < parser->bytes->len);

    guint16 byte1, byte2;
    byte1 = parser->bytes->data[parser->offset];
    byte2 = parser->bytes->data[parser->offset + 1];
    parser->offset = parser->offset + 2;
    if (parser->byteOrder == LITTLE_ENDIAN) {
        return (byte2 << 8) + byte1;
    } else {
        return (byte1 << 8) + byte2;
    }
}

gint16 parser_get_sint16(Parser *parser) {
    g_assert(parser != NULL);
    g_assert((parser->offset + 1) < parser->bytes->len);

    gint16 byte1, byte2;
    byte1 = parser->bytes->data[parser->offset];
    byte2 = parser->bytes->data[parser->offset + 1];
    parser->offset = parser->offset + 2;
    if (parser->byteOrder == LITTLE_ENDIAN) {
        return (byte2 << 8) + byte1;
    } else {
        return (byte1 << 8) + byte2;
    }
}

guint32 parser_get_uint24(Parser *parser) {
    g_assert(parser != NULL);
    g_assert((parser->offset + 2) < parser->bytes->len);

    guint8 byte1, byte2, byte3;
    byte1 = parser->bytes->data[parser->offset];
    byte2 = parser->bytes->data[parser->offset+1];
    byte3 = parser->bytes->data[parser->offset+2];
    parser->offset = parser->offset + 3;
    if (parser->byteOrder == LITTLE_ENDIAN) {
        return (byte3 << 16) + (byte2 << 8) + byte1;
    } else {
        return (byte1 << 16) + (byte2 << 8) + byte3;
    }
}

guint32 parser_get_uint32(Parser *parser) {
    g_assert(parser != NULL);
    g_assert((parser->offset + 3) < parser->bytes->len);

    guint8 byte1, byte2, byte3, byte4;
    byte1 = parser->bytes->data[parser->offset];
    byte2 = parser->bytes->data[parser->offset + 1];
    byte3 = parser->bytes->data[parser->offset + 2];
    byte4 = parser->bytes->data[parser->offset + 3];
    parser->offset = parser->offset + 4;
    if (parser->byteOrder == LITTLE_ENDIAN) {
        return (byte4 << 24) + (byte3 << 16) + (byte2 << 8) + byte1;
    } else {
        return (byte1 << 24) + (byte2 << 16) + (byte3 << 8) + byte4;
    }
}

double parser_get_sfloat(Parser *parser) {
    g_assert(parser != NULL);
    g_assert(parser->offset < parser->bytes->len);

    guint16 sfloat = parser_get_uint16(parser);

    int mantissa = sfloat & 0xfff;
    if (mantissa >= 0x800) {
        mantissa = mantissa - 0x1000;
    }
    int exponent = sfloat >> 12;
    if (exponent >= 0x8) {
        exponent = exponent - 0x10;
    }
    return (mantissa * pow(10.0, exponent));
}

/* round number n to d decimal points */
float fround(float n, int d) {
    int rounded = floor(n * pow(10.0f, d) + 0.5f);
    int divider = (int) pow(10.0f, d);
    return (float) rounded / (float) divider;
}

double parser_get_float(Parser *parser) {
    g_assert(parser != NULL);
    guint32 int_data = parser_get_uint32(parser);

    guint32 mantissa = int_data & 0xFFFFFF;
    gint8 exponent = int_data >> 24;
    double output = 0;

    if (mantissa >= MDER_POSITIVE_INFINITY &&
        mantissa <= MDER_NEGATIVE_INFINITY) {
        output = reserved_float_values[mantissa - MDER_POSITIVE_INFINITY];
    } else {
        if (mantissa >= 0x800000) {
            mantissa = -((0xFFFFFF + 1) - mantissa);
        }
        output = (mantissa * pow(10.0f, exponent));
    }

    return output;
}

GString *parser_get_string(Parser *parser) {
    g_assert(parser != NULL);
    g_assert(parser->bytes != NULL);

    return g_string_new_len((const char *) parser->bytes->data + parser->offset,
                            parser->bytes->len - parser->offset);
}

GDateTime *parser_get_date_time(Parser *parser) {
    g_assert(parser != NULL);

    guint16 year = parser_get_uint16(parser);
    guint8 month = parser_get_uint8(parser);
    guint8 day = parser_get_uint8(parser);
    guint8 hour = parser_get_uint8(parser);
    guint8 min = parser_get_uint8(parser);
    guint8 sec = parser_get_uint8(parser);

    return g_date_time_new_local(year, month, day, hour, min, sec);
}

GByteArray *binc_get_current_time() {
    GByteArray *byteArray = g_byte_array_new();

    GDateTime *now = g_date_time_new_now_local();
    guint year = g_date_time_get_year(now);
    guint8 yearLsb = year & 0xFF;
    guint8 yearMsb = year >> 8;
    guint8 month = g_date_time_get_month(now);
    guint8 day = g_date_time_get_day_of_month(now);
    guint8 hours = g_date_time_get_hour(now);
    guint8 minutes = g_date_time_get_minute(now);
    guint8 seconds = g_date_time_get_second(now);
    guint8 dayOfWeek = g_date_time_get_day_of_week(now);
    guint8 miliseconds = (g_date_time_get_microsecond(now) / 1000) * 256 / 1000;
    guint8 reason = 1;
    g_date_time_unref(now);

    g_byte_array_append(byteArray, &yearLsb, 1);
    g_byte_array_append(byteArray, &yearMsb, 1);
    g_byte_array_append(byteArray, &month, 1);
    g_byte_array_append(byteArray, &day, 1);
    g_byte_array_append(byteArray, &hours, 1);
    g_byte_array_append(byteArray, &minutes, 1);
    g_byte_array_append(byteArray, &seconds, 1);
    g_byte_array_append(byteArray, &dayOfWeek, 1);
    g_byte_array_append(byteArray, &miliseconds, 1);
    g_byte_array_append(byteArray, &reason, 1);
    return byteArray;
}

GByteArray *binc_get_date_time() {
    GByteArray *byteArray = g_byte_array_new();

    GDateTime *now = g_date_time_new_now_local();
    guint year = g_date_time_get_year(now);
    guint8 yearLsb = year & 0xFF;
    guint8 yearMsb = year >> 8;
    guint8 month = g_date_time_get_month(now);
    guint8 day = g_date_time_get_day_of_month(now);
    guint8 hours = g_date_time_get_hour(now);
    guint8 minutes = g_date_time_get_minute(now);
    guint8 seconds = g_date_time_get_second(now);
    g_date_time_unref(now);

    g_byte_array_append(byteArray, &yearLsb, 1);
    g_byte_array_append(byteArray, &yearMsb, 1);
    g_byte_array_append(byteArray, &month, 1);
    g_byte_array_append(byteArray, &day, 1);
    g_byte_array_append(byteArray, &hours, 1);
    g_byte_array_append(byteArray, &minutes, 1);
    g_byte_array_append(byteArray, &seconds, 1);
    return byteArray;
}

static void prepare_byte_array(Parser *parser, guint required_length) {
    parser->bytes = g_byte_array_set_size(parser->bytes, required_length);

    for(guint i=parser->offset; i<required_length-1; i++) {
        parser->bytes->data[i] = 0;
    }
}

static int intToSignedBits(const int i, int size) {
    if (i < 0) {
        return (1 << (size - 1)) + (i & ((1 << (size - 1)) - 1));
    }
    return i;
}

void parser_set_uint8(Parser *parser, const guint8 value) {
    prepare_byte_array(parser, parser->offset + 1);

    parser->bytes->data[parser->offset] = (value & 0xFF);
    parser->offset += 1;
}

void parser_set_sint8(Parser *parser, const gint8 value) {
    parser_set_uint8(parser, intToSignedBits(value, 8));
}

void parser_set_uint16(Parser *parser, const guint16 value) {
    prepare_byte_array(parser, parser->offset + 2);

    if (parser->byteOrder == LITTLE_ENDIAN) {
        parser->bytes->data[parser->offset] = (value & 0xFF);
        parser->bytes->data[parser->offset + 1] = (value >> 8);
    } else {
        parser->bytes->data[parser->offset] = (value >> 8);
        parser->bytes->data[parser->offset + 1] = (value & 0xFF);
    }
    parser->offset += 2;
}

void parser_set_sint16(Parser *parser, const gint16 value) {
    parser_set_uint16(parser, intToSignedBits(value, 16));
}

void parser_set_uint32(Parser *parser, const guint32 value) {
    prepare_byte_array(parser, parser->offset + 4);

    if (parser->byteOrder == LITTLE_ENDIAN) {
        parser->bytes->data[parser->offset] = (value & 0xFF);
        parser->bytes->data[parser->offset + 1] = (value >> 8);
        parser->bytes->data[parser->offset + 2] = (value >> 16);
        parser->bytes->data[parser->offset + 3] = (value >> 24);
    } else {
        parser->bytes->data[parser->offset] = (value >> 24);
        parser->bytes->data[parser->offset + 1] = (value >> 16);
        parser->bytes->data[parser->offset + 2] = (value >> 8);
        parser->bytes->data[parser->offset + 3] = (value & 0xFF);
    }
    parser->offset += 4;
}


void parser_set_uint48(Parser *parser, const guint64 value) {
    prepare_byte_array(parser, parser->offset + 6);

    if (parser->byteOrder == LITTLE_ENDIAN) {
        parser->bytes->data[parser->offset] = (value & 0xFF);
        parser->bytes->data[parser->offset + 1] = (value >> 8);
        parser->bytes->data[parser->offset + 2] = (value >> 16);
        parser->bytes->data[parser->offset + 3] = (value >> 24);
        parser->bytes->data[parser->offset + 4] = (value >> 32);
        parser->bytes->data[parser->offset + 5] = (value >> 40);
    } else {
        parser->bytes->data[parser->offset] = (value >> 40);
        parser->bytes->data[parser->offset + 1] = (value >> 32);
        parser->bytes->data[parser->offset + 2] = (value >> 24);
        parser->bytes->data[parser->offset + 3] = (value >> 16);
        parser->bytes->data[parser->offset + 4] = (value >> 8);
        parser->bytes->data[parser->offset + 5] = (value & 0xFF);
    }
    parser->offset += 6;
}

void parser_set_sint32(Parser *parser, const gint32 value) {
    parser_set_uint32(parser, intToSignedBits(value, 32));
}

static void parser_set_float_internal(Parser *parser, const int mantissa, const int exponent ) {
    int newMantissa = intToSignedBits(mantissa, 24);
    int newExponent = intToSignedBits(exponent, 8);
    guint8* value = parser->bytes->data;

    if (parser->byteOrder == LITTLE_ENDIAN) {
        value[parser->offset] = (newMantissa & 0xFF);
        value[parser->offset + 1] = ((newMantissa >> 8) & 0xFF);
        value[parser->offset + 2] = ((newMantissa >> 16) & 0xFF);
        value[parser->offset + 3] = (newExponent & 0xFF);
    } else {
        value[parser->offset] = (newExponent & 0xFF);
        value[parser->offset + 1] = ((newMantissa >> 16) & 0xFF);
        value[parser->offset + 2] = ((newMantissa >> 8) & 0xFF);
        value[parser->offset + 3] = (newMantissa & 0xFF);
    }
    parser->offset += 4;
}

static void parser_set_sfloat_internal(Parser *parser, const int mantissa, const int exponent ) {
    int newMantissa = intToSignedBits(mantissa, 12);
    int newExponent = intToSignedBits(exponent, 4);
    guint8* value = parser->bytes->data;
    if (parser->byteOrder == LITTLE_ENDIAN) {
        value[parser->offset] = (newMantissa & 0xFF);
        value[parser->offset + 1] = ((newMantissa >> 8) & 0x0F);
        value[parser->offset + 1] += ((newExponent & 0x0F) << 4);
    } else {
        value[parser->offset] = ((newMantissa >> 8) & 0x0F);
        value[parser->offset] += ((newExponent & 0x0F) << 4);
        value[parser->offset + 1] = (newMantissa & 0xFF);
    }
    parser->offset += 2;
}

void parser_set_float(Parser *parser, const float value, const guint8 precision) {
    prepare_byte_array(parser, parser->offset + 4);
    float mantissa = (float) (value * pow(10, precision));
    parser_set_float_internal(parser, (int) mantissa, -precision);
}

void parser_set_sfloat(Parser *parser, const float value, const guint8 precision) {
    prepare_byte_array(parser, parser->offset + 2);
    float mantissa = (float) (value * pow(10, precision));
    parser_set_sfloat_internal(parser, (int) mantissa, -precision);
}

void parser_set_string(Parser *parser, char *string) {
    prepare_byte_array(parser, parser->offset + strlen(string));
    g_byte_array_append(parser->bytes, (guint8*) string, strlen(string));
}

void parser_set_elapsed_time(Parser *parser) {
    prepare_byte_array(parser, parser->offset + 9);

    const gint64 elapsed_time_epoch = 946681200;
    GDateTime *now = g_date_time_new_now_local();
    gint64 seconds_since_unix_epoch = g_date_time_to_unix(now);
    gint64 seconds_since_ets_epoch = seconds_since_unix_epoch - elapsed_time_epoch;

    parser_set_uint8(parser, 0x22); // Flags
    parser_set_uint48(parser, seconds_since_ets_epoch);
    parser_set_uint8(parser, 0x06); // Cellular Network
    parser_set_uint8(parser, 0x00); // Tz/DST offset
}

GByteArray* parser_get_byte_array(Parser *parser) {
    return parser->bytes;
}

