// TODO: Handle multiple types of SD cards
/* Ideas:
 * -8 512kb sectors are used to store actual ROM data
 * -Then a 9th sector is used to store metadata about ROM, such as config
 * -9th sector can also hold user flags
 * -Metadata sector will appear first before ROM data
 * -Create Python program to add/remove ROMs from SD and change config
 * -Use dearpygui for GUI?
 * -Erase ROM by shifting all ROMS left 9 sectors
 */

#include <stdio.h>
#include "sd.h"
#include "gpio.h"
#include "delay.h"

// CS: B12
// SCK: B13
// MISO: B14 // PUR needed? Probably not since using breakout
// MOSI: B15 // Keep high during read transfer (after sending command)?

#define SPI_CLK    (1 << 14)
#define SPI2_START 0x40003800
#define SPI2_CR1   (*((volatile uint32_t *)(SPI2_START + 0x00)))
#define SPI2_CR2   (*((volatile uint32_t *)(SPI2_START + 0x04)))
#define SPI2_SR    (*((volatile uint32_t *)(SPI2_START + 0x08)))
#define SPI2_DR    (*((volatile uint32_t *)(SPI2_START + 0x0C)))

#define RESET_DUMMY_CYCLES 10
#define START_BITS 0x40
#define STOP_BITS 0x01
#define NUM_ARGS 4
#define NUM_R3_RESP_BYTES 5
#define READ_BYTE_DELAY 8
#define CARD_IDLE 1
#define CMD_OK 0
#define CMD_17_OK 0xFE

struct command {
    uint8_t cmd_bits;
    uint8_t args[NUM_ARGS];
    uint8_t crc;
};

const struct command GO_IDLE_STATE = {
    0,
    {0x00, 0x00, 0x00, 0x00},
    0x4A
};

const struct command SEND_IF_COND = {
    8,
    {0x00, 0x00, 0x01, 0xAA},
    0x43
};

const struct command APP_CMD = {
    55,
    {0x00, 0x00, 0x00, 0x00},
    0xFF
};

const struct command SD_SEND_OP_COND = {
    41,
    {0x40, 0x00, 0x00, 0xA0},
    0xFF
};

const struct command READ_OCR = {
    58,
    {0x00, 0x00, 0x00, 0x00},
    0xFF
};

const struct command READ_SINGLE_BLOCK = {
    17,
    {0x00, 0x00, 0x00, 0x00}, // These will be replaced by addr bytes
    0xFF
};

static void _gpio_init(void) {
    // Disable reset state
    GPIOB_CRH &= ~((1 << 18) | (1 << 22) | (1 << 30));

    // MODEy (12, 13, 15 2MHz out, 14 in)
    GPIOB_CRH |= ((1 << 17) | (1 << 21) | (1 << 29));

    // CNFy (13, 15 alt out, 14 floating in)
    GPIOB_CRH |= ((1 << 23) | (1 << 31));
}

static void _spi_init1(void) {
    RCC_APB1ENR |= SPI_CLK;
    for (volatile int i = 0; i < 10; i++);

    // CLK / 128
    // SD must be initialized with a clk speed of between 100-400KHz
    // When CPU clock is set to 72MHz, APB1 clock is set to 36Mhz
    // 36MHz / 128 equals roughly 280KHz
    SPI2_CR1 |= (3 << 4);

    SPI2_CR1 |= (1 << 9); // Enable software CS
    SPI2_CR2 |= (1 << 2); // Enable CS output
    SPI2_CR1 |= (1 << 2); // Set as master
    SPI2_CR1 |= (1 << 6); // Enable
}

static void _spi_init2(void) {
    // Wait for SPI to finish up then disable it
    delay(10);
    SPI2_CR1 &= ~(1 << 6);

    // Change CS pin to alt function output
    GPIOB_CRH |= (1 << 19);

    // Change the frequency to something much faster
    SPI2_CR1 &= ~(3 << 4); // Erase old freq settings
    SPI2_CR1 |= (1 << 5); // CLK / 32 (might be able to go faster)

    SPI2_CR1 &= ~(1 << 9); // Disable software CS (thus enabling hardware CS)
    SPI2_CR1 |= (1 << 6); // Re-enable SPI
}

static void _dummy_write(int n) {
    for (int i = 0; i < n; i++)
        sd_write(0xFF);
}

static void _rest(void) {
    // Wait until SD is sending out a constant high signal which means ready
    while (sd_read() != 0xFF)
        _dummy_write(1);
}

