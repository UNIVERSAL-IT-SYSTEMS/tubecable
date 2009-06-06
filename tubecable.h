/*
 * libtubecable - displaylink protocol reference implementation
 *
 * version 0.1.2 - more efficient Huffman table by Henrik Pedersen
 *                 fixed two more encoder glitches
 *                 June 6th, 2009
 *
 * version 0.1.1 - added missing Huffman sequences
 *                 fixed 2 bugs in encoder
 *                 June 5th, 2009
 *
 * version 0.1   - initial public release
 *                 May 30th, 2009
 *
 * written 2008/09 by floe at butterbrot.org
 * in cooperation with chrisly at platon42.de
 * this code is released as public domain.
 *
 * this is so experimental that the warranty shot itself.
 * so don't expect any.
 *
 */

#include <usb.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


/******************** ENCRYPTION STUFF ********************/

// Key sequence for disabling encryption.
#define DL_CRYPT_NULLKEY { 0x57, 0xCD, 0xDC, 0xA7, 0x1C, 0x88, 0x5E, 0x15, 0x60, 0xFE, 0xC6, 0x97, 0x16, 0x3D, 0x47, 0xF2 }
extern uint8_t  dl_crypt_nullkey[16];

// Key sequence and reverse-mapping table.
extern uint8_t  dl_crypt_keybuffer[0x11000];
extern uint16_t dl_crypt_ofsbuffer[0x1000];

// Generate a CRC12 over len bytes of data. Generator polynom:
// x^12 + x^11 + x^3 + x^2 + x + 1 = 0001 1000 0000 1111 = 0x180F
#define DL_CRYPT_CRC12 0x180F
int dl_crypt_crc12( uint8_t* data, int len ); 

// Fill key buffer and reverse-mapping buffer with pseudorandom numbers.
#define DL_CRYPT_LFSR12 0x0829
void dl_crypt_generate_key( uint8_t key[0x11000], uint16_t map[0x1000] );


/********************* CONTROL STUFF **********************/

// Read one byte of in-device memory (wraps after 64k).
uint8_t dl_ctrl_peek( usb_dev_handle* handle, int addr, int timeout = 1000 );

// Write one byte of in-device memory.
void dl_ctrl_poke( usb_dev_handle* handle, int addr, uint8_t value, int timeout = 1000 );

// Dump the entire 64k of in-device memory to a file.
void dl_ctrl_dumpmem( usb_dev_handle* handle, char* f ); 

// Retrieve device status word.
int dl_ctrl_status( usb_dev_handle* handle, int timeout = 1000 );

// Set encryption key.
void dl_ctrl_set_key( usb_dev_handle* handle, uint8_t key[16], int timeout = 1000 );

// Unknown purpose.
void dl_ctrl_unknown( usb_dev_handle* handle, int timeout = 1000 ); 

// Read EDID from attached display.
void dl_ctrl_get_edid( usb_dev_handle* handle, uint8_t edid[128], int timeout = 1000 );


/********************* COMMAND BUFFER *********************/

// Command stream descriptor.
typedef struct {
	uint8_t* buffer;
	int pos, bitpos, size;
} dl_cmdstream;


// Initialize a new command buffer and allocate space.
void create( dl_cmdstream* cs, int size );

// Delete the command buffer.
void destroy( dl_cmdstream* cs );

// Send a command buffer to the device.
void send( usb_dev_handle* handle, dl_cmdstream* cs, int ep = 1, int timeout = 1000 ); 

// Insert one byte into the command buffer.
inline void insertb( dl_cmdstream* cs, uint8_t val ) {
	cs->buffer[cs->pos++] = val;
}

// Insert one word into the command buffer.
inline void insertw( dl_cmdstream* cs, uint16_t val ) {
	insertb( cs, (val >> 8) & 0xFF );
	insertb( cs, (val     ) & 0xFF );
}

// Insert an device memory address into the command buffer.
inline void inserta( dl_cmdstream* cs, uint32_t address ) {
	insertb( cs, (address >> 16) & 0xFF );
	insertb( cs, (address >>  8) & 0xFF );
	insertb( cs, (address      ) & 0xFF );
}

// Insert a doubleword into the command buffer.
inline void insertd( dl_cmdstream* cs, uint32_t val ) {
	insertb( cs, (val >> 24) & 0xFF );
	insertb( cs, (val >> 16) & 0xFF );
	insertb( cs, (val >>  8) & 0xFF );
	insertb( cs, (val      ) & 0xFF );
}

// Insert a sequence of bytes into the command buffer.
inline void insert( dl_cmdstream* cs, int size, uint8_t* buf ) {
	memcpy( cs->buffer+cs->pos, buf, size );
	cs->pos += size;
}


