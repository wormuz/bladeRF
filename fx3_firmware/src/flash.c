/*
 * Copyright (c) 2013 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <string.h>
#include "cyu3os.h"
#include "cyu3spi.h"
#include "cyu3error.h"
#include "bladeRF.h"
#include "spi_flash_lib.h"
#include "flash.h"
#include "misc.h"

#define THIS_FILE LOGGER_ID_FLASH_C

static CyU3PReturnStatus_t FlashTransfer(uint8_t *tx, uint16_t tx_len,
                                        uint8_t *rx, uint16_t rx_len)
{
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
    CyU3PReturnStatus_t ssn_status;

    if ((tx_len != 0 && tx == NULL) || (rx_len != 0 && rx == NULL)) {
        return CY_U3P_ERROR_BAD_ARGUMENT;
    }

    status = CyU3PSpiSetSsnLine(CyFalse);
    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    if (tx_len != 0) {
        status = CyU3PSpiTransmitWords(tx, tx_len);
    }
    if (status == CY_U3P_SUCCESS && rx_len != 0) {
        status = CyU3PSpiReceiveWords(rx, rx_len);
    }

    ssn_status = CyU3PSpiSetSsnLine(CyTrue);
    return status != CY_U3P_SUCCESS ? status : ssn_status;
}

static CyU3PReturnStatus_t FlashReadStatus(uint8_t *val)
{
    uint8_t read_status = 0x05; /* RDSTATUS */
    if (val == NULL) {
        return CY_U3P_ERROR_BAD_ARGUMENT;
    }
    return FlashTransfer(&read_status, 1, val, 1);
}

static CyU3PReturnStatus_t FlashWaitUntilReady(void)
{
    CyU3PReturnStatus_t status;
    uint8_t read_status;
    uint32_t start = CyU3PGetTime();

    do {
        status = FlashReadStatus(&read_status);
        if (status != CY_U3P_SUCCESS) {
            return status;
        }
        if ((uint32_t)(CyU3PGetTime() - start) >= 30000) {
            return CY_U3P_ERROR_FAILURE;
        }
    } while (read_status & 0x01);

    return CY_U3P_SUCCESS;
}

CyU3PReturnStatus_t NuandLockOtpMacronix() {
    CyU3PReturnStatus_t status;
    uint8_t location[1];
    uint8_t read_status;

    status = FlashReadStatus(&read_status);
    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    status = CyFxSpiWaitForStatus();
    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    status = FlashReadStatus(&read_status);
    if (status != CY_U3P_SUCCESS) {
        return status;
    }
    if (!(read_status & 0x02)) {
        return CY_U3P_ERROR_FAILURE;
    }

    location[0] = 0x2f;// WRSCUR
    status = FlashTransfer(location, 1, NULL, 0);
    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    location[0] = 0x02;// Page program
    status = FlashTransfer(location, 1, NULL, 0);
    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    location[0] = 0x2b; /* RDSCUR */
    return FlashTransfer(location, 1, location, 1);
}

CyU3PReturnStatus_t NuandLockOtpWinbond() {
    CyU3PReturnStatus_t status;
    uint8_t location[2];
    uint8_t val[2];

    location[0] = 0x35; // Read Status Register 2
    status = FlashTransfer(location, 1, val, 1);
    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    val[0] |= 0x8; // Set LB-1

    status = CyFxSpiWaitForStatus();
    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    location[0] = 0x31; // Write Status Register 2
    location[1] = val[0];

    status = FlashTransfer(location, 2, NULL, 0);

    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    status = FlashWaitUntilReady();

    return status;
}

static CyU3PReturnStatus_t NuandLockOtpRenesas() {
    CyU3PReturnStatus_t status;
    uint8_t cmd[5];
    uint8_t lock_val = 0x00;

    status = CyFxSpiWaitForStatus();
    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    cmd[0] = 0x9B; /* Program OTP Security Register */
    cmd[1] = 0x00; /* A[23:16] don't care */
    cmd[2] = 0x00; /* A[15:8], A[8]=0 */
    cmd[3] = 0xFF; /* A[7:0]: A[7]=1 (reg 1), A[6:0]=0x7F (byte 127) */
    cmd[4] = lock_val;

    status = FlashTransfer(cmd, sizeof(cmd), NULL, 0);
    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    return FlashWaitUntilReady();
}

CyU3PReturnStatus_t NuandLockOtp() {
    uint8_t mfn = NuandGetSPIManufacturer();
    if (mfn == 0xEF) {
        return NuandLockOtpWinbond();
    } else if (mfn == 0x1F) {
        return NuandLockOtpRenesas();
    }
    return NuandLockOtpMacronix();
}

