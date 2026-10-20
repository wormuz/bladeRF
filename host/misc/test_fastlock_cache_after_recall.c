#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board/bladerf2/fastlock_cache_policy.h"

/* Reproduces the live -5 failure: 20 get_quick_tune saves rotate through
 * NIOS slots 0..15 (AD9361 slot = NIOS slot % 8), then a profile captured
 * at the first frequency is loaded into a NIOS slot that a save already
 * filled, and a retune2 NOW recalls it.
 *
 * Model of the three copies of a profile:
 *  - bank[r]: the AD9361 fastlock slot, what the PLL actually uses;
 *  - nios[n]: the NIOS BBP copy that retune2 uploads into bank[n % 8];
 *  - cache.entry[0][r]: the ADI driver cache. ad9361_fastlock_store()
 *    fills it via ad9361_fastlock_load(); ad9361_fastlock_get_freq()
 *    decodes it instead of the bank whenever FASTLOOK_INIT is set
 *    (ad9361.c, "phy->fastlock.entry[...].flags & FASTLOOK_INIT"). */

#define SAVES 20

static uint8_t bank[8][16];
static uint8_t nios[16][16];
static struct ad9361_fastlock cache;
static struct bladerf2_nios_fastlock_words host_words[2][16];
static int failures;

static void words_for(unsigned int frequency_index, uint8_t *words)
{
    unsigned int i;

    for (i = 0; i < 16; i++) {
        words[i] = (uint8_t)(0x10 * (frequency_index + 1) + i);
    }
}

/* bladerf2_get_quick_tune in HOST mode. */
static void get_quick_tune(unsigned int nios_slot, unsigned int frequency)
{
    const unsigned int rffe = nios_slot % 8;
    uint8_t words[16];

    words_for(frequency, words);
    host_words[0][nios_slot].valid = false;
    /* store_fastlock_profile -> ad9361_fastlock_store -> fastlock_load */
    memcpy(bank[rffe], words, 16);
    cache.entry[0][rffe].flags = FASTLOOK_INIT;
    memcpy(cache.entry[0][rffe].profile_data, words, 16);
    /* rffe_fastlock_save: NIOS reads the bank into its slot */
    memcpy(nios[nios_slot], bank[rffe], 16);
    /* bladerf2_record_nios_fastlock_words */
    memcpy(host_words[0][nios_slot].words, cache.entry[0][rffe].profile_data,
           16);
    host_words[0][nios_slot].valid = true;
}

/* bladerf2_load_quick_tune_profile */
static void load_profile(unsigned int nios_slot, const uint8_t *words)
{
    memcpy(nios[nios_slot], words, 16);
    memcpy(host_words[0][nios_slot].words, words, 16);
    host_words[0][nios_slot].valid = true;
}

/* retune2 NOW, then the host readback that failed with -EIO. Returns true
 * when the words the driver decodes are the words the PLL runs on. */
static bool retune_and_readback(unsigned int nios_slot, bool with_fix)
{
    const unsigned int rffe = nios_slot % 8;
    const uint8_t *decoded;

    memcpy(bank[rffe], nios[nios_slot], 16); /* NIOS upload + recall */
    if (with_fix) {
        bladerf2_fastlock_cache_after_nios_recall(&cache.entry[0][rffe],
                                                  &host_words[0][nios_slot]);
    }
    decoded = (cache.entry[0][rffe].flags & FASTLOOK_INIT)
        ? cache.entry[0][rffe].profile_data : bank[rffe];
    return memcmp(decoded, bank[rffe], 16) == 0;
}

static void run_sequence(bool with_fix, unsigned int *mismatches)
{
    uint8_t first[16];
    unsigned int k;

    memset(&cache, 0, sizeof(cache));
    memset(host_words, 0, sizeof(host_words));
    *mismatches = 0;
    for (k = 0; k < SAVES; k++) {
        get_quick_tune(k % 16, k);
    }
    /* get_quick_tune_profile of the first capture (925 MHz in the log). */
    words_for(0, first);
    for (k = 0; k < 12; k++) {
        load_profile(k, first);
        if (!retune_and_readback(k, with_fix)) {
            (*mismatches)++;
        }
    }
    /* Plain quick-tune rotation: NIOS slot 4 shares AD9361 slot 4 with
     * NIOS slot 12, which was saved later. */
    if (!retune_and_readback(12, with_fix) ||
        !retune_and_readback(4, with_fix)) {
        (*mismatches)++;
    }
}

int main(void)
{
    unsigned int mismatches;
    struct ad9361_fastlock_entry entry = { 0 };
    struct bladerf2_nios_fastlock_words unknown = { { 0 }, false };

    run_sequence(false, &mismatches);
    if (mismatches == 0) {
        fputs("model does not reproduce the stale driver cache\n", stderr);
        failures++;
    }
    run_sequence(true, &mismatches);
    if (mismatches != 0) {
        fprintf(stderr, "%u readbacks still decode stale words\n",
                mismatches);
        failures++;
    }

    /* Unknown NIOS words: drop the cache so the driver reads the bank. */
    entry.flags = FASTLOOK_INIT;
    bladerf2_fastlock_cache_after_nios_recall(&entry, &unknown);
    if (entry.flags != 0) {
        fputs("unknown NIOS words left the cache trusted\n", stderr);
        failures++;
    }
    entry.flags = FASTLOOK_INIT;
    bladerf2_fastlock_cache_after_nios_recall(&entry, NULL);
    if (entry.flags != 0) {
        fputs("missing NIOS slot left the cache trusted\n", stderr);
        failures++;
    }

    if (failures != 0) {
        return 1;
    }
    puts("fastlock driver cache after NIOS recall: PASS");
    return 0;
}
