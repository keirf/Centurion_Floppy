#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BC_BUF_SIZE_BYTES (2 * 1024 * 1024)

#include "fdc9216.h"
#include "ff_v341.h"
#include "ff_master.h"
#include "ff_master_greaseweazle_default_pll.h"
#include "ff_master_greaseweazle_fallback_pll.h"
#include "nco_178k.h"
#include "nco_358k.h"
#include "nco_715k.h"
#include "nco_1440k_0p2.h"
#include "nco_1440k_0p25.h"
#include "nco_2160k_0p1.h"
#include "nco_2160k_0p2.h"
#include "nco_2160k_0p25.h"
#include "nco_2160k_0p5.h"
#include "nco_2160k_1p0.h"
#include "nco_generic.h"

struct algorithm
{
    const char *name;
    uint32_t (*func)(uint16_t write_bc_ticks, uint16_t *ff_samples, size_t ff_sample_count, uint32_t *bc_buf, uint32_t bc_buf_mask);
};

static struct algorithm ALGS[] = {
    {"ff_v341", &ff_v341},
    {"ff_master", &ff_master},
    {"ff_master_greaseweazle_default_pll", ff_master_greaseweazle_default_pll},
    {"ff_master_greaseweazle_fallback_pll", ff_master_greaseweazle_fallback_pll},
    {"fdc9216", fdc9216},
    {"nco_715k", nco_715k},
    {"nco_358k", nco_358k},
    {"nco_178k", nco_178k},
    {"nco_1440k_0p2", nco_1440k_0p2},
    {"nco_1440k_0p25", nco_1440k_0p25},
    {"nco_2160k_0p1", nco_2160k_0p1},
    {"nco_2160k_0p2", nco_2160k_0p2},
    {"nco_2160k_0p25", nco_2160k_0p25},
    {"nco_2160k_0p5", nco_2160k_0p5},
    {"nco_2160k_1p0", nco_2160k_1p0},
    {NULL, NULL},
};

void usage(const char *const progname)
{
    fprintf(stderr, "Usage: %s <ff_samples> <hfe_out> <hfe-bit-rate-kbps> <algorithm>\n", progname);
    fprintf(stderr, "\n");
    fprintf(stderr, "Algorithms:\n");

    for (struct algorithm *alg = ALGS; alg->name != NULL; alg += 1)
    {
        fprintf(stderr, "\t* %s\n", alg->name);
    }

    exit(1);
}