static uint8_t _read_R1(void) {
    /* Must keep writing until receive a valid response,
     * or 8 bytes have been written (max time a response can take)
     * We detect a valid response by looking for a 0 in the 8th bit */
    uint8_t resp = sd_read();
    for (int i = 0; i < READ_BYTE_DELAY && (resp & (1 << 7)); i++) {
        _dummy_write(1);
        resp = sd_read();
    }

    return resp;
}

static const uint8_t* _read_R3(void) {
    static uint8_t resp[NUM_R3_RESP_BYTES];

    // First read the response byte. The read the next 4 data bytes.
    resp[0] = _read_R1();
    for (int i = 1; i < NUM_R3_RESP_BYTES; i++) {
        _dummy_write(1);
        resp[i] = sd_read();
    }

    return resp;
}

static void _power_on(void) {
    GPIOB_ODR |= (1 << 12); // Set CS high

    // Send >74 dummy clocks with MOSI high
    _dummy_write(RESET_DUMMY_CYCLES);

    // Stabilize
    delay(10);
}

static void _send_cmd(const struct command *cmd, const uint8_t *args) {
    // Wait for SD to be ready to receive command
    _rest();

    // The full command byte must start with the start bits
    sd_write(START_BITS | cmd->cmd_bits);

    // Send arguments
    // Use default arguments if none provided
    for (int i = 0; i < NUM_ARGS; i++) {
        if (args)
            sd_write(args[i]);
        else
            sd_write(cmd->args[i]);
    }

    // The full CRC byte must end with the stop bits
    sd_write((cmd->crc << 1) | STOP_BITS);
}

static bool _reset(void) {
    // Some garbage comes in on MISO when MCU is reset without power loss
    // So do a few writes to discard it
    _dummy_write(3);

    _send_cmd(&GO_IDLE_STATE, NULL);

    // Ensure SD is now in idle state
    return _read_R1() == CARD_IDLE;
}

static bool _verify(void) {
    _send_cmd(&SEND_IF_COND, NULL);

    // If the last byte is 0xAA (which means our card is SD2+), SD card is good
    return _read_R3()[NUM_R3_RESP_BYTES - 1] == 0xAA;
}

static bool _initialize(void) {
    /* The below sequence begins the SD initilization process.
     * It must be repeated until R1 returns 0
     * (signifying SD is no longer idle and ready to accept all commands) */
    do {
        _send_cmd(&APP_CMD, NULL);
        _read_R1();
        _send_cmd(&SD_SEND_OP_COND, NULL);
    } while (_read_R1() == CARD_IDLE); // Should implement timeout just in case

    _send_cmd(&READ_OCR, NULL);

    // Ensure SD is no longer idle and CCS is 1
    const uint8_t *resp = _read_R3();
    return ((resp[0] == CMD_OK) && (resp[1] & (1 << 6)));
}

static bool _wait_for_data_token(uint8_t token) {
    while (sd_read() != token)
        _dummy_write(1);
    
    return true; // Todo: Return false if timeout
}

bool sd_init(void) {
    if (!sd_inserted())
        return false;

    _gpio_init();
    _spi_init1();
    _power_on();

    // Set CS low manually since we aren't in full-blown SPI yet
    GPIOB_ODR &= ~(1 << 12);

    // Ensure all stages of sequence were successful
    if (!_reset())
        return false;
    if (!_verify())
        return false;
    if (!_initialize())
        return false;

    // Reinitialize SPI with a much faster frequency and hardware CS
    _spi_init2();
    return true;
}

void sd_write(uint8_t data) {
    SPI2_DR = data;
    while (!(SPI2_SR & 0x02));
}

uint8_t sd_read(void) {
    return SPI2_DR;
}

bool sd_inserted(void) {
    return (GPIOA_IDR & (1 << 8));
}

void sd_read_block(uint16_t addr, uint8_t *buffer) {
    uint8_t args[NUM_ARGS];

    // Split addr into 4 argument bytes
    for (int i = 0; i < NUM_ARGS; i++)
        args[i] = (addr >> (24 - (i * 8))) & 0xFF;

    /* Send a read command and ensure we get an OK response,
     * wait for the beginning of the data packet,
     * then read data bytes into buffer. */
    _send_cmd(&READ_SINGLE_BLOCK, args);
    if (_read_R1() == CMD_OK && _wait_for_data_token(CMD_17_OK)) {
        for (int i = 0; i < SD_BLOCK_SIZE; i++) {
            _dummy_write(1);
            buffer[i] = sd_read();
        }

        // Have to read the 2 byte CRC so send a couple dummy writes
        _dummy_write(2);
    }
}