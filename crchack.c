#include "crchack.h"

#include <ctype.h>
#include <getopt.h>
#include <limits.h>
#define __USE_MINGW_ANSI_STDIO 1 /* make MinGW happy */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bigint.h"
#include "crc.h"
#include "forge.h"

// stupid name?
long TARGETS[TARGETS_SIZE];

/**
 * Usage.
 */
static void help(char *argv0)
{
    fprintf(stderr, "usage: %s [options] file [new_checksum]\n", argv0);
    fprintf(stderr, "\n"
         "options:\n"
         "  -o pos    byte.bit offset of mutable input bits\n"
         "  -O pos    offset from the end of the input\n"
         "  -b l:r:s  specify bits at offsets l..r with step s\n"
         "  -h        show this help\n"
         "  -v        verbose mode\n"
         "\n"
         "CRC parameters (default: CRC-32):\n"
         "  -p poly   generator polynomial    -w size   register size in bits\n"
         "  -i init   initial register value  -x xor    final register XOR mask\n"
         "  -r        reverse input bytes     -R        reverse final register\n");
}

/**
 * Structure and functions for slices of bit indices.
 */
struct slice {
    ssize_t l;
    ssize_t r;
    ssize_t s;
};

static int parse_slice(const char *p, struct slice *slice);
static int parse_offset(const char *p, ssize_t *offset);
static size_t bits_of_slice(struct slice *slice, size_t end, size_t *bits);

/**
 * User input and options.
 */
static struct {
    u8 *msg;
    size_t len;

    struct crc_params crc;
    struct bigint checksum;
    int has_checksum;

    size_t *bits;
    size_t nbits;

    struct slice *slices;
    size_t nslices;

    int verbose;
} input;

/**
 * Parse command-line arguments.
 *
 * Returns an exit code (0 for success).
 */