CyU3PReturnStatus_t NuandEnso() {
    uint8_t location = 0xb1; // ENSO
    return FlashTransfer(&location, 1, NULL, 0);
}

CyU3PReturnStatus_t NuandExso() {
    uint8_t location = 0xc1; // EXSO
    return FlashTransfer(&location, 1, NULL, 0);
}

static CyU3PReturnStatus_t NuandEraseWBSec(void) {
    uint8_t location[4];
    CyU3PReturnStatus_t status;

    location[0] = 0x44; // Erase security register
    location[1] = 0x0;
    location[2] = 0x1f;
    location[3] = 0x0;

    status = CyFxSpiWaitForStatus();
    if (status != CY_U3P_SUCCESS) {
        return status;
    }
    status = FlashTransfer(location, sizeof(location), NULL, 0);
    if (status != CY_U3P_SUCCESS) {
        return status;
    }
    return FlashWaitUntilReady();
}

static uint8_t spi_mfn[2];
static CyBool_t cached_spi_mfn = CyFalse;
CyU3PReturnStatus_t cacheSPIManufacturer(void) {
    uint8_t location[4];
    CyU3PReturnStatus_t status;

    location[0] = 0x90; // RD Manu
    location[1] = 0x0;
    location[2] = 0x0;
    location[3] = 0x0;

    status = FlashTransfer(location, 4, spi_mfn, 2);
    if (status != CY_U3P_SUCCESS) {
        cached_spi_mfn = CyFalse;
        return status;
    }

    cached_spi_mfn = CyTrue;
    return CY_U3P_SUCCESS;
}

uint8_t NuandGetSPIManufacturer() {
    if (!cached_spi_mfn) {
        if (cacheSPIManufacturer() != CY_U3P_SUCCESS) {
            return 0;
        }
    }
    return spi_mfn[0];
}

uint8_t NuandGetSPIDeviceID() {
    if (!cached_spi_mfn) {
        if (cacheSPIManufacturer() != CY_U3P_SUCCESS) {
            return 0;
        }
    }
    return spi_mfn[1];
}

CyU3PReturnStatus_t NuandReadOtp(size_t offset, size_t size, void *buf) {
    CyU3PReturnStatus_t status;

    if (size != 0 && buf == NULL) {
        return CY_U3P_ERROR_BAD_ARGUMENT;
    }
    if (size == 0) {
        return CY_U3P_SUCCESS;
    }

    if (NuandGetSPIManufacturer() == 0x1F) {
        uint8_t cmd[5];

        if (size < 128) {
            return CY_U3P_ERROR_BAD_ARGUMENT;
        }

        cmd[0] = 0x4B; /* Read OTP Security Register */
        cmd[1] = 0x00; /* A[23:16] don't care */
        cmd[2] = 0x00; /* A[15:9] don't care, A[8]=0 */
        cmd[3] = 0x80; /* A[7]=1 (reg 1), A[6:0]=0 (byte 0) */
        cmd[4] = 0x00; /* dummy byte */

        status = FlashTransfer(cmd, sizeof(cmd), buf, 128);
        if (status != CY_U3P_SUCCESS) {
            return status;
        }

        memset((uint8_t *)buf + 128, 0xFF, size > 128 ? size - 128 : 0);
        return status;
    }

    if (NuandGetSPIManufacturer() == 0xEF) {
        offset = (1 << 12) / FLASH_PAGE_SIZE;
    } else {
        status = NuandEnso();
        if (status != CY_U3P_SUCCESS) {
            return status;
        }
    }

    status = CyFxSpiTransfer(offset, size, buf, CyTrue, CyTrue);

    if (NuandGetSPIManufacturer() != 0xEF) {
        CyU3PReturnStatus_t exso_status = NuandExso();
        if (status == CY_U3P_SUCCESS) {
            status = exso_status;
        }
    }

    return status;
}