/************************ COMMANDS ************************/

#define DL_CMD_START   0xAF // start-of-command token

#define DL_CMD_SET_REG 0x20 // set register
#define DL_CMD_UNKNOWN 0x40 // unknown purpose 
#define DL_CMD_SYNC    0xA0 // sync/flush/exec
#define DL_CMD_HUFFMAN 0xE0 // set Huffman table

#define DL_HUFF_MAGIC  0x263871CD // probably magic number


/********************* MISC COMMANDS **********************/

// Unknown purpose.
void dl_cmd_unknown( dl_cmdstream* cs );

// Flush/synchronize/execute all commands up to this point.
void dl_cmd_sync( dl_cmdstream* cs );


/******************* REGISTER COMMANDS ********************/

#define DL_REG_COLORDEPTH   0x00 // 0x00 = 16 bit, 0x01 = 24 bit
// 0x01 - 0x0E unknown
//   0x01 - 0x0
#define DL_REG_XRES_MSB     0x0F
#define DL_REG_XRES_LSB     0x10
// 0x11 - 0x16 unknown
#define DL_REG_YRES_MSB     0x17
#define DL_REG_YRES_LSB     0x18
// 0x19 - 0x1C unknown
// 0x1D - 0x1E unused
#define DL_REG_BLANK_SCREEN 0x1F // 0x00 = normal operation, 0x01 = blank screen
// 0x20 - 0xFE unused
#define DL_REG_SYNC         0xFF // 0x00 = hold register updates, 0xFF = resume


// Set a single register.
void dl_reg_set( dl_cmdstream* cs, uint8_t reg, uint8_t val );

// Set all mode registers at once.
void dl_reg_set_all( dl_cmdstream* cs, uint8_t values[0x1D] );


// The unknown registers very likely contain pixel clock, sync polarity etc.
// While the mapping hasn't been found yet, some default register sets for 
// standard resolutions are given below.

// Modes for DL-120:

#define DL_REG_MODE_640x480_60   { 0x00,  0x99, 0x30, 0x26, 0x94,  0x60, 0xa9, 0xce, 0x60,  0x07, 0xb3, 0x0f, 0x79, 0xff, 0xff,  0x02, 0x80,  0x83, 0xbc, 0xff, 0xfc, 0xff, 0xff,  0x01, 0xe0,  0x01, 0x02,  0xab, 0x13 }
extern uint8_t dl_reg_mode_640x480_60[0x1D];
// 60 hz                           0x01,  0x30, 0x1d, 0x94, 0x07,  0xa9, 0x55, 0x60, 0xef,  0x07, 0xb3, 0x0f, 0x79, 0xff, 0xff,  0x02, 0x80,  0x83, 0xbc, 0xff, 0xfc, 0xff, 0xff,  0x01, 0xe0,  ...........  0xab, 0x13
// 73 hz                           0x01,  0x2b, 0xeb, 0x35, 0xd3,  0x0a, 0x95, 0xe6, 0x0e,  0x0f, 0xb5, 0x15, 0x2a, 0xff, 0xff,  0x02, 0x80,  0xcc, 0x1d, 0xff, 0xf9, 0xff, 0xff,  0x01, 0xe0,  ...........  0x9c, 0x18
// 75 hz                           0x01,  0xeb, 0xf7, 0xd3, 0x0f,  0x4f, 0x93, 0xfa, 0x47,  0xb5, 0x58, 0xbf, 0x70, 0xff, 0xff,  0x02, 0x80,  0xf4, 0x8f, 0xff, 0xf9, 0xff, 0xff,  0x01, 0xe0,  ...........  0x9c, 0x18
// 85 hz                           0x01,  0x93, 0x99, 0xd7, 0x26,  0x26, 0xc1, 0x8f, 0x9c,  0x0f, 0xb5, 0xb9, 0xbf, 0xff, 0xff,  0x02, 0x80,  0x1f, 0x39, 0xff, 0xf9, 0xff, 0xff,  0x01, 0xe0,  ...........  0x20, 0x1c

#define DL_REG_MODE_800x480_60   { 0x00,  0x20, 0x3c, 0x7a, 0xc9,  0xf2, 0x6c, 0x48, 0xf9,  0x70, 0x53, 0xff, 0xff, 0x21, 0x27,  0x03, 0x20,  0x91, 0xf3, 0xff, 0xff, 0xff, 0xf9,  0x01, 0xe0,  0x01, 0x02,  0xc8, 0x19 }
extern uint8_t dl_reg_mode_800x480_60[0x1D];
// 60 hz                           0x01,  0xff, 0x29, 0x66, 0x6b,  0xfe, 0x53, 0x13, 0x3e,  0xd3, 0x0f, 0xff, 0xff, 0xff, 0xf9,  0x03, 0x20,  0x33, 0xe9, 0xff, 0xff, 0xff, 0xfe,  0x01, 0xe0,  ...........  0x24, 0x13