static u8 *read_input_message(FILE *in, size_t *length);
static int handle_options(int argc, char *argv[])
{
    size_t nbits;
    ssize_t offset;
    int has_offset;
    size_t i, width;
    char c, *checksum, *input_fn;
    char *poly, *init, reflect_in, reflect_out, *xor_out;

    offset = 0;
    has_offset =  0;
    checksum = input_fn = NULL;

    /* CRC parameters */
    width = 0;
    poly = init = xor_out = NULL;
    reflect_in = reflect_out = 0;
    memset(&input, 0, sizeof(input));

  memset(&TARGETS, -1, sizeof(TARGETS));

    /* Parse command options */
    while ((c = getopt(argc, argv, "hvp:w:i:x:rRo:O:b:")) != -1) {
        switch (c) {
        case 'h': help(argv[0]); return 1;
        case 'v': input.verbose++; break;
        case 'p': poly = optarg; break;
        case 'w':
            if (sscanf(optarg, "%zu", &width) != 1) {
                fprintf(stderr, "invalid CRC width '%s'\n", optarg);
                return 1;
            }
            break;
        case 'i': init = optarg; break;
        case 'x': xor_out = optarg; break;
        case 'r': reflect_in = 1; break;
        case 'R': reflect_out = 1; break;
        case 'o': case 'O':
            if (has_offset) {
                fprintf(stderr, "multiple -oO not allowed\n");
                return 1;
            }
            if (!parse_offset(optarg, &offset)) {
                fprintf(stderr, "invalid offset '%s'\n", optarg);
                return 1;
            }
            has_offset = c;
            break;
        case 'b':
            if (!(input.nslices & (input.nslices + 1))) {
                struct slice *new;
                if (input.slices == NULL) {
                    new = malloc(64 * sizeof(struct slice));
                } else {
                    size_t capacity = 2*(input.nslices + 1);
                    new = realloc(input.slices, capacity*sizeof(struct slice));
                }
                if (!new) {
                    fprintf(stderr, "out-of-memory allocating slice %zu\n",
                            input.nslices + 1);
                    return 1;
                }
                input.slices = new;
            }
            if (!parse_slice(optarg, &input.slices[input.nslices])) {
                fprintf(stderr, "invalid slice '%s'\n", optarg);
                return 1;
            }
            input.nslices++;
            break;

        case '?':
            if (strchr("oObwpix", optopt)) {
                fprintf(stderr, "option -%c requires an argument\n", optopt);
            } else if (isprint(optopt)) {
                fprintf(stderr, "unknown option '-%c'\n", optopt);
            } else {
                fprintf(stderr, "unknown option character '\\x%x'\n", optopt);
            }
            return 1;
        default:
            help(argv[0]);
            return 1;
        }
    }

    /* Determine input file argument position */
    if (optind == argc || optind+2 < argc) {
        help(argv[0]);
        return 1;
    }
    checksum = (optind == argc-2) ? argv[argc-1] : NULL;
    input_fn = argv[optind];

    /* CRC parameters */
    if (!width && poly) {
        const char *p = poly + ((poly[0] == '0' && poly[1] == 'x') << 1);
        size_t span = strspn(p, "0123456789abcdefABCDEF");
        if (!span || p[span] != '\0') {
            fprintf(stderr, "invalid poly (%s)\n", poly);
            return 1;
        }
        width = span * 4;
    }
    input.crc.width = width ? width : 32;
    bigint_init(&input.crc.poly, input.crc.width);
    bigint_init(&input.crc.init, input.crc.width);
    bigint_init(&input.crc.xor_out, input.crc.width);
    if (width || poly || init || xor_out || reflect_in || reflect_out) {
        if (!poly) {
            fprintf(stderr, "custom CRC requires generator polynomial\n");
            return 1;
        }

        /* CRC generator polynomial */
        if (!bigint_from_string(&input.crc.poly, poly)) {
            fprintf(stderr, "invalid poly (%s)\n", poly);
            return 1;
        }

        /* Initial CRC register value */
        if (init && !bigint_from_string(&input.crc.init, init)) {
            fprintf(stderr, "invalid init (%s)\n", init);
            return 1;
        }

        /* Final CRC register XOR mask */
        if (xor_out && !bigint_from_string(&input.crc.xor_out, xor_out)) {
            fprintf(stderr, "invalid xor_out (%s)\n", xor_out);
            return 1;
        }

        /* Reflect in and out */
        input.crc.reflect_in = reflect_in;
        input.crc.reflect_out = reflect_out;
    } else {
        /* Default: CRC-32 */
        bigint_from_string(&input.crc.poly, "04c11db7");
        bigint_load_ones(&input.crc.init);
        bigint_load_ones(&input.crc.xor_out);
        input.crc.reflect_in = 1;
        input.crc.reflect_out = 1;
    }

    /* Read input mesasge */
    if (!strcmp(input_fn, "-")) {
        if (!(input.msg = read_input_message(stdin, &input.len))) {
            fprintf(stderr, "reading message from stdin failed\n");
            return 2;
        }
    } else {
        FILE *in;
        if (!(in = fopen(input_fn, "rb"))) {
            fprintf(stderr, "opening file '%s' for read failed\n", input_fn);
            return 2;
        }
        input.msg = read_input_message(in, &input.len);
        fclose(in);
        if (!input.msg) {
            fprintf(stderr, "reading file '%s' failed\n", input_fn);
            return 2;
        }
    }

    /* Read checksum value */
    if (checksum) {
        bigint_init(&input.checksum, input.crc.width);
        if (!bigint_from_string(&input.checksum, checksum)) {
            bigint_destroy(&input.checksum);
            fprintf(stderr, "checksum '%s' is not a valid %d-bit hex value\n",
                    checksum, input.crc.width);
            return 1;
        }
        input.has_checksum = 1;
    } else {
        if (has_offset) fprintf(stderr, "flags -oO ignored\n");
        if (input.slices) fprintf(stderr, "flag -b ignored\n");
        return 0;
    }

    /* Determine (upper bound for) size of the input.bits array */
    nbits = (has_offset || !input.nslices) ? input.crc.width : 0;
    for (i = 0; i < input.nslices; i++)
        nbits += bits_of_slice(&input.slices[i], input.len * 8, NULL);

    /* Fill input.bits */
    if (nbits) {
        /* Read bit indices from '-b' slices */
        if (!(input.bits = calloc(nbits, sizeof(size_t)))) {
            fprintf(stderr, "allocating bits array failed\n");
            return 4;
        }

        for (i = 0; i < input.nslices; i++) {
            size_t n = bits_of_slice(&input.slices[i], input.len * 8,
                                     &input.bits[input.nbits]);
            input.nbits += n;
        }

        /* Handle '-oO' offsets */
        if (has_offset || !input.slices) {
            if (has_offset != 'o')
                offset = input.len*8 - offset;
            for (i = 0; i < input.crc.width; i++)
                input.bits[input.nbits++] = offset + i;
        }
    }

    /* Verbose info */
    if (input.verbose >= 1) {
        fprintf(stderr, "len(msg) = %zu bytes\n", input.len);
        fprintf(stderr, "bits[%zu] = {", input.nbits);
        for (i = 0; i < input.nbits; i++)
            fprintf(stderr, &", %zu.%zu"[!i], input.bits[i]/8, input.bits[i]%8);
        fprintf(stderr, " }\n");

      for (int i = 0; i < input.len; ++i) {
        if (TARGETS[i] != -1) {
          fprintf(stderr, "TARGETS[%d]: 0x%02x ^ 0x%02x => 0x%02x\n", i, input.msg[i], TARGETS[i] , TARGETS[i] ^ input.msg[i]);
          TARGETS[i] ^= input.msg[i];
        }
      }
    }

    return 0;
}