CyU3PReturnStatus_t NuandWriteOtp(size_t offset, size_t size, void *buf) {
    CyU3PReturnStatus_t status;

    if (size != 0 && buf == NULL) {
        return CY_U3P_ERROR_BAD_ARGUMENT;
    }
    if (size == 0) {
        return CY_U3P_SUCCESS;
    }

    if (NuandGetSPIManufacturer() == 0x1F) {
        uint8_t cmd[4 + 127];
        /* Cap at 127 bytes; byte 127 is the lock byte */
        size_t write_len = size < 127 ? size : 127;

        status = CyFxSpiWaitForStatus();
        if (status != CY_U3P_SUCCESS) {
            return status;
        }

        cmd[0] = 0x9B; /* Program OTP Security Register */
        cmd[1] = 0x00; /* A[23:16] */
        cmd[2] = 0x00; /* A[15:8], A[8]=0 */
        cmd[3] = 0x80; /* A[7]=1 (reg 1), A[6:0]=0 */
        memcpy(&cmd[4], buf, write_len);

        status = FlashTransfer(cmd, 4 + write_len, NULL, 0);
        if (status != CY_U3P_SUCCESS) {
            return status;
        }

        return FlashWaitUntilReady();
    }

    if (NuandGetSPIManufacturer() == 0xEF) {
        status = NuandEraseWBSec();
        if (status != CY_U3P_SUCCESS) {
            return status;
        }
        offset = (1 << 12) / FLASH_PAGE_SIZE;
    } else {
        status = NuandEnso();
        if (status != CY_U3P_SUCCESS) {
            return status;
        }
    }

    status = CyFxSpiTransfer(offset, size, buf, CyFalse, CyTrue);

    if (NuandGetSPIManufacturer() != 0xEF) {
        CyU3PReturnStatus_t exso_status = NuandExso();
        if (status == CY_U3P_SUCCESS) {
            status = exso_status;
        }
    }

    return status;
}

CyU3PReturnStatus_t NuandFlashInit() {
    CyU3PReturnStatus_t status;
    uint8_t mfn;

    NuandGPIOReconfigure(CyFalse, CyTrue);

    status = CyFxSpiInit();
    if (status != CY_U3P_SUCCESS) {
        return status;
    }

    status = cacheSPIManufacturer();
    if (status != CY_U3P_SUCCESS) {
        CyFxSpiDeInit();
        return status;
    }

    mfn = NuandGetSPIManufacturer();

    if (mfn == 0xEF || mfn == 0x1F) {
        status = CyU3PSpiSetClock(30000000);
        if (status != CY_U3P_SUCCESS) {
            CyFxSpiDeInit();
            return status;
        }
    }

    if (mfn == 0x1F) {
        uint8_t location[1];

        status = CyFxSpiWaitForStatus();
        if (status != CY_U3P_SUCCESS) {
            CyFxSpiDeInit();
            return status;
        }

        location[0] = 0x98; /* Global Block Unlock */
        status = FlashTransfer(location, 1, NULL, 0);
        if (status != CY_U3P_SUCCESS) {
            CyFxSpiDeInit();
            return status;
        }
    }

    glAppMode = MODE_FW_CONFIG;

    return status;
}

void NuandFlashDeinit() {
    CyFxSpiDeInit();
}

static inline size_t min_sz(size_t x, size_t y)
{
    return x < y ? x : y;
}

int NuandExtractField(char *ptr, int len, char *field,
                            char *val, size_t  maxlen) {
    int c, wlen;
    unsigned char *ub, *end;
    unsigned short a1, a2;
    int flen;

    flen = strlen(field);

    ub = (unsigned char *)ptr;
    end = ub + len;
    while (ub < end) {
        c = *ub;

        if (c == 0xff) // flash and OTP are 0xff if they've never been written to
            break;

        /* `c' comes straight from flash/OTP; make sure the record (1 byte
         * length + c data bytes + 2 byte CRC) actually fits before reading
         * past it, otherwise ub[c+1] can read beyond `end`. */
        if ((size_t)(end - ub) < (size_t)c + 3)
            return 1;

        a1 = *(unsigned short *)(&ub[c+1]);  // read checksum
        a2 = zcrc(ub, c+1);  // calculate checksum

        if (a1 == a2 || 1) {
            if (!strncmp((char *)ub + 1, field, flen)) {
                /* Field name longer than the stored record data means there
                 * is nothing to copy; avoid `c - flen' underflowing to a
                 * huge size_t in min_sz(), which previously caused
                 * strncpy()/val[wlen]=0 to overrun the caller's buffer. */
                if (c < flen)
                    return 1;
                wlen = min_sz(c - flen, maxlen > 0 ? maxlen - 1 : 0);
                strncpy(val, (char *)ub + 1 + flen, wlen);
                val[wlen] = 0;
                return 0;
            }
        } else {
            return 1;
        }
        ub += c + 3; //skip past `c' bytes, 2 byte CRC field, and 1 byte len field
    }
    return 1;
}