#define DL_REG_MODE_800x600_60   { 0x00,  0x20, 0x3c, 0x7a, 0xc9,  0x93, 0x60, 0xc8, 0xc7,  0x70, 0x53, 0xff, 0xff, 0x21, 0x27,  0x03, 0x20,  0x91, 0x8f, 0xff, 0xff, 0xff, 0xf2,  0x02, 0x58,  0x01, 0x02,  0x40, 0x1f }
extern uint8_t dl_reg_mode_800x600_60[0x1D];
// 56 hz                           0x01,  0x65, 0x35, 0x48, 0xf4,  0xf2, 0x6c, 0x19, 0x18,  0xc9, 0x4b, 0xff, 0xff, 0x70, 0x35,  0x03, 0x20,  0x32, 0x31, 0xff, 0xff, 0xff, 0xfc,  0x02, 0x58,  ...........  0x20, 0x1c
// 60 hz                           0x01,  0x20, 0x3c, 0x7a, 0xc9,  0x93, 0x60, 0xc8, 0xc7,  0x70, 0x53, 0xff, 0xff, 0x21, 0x27,  0x03, 0x20,  0x91, 0x8f, 0xff, 0xff, 0xff, 0xf2,  0x02, 0x58,  ...........  0x40, 0x1f
// 72 hz                           0x01,  0xeb, 0xf7, 0xd1, 0x90,  0x4d, 0x82, 0x23, 0x1f,  0x39, 0xcf, 0xff, 0xff, 0x43, 0x21,  0x03, 0x20,  0x62, 0xc5, 0xff, 0xff, 0xff, 0xca,  0x02, 0x58,  ...........  0x10, 0x27
// 75 hz                           0x01,  0xb3, 0x76, 0x39, 0xcf,  0xf2, 0x6c, 0x19, 0x18,  0x70, 0x53, 0xff, 0xff, 0x35, 0x33,  0x03, 0x20,  0x32, 0x31, 0xff, 0xff, 0xff, 0xf9,  0x02, 0x58,  ...........  0xac, 0x26
// 85 hz                           0x01,  0x20, 0x3c, 0x7a, 0xc9,  0x9b, 0x05, 0x46, 0x3f,  0xcf, 0x70, 0xff, 0xff, 0xbf, 0x70,  0x03, 0x20,  0x8c, 0x7f, 0xff, 0xff, 0xff, 0xf9,  0x02, 0x58,  ...........  0xf2, 0x2b

#define DL_REG_MODE_1024x768_60  { 0x00,  0x36, 0x18, 0xd5, 0x10,  0x60, 0xa9, 0x7b, 0x33,  0xa1, 0x2b, 0x27, 0x32, 0xff, 0xff,  0x04, 0x00,  0xd9, 0x9a, 0xff, 0xca, 0xff, 0xff,  0x03, 0x00,  0x04, 0x03,  0xc8, 0x32 }
extern uint8_t dl_reg_mode_1024x768_60[0x1D];
// 60 hz                           0x01,  0x36, 0x18, 0xd5, 0x10,  0x60, 0xa9, 0x7b, 0x33,  0xa1, 0x2b, 0x27, 0x32, 0xff, 0xff,  0x04, 0x00,  0xd9, 0x9a, 0xff, 0xca, 0xff, 0xff,  0x03, 0x00,  ...........  0xc8, 0x32
// 70 hz                           0x01,  0xb4, 0xed, 0x4c, 0x5e,  0x60, 0xa9, 0x7b, 0x33,  0x10, 0x4d, 0x27, 0x32, 0xff, 0xff,  0x04, 0x00,  0xd9, 0x9a, 0xff, 0xca, 0xff, 0xff,  0x03, 0x00,  ...........  0x98, 0x3a
// 75 hz                           0x01,  0xec, 0xb4, 0xa0, 0x4c,  0x36, 0x0a, 0x07, 0xb3,  0x5e, 0xd5, 0xff, 0xff, 0x0f, 0x79,  0x04, 0x00,  0x0f, 0x66, 0xff, 0xff, 0xff, 0xf9,  0x03, 0x00,  ...........  0x86, 0x3d
// 85 hz                           0x01,  0x18, 0xdc, 0x10, 0x4d,  0x0a, 0x95, 0xb3, 0x35,  0x79, 0x01, 0xff, 0xff, 0x0f, 0x79,  0x04, 0x00,  0x66, 0x6b, 0xff, 0xff, 0xff, 0xf9,  0x03, 0x00,  ...........  0xd4, 0x49