/**
 * Recursive descent parser for slices (-b).
 */
static const char peek(const char **pp)
{
    while (**pp == ' ') pp++;
    return **pp;
}

static const char *parse_expression(const char *p, ssize_t *value);
static const char *parse_factor(const char *p, ssize_t *value)
{
    switch (peek(&p)) {
    case '0': case'1': case '2': case '3': case '4':
    case '5': case'6': case '7': case '8': case '9':
        if (p[0] == '0' && p[1] == 'x') {
            sscanf(p, "0x%zx", (size_t *)value);
            p += 2;
            while (strchr("0123456789abcdefABCDEF", *p)) p++;
        } else {
            sscanf(p, "%zd", value);
            while (*p >= '0' && *p <= '9') p++;
        }
        break;
    case '(':
        if ((p = parse_expression(p+1, value))) {
            if (peek(&p) != ')') {
                fprintf(stderr, "missing ')' in slice\n");
                return NULL;
            }
            p++;
        }
        break;
    default:
        fprintf(stderr, "unexpected character '%c' in slice\n", *p);
        return NULL;
    }
    return p;
}

static const char *parse_unary(const char *p, ssize_t *value)
{
    char op = peek(&p);
    if (op == '+' || op == '-') {
        p = parse_unary(p+1, value);
        if (op == '-') *value = -(*value);
    } else {
        p = parse_factor(p, value);
    }
    return p;
}

static const char *parse_muldiv(const char *p, ssize_t *value)
{
    if ((p = parse_unary(p, value))) {
        ssize_t rhs;
        char op = peek(&p);
        while ((op == '*' || op == '/') && (p = parse_unary(p+1, &rhs))) {
            *value = (op == '*') ? *value * rhs : *value / rhs;
            op = peek(&p);
        }
    }
    return p;
}

static const char *parse_addsub(const char *p, ssize_t *value)
{
    if ((p = parse_muldiv(p, value))) {
        ssize_t rhs;
        char op = peek(&p);
        while ((op == '+' || op == '-') && (p = parse_muldiv(p+1, &rhs))) {
            *value = (op == '+') ? *value + rhs : *value - rhs;
            op = peek(&p);
        }
    }
    return p;
}