int main(int argc, const char *const argv[])
{
    if (argc < 5)
    {
        usage(argv[0]);
    }

    const char *const ff_sample_path = argv[1];
    const char *const hfe_path = argv[2];
    unsigned long hfe_bit_rate_kbps = strtoul(argv[3], NULL, 10);
    const char *const algorithm = argv[4];

    // Open sample input file
    FILE *ff_sample_fd = fopen(ff_sample_path, "rb");
    if (ff_sample_fd == NULL)
    {
        fprintf(stderr, "ERROR: Unable to open ff samples file: %s\n", ff_sample_path);
        return 1;
    }

    // Figure out how big the sample file is.
    if (fseek(ff_sample_fd, 0, SEEK_END) < 0)
    {
        fprintf(stderr, "ERROR: failed to seek to end of ff sample file: %s\n", strerror(errno));
        return 1;
    }
    long ff_sample_size = ftell(ff_sample_fd);
    if (ff_sample_size < 0)
    {
        fprintf(stderr, "ERROR: unable to read current ff sample file position: %s\n", strerror(errno));
        return 1;
    }
    rewind(ff_sample_fd);

    // Allocate memory for samples
    size_t ff_sample_count = ff_sample_size / sizeof(uint16_t);
    uint16_t *ff_samples = calloc(ff_sample_count, sizeof(uint16_t));
    if (ff_samples == NULL)
    {
        fprintf(stderr, "ERROR: failed to allocate memory for %lu samples\n", ff_sample_count);
        return 1;
    }

    // Read samples into buffer
    {
        int res = fread(ff_samples, sizeof(uint16_t), ff_sample_count, ff_sample_fd);
        if (res < 0)
        {
            fprintf(stderr, "ERROR: error reading samples from file: %s\n", strerror(errno));
            return 1;
        }
        else if (res != ff_sample_count)
        {
            fprintf(stderr, "ERROR: only read %d samples of %lu expected\n", res, ff_sample_count);
            return 1;
        }
        fclose(ff_sample_fd);
    }

    /* Process the flux timings into the raw bitcell buffer. */

    printf("Starting to process flux to bitcells\n");

    uint32_t *bc_buf = malloc(BC_BUF_SIZE_BYTES);
    uint32_t bc_bufmask = (BC_BUF_SIZE_BYTES / 4) - 1;
    uint16_t write_bc_ticks = (500*72) / hfe_bit_rate_kbps;
    uint32_t bc_prod;

    if (!strncmp(algorithm, "nco[", 4)) {
        char *p = (char *)algorithm + 4;
        int integral_div, error_div;
        integral_div = strtol(p, &p, 10);
        error_div = strtol(p+1, &p, 10);
        printf("NCO: Integral/%d, Error/%d\n", integral_div, error_div);
        bc_prod = nco_generic((uint16_t)write_bc_ticks, ff_samples, ff_sample_count, bc_buf, bc_bufmask, integral_div, error_div);
    } else {

        struct algorithm *alg = ALGS;
        while (alg->name != NULL)
        {
            if (strcmp(algorithm, alg->name) == 0)
            {
                break;
            }
            alg += 1;
        }

        if (alg->name == NULL)
        {
            fprintf(stderr, "Unknown algorithm: %s\n", algorithm);
            return 1;
        }

        printf("Running %s with write_bc_ticks=%hu\n", alg->name, write_bc_ticks);
        bc_prod = alg->func((uint16_t)write_bc_ticks, ff_samples, ff_sample_count, bc_buf, bc_bufmask);

    }

    printf("Decoded %u bitcells\n", bc_prod);
    if (bc_prod / 4 >= BC_BUF_SIZE_BYTES)
    {
        fprintf(stderr, "ERROR: decoded more bitcells than buffer space\n");
        return 1;
    }

    /* Round up to next byte and word in case last byte is partial */
    size_t bc_bytes = (bc_prod + 7) / 8;
    size_t bc_words = bc_bytes / 4;

    /* Write HFE */
    FILE *hfe_fd = fopen(hfe_path, "w");
    if (hfe_fd == NULL)
    {
        fprintf(stderr, "ERROR: unable to open output HFE file: %s\n", strerror(errno));
        return 1;
    }

    const uint8_t header[] = {
        'H',
        'X',
        'C',
        'P',
        'I',
        'C',
        'F',
        'E',
        /* Revision */ 0x0,
        /* Number of tracks */ 0x1,
        /* Number of sides */ 0x1,
        /* Track encoding */ 0xFF /* Unknown */,
        /* Bitrate (kbps) */ hfe_bit_rate_kbps,
        0x01, /* 500 */
        /* RPM */ 0x00,
        0x00,
        /* Interface mode */ 0x07 /* GENERIC_SHUGGART_DD_FLOPPYMODE */,
        /* Reserved */ 0,
        /* Track list offset */ 0x01,
        0x00,
    };
    fwrite(header, sizeof(header), 1, hfe_fd);

    // Track list
    size_t track_data_length_bytes = bc_bytes * 2;
    const uint8_t track_list[] = {
        /* Track data offset */ 0x02,
        0x00,
        /* Track data length */ (track_data_length_bytes & 0xFF),
        (track_data_length_bytes >> 8) & 0xFF,
    };
    fseek(hfe_fd, 0x200, SEEK_SET);
    fwrite(track_list, sizeof(track_list), 1, hfe_fd);

    // Track data
    for (int ii = 0; ii < bc_words; ++ii)
    {
        uint32_t bits_left_to_right = be32toh(bc_buf[ii]);
        uint8_t bits_out[4];

        for (int jj = 31; jj >= 0; --jj, bits_left_to_right >>= 1)
        {
            int bit = bits_left_to_right & 1;

            int byte_idx = jj / 8;
            bits_out[byte_idx] = bits_out[byte_idx] << 1 | bit;
        }

        long byte_number = ii * 4;
        long block_number = byte_number / 256;
        long offset = 0x400 + (block_number * 512) + (byte_number % 256);
        //printf("Writing word %d (data: 0x%x, 0x%x 0x%x 0x%x 0x%x) to offset 0x%lx\n", ii, be32toh(bc_buf[ii]), bits_out[0], bits_out[1], bits_out[2], bits_out[3], offset);
        fseek(hfe_fd, offset, SEEK_SET);
        fwrite(bits_out, 4, 1, hfe_fd);
    }

    fclose(hfe_fd);

    return 0;
}