#define DL_REG_MODE_1280x1024_60 { 0x00,  0x98, 0xf8, 0x0d, 0x57,  0x2a, 0x55, 0x4d, 0x54,  0xca, 0x0d, 0xff, 0xff, 0x94, 0x43,  0x05, 0x00,  0x9a, 0xa8, 0xff, 0xff, 0xff, 0xf9,  0x04, 0x00,  0x04, 0x02,  0x60, 0x54 }
extern uint8_t dl_reg_mode_1280x1024_60[0x1D];
// 60 hz                           0x01,  0x98, 0xf8, 0x0d, 0x57,  0x2a, 0x55, 0x4d, 0x54,  0xca, 0x0d, 0xff, 0xff, 0x94, 0x43,  0x05, 0x00,  0x9a, 0xa8, 0xff, 0xff, 0xff, 0xf9,  0x04, 0x00,  ...........  0x60, 0x54
// 75 hz                           0x01,  0xce, 0x12, 0x3f, 0x9f,  0x2a, 0x55, 0x4d, 0x54,  0xca, 0x0d, 0xff, 0xff, 0x32, 0x60,  0x05, 0x00,  0x9a, 0xa8, 0xff, 0xff, 0xff, 0xf9,  0x04, 0x00,  ...........  0x78, 0x69
// 85 hz                           0x01,  0x24, 0xce, 0x77, 0x3f,  0x95, 0x5c, 0x55, 0x1f,  0x9e, 0x64, 0xff, 0xff, 0x3a, 0x66,  0x05, 0x00,  0xaa, 0x3f, 0xff, 0xff, 0xff, 0xf9,  0x04, 0x00,  ...........  0x0c, 0x7b

#define DL_REG_MODE_1360x768_60  { 0x01,  0xf8, 0x42, 0x9e, 0x64,  0xf2, 0x6c, 0x28, 0x0f,  0xe8, 0x61, 0xff, 0xff, 0x94, 0x43,  0x05, 0x50,  0x40, 0x7b, 0xff, 0xff, 0xff, 0xca,  0x03, 0x00,  0x04, 0x02,  0xcc, 0x42 }
extern uint8_t dl_reg_mode_1360x768_60[0x1D];

#define DL_REG_MODE_1366x768_60  { 0x01,  0x19, 0x1e, 0x1f, 0xb0,  0x93, 0x60, 0x40, 0x7b,  0x36, 0xe8, 0x27, 0x32, 0xff, 0xff,  0x05, 0x56,  0x03, 0xd9, 0xff, 0xff, 0xfc, 0xa7,  0x03, 0x00,  0x04, 0x02,  0x9a, 0x42 }
extern uint8_t dl_reg_mode_1366x768_60[0x1D];

#define DL_REG_MODE_1400x1050_60 { 0x01,  0x42, 0x24, 0x38, 0x36,  0xc1, 0x52, 0xd9, 0x29,  0xea, 0xb8, 0x32, 0x60, 0xff, 0xff,  0x05, 0x78,  0xc9, 0x4e, 0xff, 0xff, 0xff, 0xf2,  0x04, 0x1a,  0x04, 0x02,  0x1e, 0x5f }
extern uint8_t dl_reg_mode_1400x1050_60[0x1D];
// 1400x1050 60 hz                 0x01,  0xca, 0x21, 0x90, 0x9f,  0x93, 0x60, 0x47, 0xec,  0xd5, 0x03, 0xff, 0xff, 0x6c, 0x15,  0x05, 0x78,  0x3f, 0x64, 0xff, 0xf2, 0xff, 0xff,  0x04, 0x1a,  0x04, 0x02,  0xe8, 0x4e

// Modes for DL-160:

#define DL_REG_MODE_1600x1200_60 { 0x01,  0xcf, 0xa4, 0x3c, 0x4e,  0x55, 0x73, 0x71, 0x2b,  0x71, 0x52, 0xff, 0xff, 0xee, 0xca,  0x06, 0x40,  0xe2, 0x57, 0xff, 0xff, 0xff, 0xf9,  0x04, 0xb0,  0x04, 0x02,  0x90, 0x7e }
extern uint8_t dl_reg_mode_1600x1200_60[0x1D];