static const char *parse_expression(const char *p, ssize_t *value)
{
    return parse_addsub(p, value);
}

static const char *parse_slice_offset(const char *p, ssize_t *offset)
{
    if (peek(&p) == '.') {
        p = parse_expression(p+1, offset);
    } else if ((p = parse_expression(p, offset))) {
        *offset *= 8;
        if (peek(&p) == '.') {
            ssize_t byteoffset = *offset;
            if ((p = parse_expression(p+1, offset)))
                *offset += byteoffset;
        }
    }
    if (p && peek(&p) != '\0' && peek(&p) != ':') {
        fprintf(stderr, "junk '%s' after slice offset\n", p);
        return NULL;
    }
    return p;
}

static int parse_offset(const char *p, ssize_t *offset)
{
    if ((p = parse_slice_offset(p, offset)) && peek(&p)) {
        fprintf(stderr, "junk '%s' after offset\n", p);
        return 0;
    }
    return 1;
}

static int parse_slice(const char *p, struct slice *slice)
{
    int relative;

    slice->s = 1;
    slice->l = 0;

    /* L:r:s */
    if (!peek(&p) || (*p != ':' && !(p = parse_slice_offset(p, &slice->l))))
        return 0;
    slice->r = !peek(&p) ? slice->l+1 : ((size_t)SIZE_MAX >> 1);
    p += *p == ':';

    /* l:R:s */
    if (!peek(&p)) return 1;
    relative = peek(&p) == '+';
    if (*p != ':' && !(p = parse_slice_offset(p, &slice->r)))
        return 0;
    if (relative)
        slice->r += slice->l;
    p += *p == ':';

    /* l:r:S */
    if (!peek(&p)) return 1;

  u8 target;
  sscanf(p, "%x", &target);

  if (DEBUG)
    fprintf(stderr, "-b %d:%d:0x%02x\n", slice->l, slice->r, target);

  for (int i = slice->l / 8; i < slice->r / 8; ++i) {
    TARGETS[i] = target;
  }
  return 1;

    if (peek(&p)) {
        fprintf(stderr, "junk '%s' after slice\n", p);
        return 0;
    }

    return 1;
}

/**
 * Extract bits of a slice.
 *
 * Returns the number of bits in the given slice.
 */
static size_t bits_of_slice(struct slice *slice, size_t end, size_t *bits)
{
    size_t n = 0;
    ssize_t l = slice->l, r = slice->r, s = slice->s;
    if (s == 0) {
        fprintf(stderr, "slice step cannot be zero\n");
        return 0;
    }

    if (l < 0 && (l += end) < 0) l = 0;
    else if (l > (ssize_t)end) l = end;
    if (r < 0 && (r += end) < 0) r = 0;
    else if (r > (ssize_t)end) r = end;

    while ((s > 0 && l < r) || (s < 0 && l > r)) {
        if (bits) *bits++ = l;
        l += s;
        n++;
    }

    return n;
}

/**
 * Read input message from stream 'in' and store its length to *length.
 *
 * Returns a pointer to the read message which must be freed with free().
 * If the read fails, the return value is NULL.
 */
static u8 *read_input_message(FILE *in, size_t *length)
{
    size_t capacity, *size, tmp;
    u8 *msg, *new;

    /* Read using a dynamic buffer (to support non-seekable streams) */
    if (!(msg = malloc((capacity = 1024))))
        return NULL;

    *(size = length ? length : &tmp) = 0;
    while (!feof(in)) {
        if (*size == capacity) {
            if (!(new = realloc(msg, (capacity = capacity*2))))
                goto fail;
            msg = new;
        }
        *size += fread(&msg[*size], sizeof(u8), capacity-*size, in);
        if (ferror(in))
            goto fail;
    }

    /* Truncate */
    if (*size > 0 && *size < capacity && (new = realloc(msg, *size)))
        msg = new;
    return msg;

fail:
    free(msg);
    return NULL;
}

static void crc_checksum(const u8 *msg, size_t length, struct bigint *checksum)
{
    crc(msg, length, &input.crc, checksum);
}

int main(int argc, char *argv[])
{
    int exit_code, i, j, ret;
    u8 *out = NULL;

    /* Parse command-line */
    if ((exit_code = handle_options(argc, argv)))
        goto finish;

    /* Calculate CRC and exit if no new checksum given */
    if (!input.has_checksum) {
        struct bigint checksum;
        bigint_init(&checksum, input.crc.width);

        crc(input.msg, input.len, &input.crc, &checksum);
        bigint_print(&checksum); puts("");
        bigint_destroy(&checksum);

        exit_code = 0;
        goto finish;
    }

    /* Check bits array and pad the message buffer if needed */
    for (i = j = 0; i < input.nbits; i++) {
        if (input.bits[i] > input.len*8 + input.crc.width - 1) {
            fprintf(stderr, "bits[%d]=%zu exceeds message length (%zu bits)\n",
                    i, input.bits[i], input.len*8 + input.crc.width - 1);
            exit_code = 3;
            goto finish;
        }

        if (input.bits[i] > input.bits[j])
            j = i;
    }
    if (input.nbits && input.bits[j] >= input.len*8) {
        u8 *new;
        int pad_size = 1 + (input.bits[j] - input.len*8) / 8;
        if (!(new = realloc(input.msg, input.len + pad_size))) {
            fprintf(stderr, "reallocating message buffer failed\n");
            exit_code = 4;
            goto finish;
        }
        input.msg = new;

        memset(&input.msg[input.len], 0, pad_size);
        input.len += pad_size;

        if (input.verbose >= 1) {
            fprintf(stderr, "input message padded by %d bytes\n", pad_size);
        }
    }

    /* Allocate output buffer for the modified message */
    if (!(out = malloc(input.len))) {
        fprintf(stderr, "allocating output buffer failed\n");
        exit_code = 4;
        goto finish;
    }

    /* Forge */
    ret = forge(input.msg, input.len, &input.checksum, crc_checksum,
                input.bits, input.nbits, out);

    /* Show flipped bits */
    if (input.verbose >= 1) {
        for (i = 0; i < ret; i++) {
            for (j = i; j < ret; j++) {
                if (input.bits[j] < input.bits[i]) {
                    size_t tmp = input.bits[i];
                    input.bits[i] = input.bits[j];
                    input.bits[j] = tmp;
                }
            }
        }
        fprintf(stderr, "flip[%d] = {", ret);
        for (i = 0; i < ret; i++)
            fprintf(stderr, &", %zu.%zu"[!i], input.bits[i]/8, input.bits[i]%8);
        fprintf(stderr, " }\n");
    }

    if (ret >= 0) {
        /* Write the result to stdout */
        u8 *ptr = &out[0];
        size_t left = input.len;
        while (left > 0) {
            size_t written = fwrite(ptr, sizeof(u8), left, stdout);
            if (written <= 0 || ferror(stdout)) {
                fprintf(stderr, "writing result to stdout failed\n");
                exit_code = 5;
                goto finish;
            }
            left -= written;
            ptr += written;
        }
        fflush(stdout);
    } else {
        fprintf(stderr, "FAIL! try giving %d mutable bits more (got %zu)\n",
                -ret, input.nbits);
        exit_code = 6;
        goto finish;
    }

    /* Success! */
    exit_code = 0;

finish:
    bigint_destroy(&input.checksum);
    bigint_destroy(&input.crc.poly);
    bigint_destroy(&input.crc.init);
    bigint_destroy(&input.crc.xor_out);
    free(input.slices);
    free(input.bits);
    free(input.msg);
    free(out);
    return exit_code;
}