#define DL_REG_MODE_1920x1080_60 { 0x01,  0x73, 0xa6, 0x28, 0xb3,  0x54, 0xaa, 0x41, 0x5d,  0x0d, 0x9f, 0x32, 0x60, 0xff, 0xff,  0x07, 0x80,  0x0a, 0xea, 0xff, 0xf9, 0xff, 0xff,  0x04, 0x38,  0x04, 0x02,  0xe0, 0x7c }
extern uint8_t dl_reg_mode_1920x1080_60[0x1D];


/******************* ADDRESS REGISTERS ********************/

#define DL_ADDR_FB16_START  0x20 // 16-bit mode, color MSBs, RGB 565
#define DL_ADDR_FB16_STRIDE 0x23 // 16-bit stride = 2*xres
#define DL_ADDR_FB8_START   0x26 // additional 8 bit for 24-bit mode, color LSBs, RGB 323
#define DL_ADDR_FB8_STRIDE  0x29 // 8-bit stride = 1*xres

// Set a single address register.
void dl_reg_set_address( dl_cmdstream* cs, uint8_t reg, int address );

// Set all address registers at once.
void dl_reg_set_offsets( dl_cmdstream* cs, int start16, int stride16, int start8, int stride8 );


/******************* GRAPHICS COMMANDS ********************/

#define DL_GFX_BASE 0x60 // base graphics command
#define DL_GFX_WORD 0x08 // word-mode flag
#define DL_GFX_COMP 0x10 // compressed-mode flag

#define DL_GFX_WRITE (DL_GFX_BASE | 0x00) // write memory
#define DL_GFX_RLE   (DL_GFX_BASE | 0x01) // write RLE-encoded data
#define DL_GFX_COPY  (DL_GFX_BASE | 0x02) // internal copy

// Insert a generic GFX command into the stream.
void dl_gfx_base( dl_cmdstream* cs, uint8_t cmd, int addr, uint8_t count ); 

// Insert a raw-write command into the stream.
void dl_gfx_write( dl_cmdstream* cs, int addr, uint8_t count, uint8_t* data );

// Descriptor for RLE-encoded data.
typedef struct {
	uint8_t count;
	uint16_t value;
} dl_rle_word;

// Insert a RLE-encoded write command into the stream.
void dl_gfx_rle( dl_cmdstream* cs, int addr, uint8_t count, dl_rle_word* rs );

// Insert a on-device memcopy command into the stream.
void dl_gfx_copy( dl_cmdstream* cs, int src_addr, int dst_addr, uint8_t count );


/****************** COMPRESSION COMMANDS ******************/

#define DL_HUFFMAN_COUNT (1<<15)                // number of encoded offsets (pos/neg)
#define DL_HUFFMAN_SIZE  (2*DL_HUFFMAN_COUNT+1) // total number of Huffman sequences

#define DL_HUFFMAN_BLOCKSIZE 512 // maximum size of one compressed block


// The on-device Huffman table.
extern uint8_t dl_huffman_device_table[4608];

// Set the on-device Huffman table.
void dl_huffman_set_device_table( dl_cmdstream* cs, int size, uint8_t* buf );

// Descriptor for one Huffman sequence.
typedef struct {
	int bitcount;
	const char* sequence;
} dl_huffman_code;

// The userspace Huffman table.
extern dl_huffman_code  dl_huffman_storage[ DL_HUFFMAN_SIZE ];
extern dl_huffman_code* dl_huffman_table;

// Load the userspace Huffman table.
int dl_huffman_load_table( const char* filename );

// Append one huffman bit sequence to the stream.
void dl_huffman_append( dl_cmdstream* cs, int16_t value );


// Append one 512-byte block of compressed data to the stream.
int dl_huffman_compress( dl_cmdstream* cs, int addr, int pcount, uint16_t* pixels, int blocksize = DL_HUFFMAN_BLOCKSIZE );


/******************** HELPER FUNCTIONS ********************/

// get a device handle according to vendor and product
usb_dev_handle* usb_get_device_handle( int vendor, int product, int interface = 0 );

// convert 24-bit rgb data to 16-bit rgb 565 data.
// host bit order (uint16_t) for compression is the default, data sent 
// to the device from a little-endian machine needs to clear this flag
void rgb24_to_rgb16( uint8_t* rgb24, uint8_t* rgb16, int count, int host_bit_order = 1 );

// read raw 24-bit data from a file
void read_rgb24( const char* filename, uint8_t* rgb24, int count );

// read rgb565 data from a 24-bit file. host bit order: see above
uint8_t* read_rgb16( const char* filename, int count, int host_bit_order = 1 );


/**************** INITIALIZATION SEQUENCE *****************/

// Send a default init sequence to a DisplayLink device.
void dl_init( usb_dev_handle* handle ); 

