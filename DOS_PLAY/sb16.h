#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <dos.h>
#include <pc.h>
#include <sys/movedata.h>

extern "C" {
	extern int __dpmi_allocate_dos_memory(int paragraphs, int *selector);
	extern int __dpmi_free_dos_memory(int selector);
}

static uint16_t sb_port = 0x220;
static int sb_irq = 5;
static int sb_dma8 = 1;
static int sb_dma16 = 5;

static int dma_selector = -1;
static uint32_t dma_buffer_phys = 0;
static int total_size = 32768;
static int block_size = 8192;   // = Groesse eines DSP-IRQ-Blocks; 4 Bloecke pro Puffer
static int write_pos = 0;       // (nur noch fuer Alt-Kompatibilitaet, ungenutzt)

// Position wird ueber gezaehlte Block-IRQs bestimmt, NICHT mehr ueber das
// DMA-Adressregister (das VirtualBox nicht feinkoernig genug aktualisiert).
static uint32_t bytes_written = 0;          // absolut in den Ring geschriebene Bytes
static volatile uint32_t bytes_played = 0;  // absolut abgespielte Bytes (irq_count * block_size)

static uint8_t orig_pic1_mask = 0xff;
static uint8_t orig_pic2_mask = 0xff;
static int pic_masked = 0;
static int play_called = 0;

static int sb_samplerate = 44100;
static int sb_channels = 2;
static int sb_bps = 44100 * 2 * 2;

static void mask_sb_irq(int irq) {
	orig_pic1_mask = inportb(0x21);
	orig_pic2_mask = inportb(0xa1);
	uint8_t new_pic1 = orig_pic1_mask | (1 << 5) | (1 << 7);
	if (irq < 8) {
		new_pic1 |= (1 << irq);
		outportb(0x21, new_pic1);
	} else {
		outportb(0x21, new_pic1);
		outportb(0xa1, orig_pic2_mask | (1 << (irq - 8)));
	}
	pic_masked = 1;
}

static void unmask_sb_irq(int irq) {
	if (!pic_masked) return;
	outportb(0x21, orig_pic1_mask);
	outportb(0xa1, orig_pic2_mask);
	pic_masked = 0;
}

static inline void delay_ms(int ms) {
	int lps = ms * 1000;
	for (volatile int i = 0; i < lps; i++) inportb(0x80);
}

static int sb16_dsp_reset(uint16_t port) {
	outportb(port + 0x6, 1);
	delay_ms(10);
	outportb(port + 0x6, 0);
	delay_ms(10);
	int timeout = 10000;
	while (timeout--) {
		if (inportb(port + 0xE) & 0x80) {
			if (inportb(port + 0xA) == 0xAA) return 1;
		}
	}
	return 0;
}

static inline void sb16_dsp_write(uint16_t port, uint8_t val) {
	while (inportb(port + 0xC) & 0x80);
	outportb(port + 0xC, val);
}

static int sb16_detect(void) {
	char *blaster = getenv("BLASTER");
	if (!blaster) return 1;
	char *p = blaster;
	while (*p) {
		if (*p == 'A' || *p == 'a') sb_port = strtol(p + 1, NULL, 16);
		else if (*p == 'I' || *p == 'i') sb_irq = strtol(p + 1, NULL, 10);
		else if (*p == 'D' || *p == 'd') sb_dma8 = strtol(p + 1, NULL, 10);
		else if (*p == 'H' || *p == 'h') sb_dma16 = strtol(p + 1, NULL, 10);
		p++;
		while (*p && *p != ' ') p++;
		while (*p && *p == ' ') p++;
	}
	return 1;
}

static int allocate_dma_buffer(int size) {
	int paragraphs = (size*2+15)/16;
	int segment = __dpmi_allocate_dos_memory(paragraphs, &dma_selector);
	if (segment == -1) return 0;
	uint32_t phys = (uint32_t)segment * 16;
	uint32_t start1 = phys;
	uint32_t end1 = phys + size - 1;
	if ((start1 / 131072) == (end1 / 131072)) {
		dma_buffer_phys = start1;
	} else {
		dma_buffer_phys = (start1 + 131071) & ~131071;
	}
	return 1;
}

static void free_dma_buffer(void) {
	if (dma_selector != -1) {
		__dpmi_free_dos_memory(dma_selector);
		dma_selector = -1;
		dma_buffer_phys = 0;
	}
}

static inline int dma_get_play_pos(void) {
	uint16_t addr_a, addr_b;
	do {
		outportb(0xD8, 0);
		addr_a  = inportb(0xC4);
		addr_a |= (inportb(0xC4) << 8);
		outportb(0xD8, 0);
		addr_b  = inportb(0xC4);
		addr_b |= (inportb(0xC4) << 8);
	} while (abs((int)addr_a - (int)addr_b) > 32);
	uint32_t phys_byte = (uint32_t)addr_b * 2;
	int offset = (int)((phys_byte - dma_buffer_phys) & (uint32_t)(total_size - 1));
	return offset;
}

static void dma_setup(uint32_t phys_addr, int size) {
	uint32_t word_addr = phys_addr >> 1;
	uint16_t word_count = size / 2;
	outportb(0xD4, 5);
	outportb(0xD6, 0x59);
	outportb(0xD8, 0);
	outportb(0x8B, phys_addr >> 16);
	outportb(0xC4, word_addr & 0xFF);
	outportb(0xC4, word_addr >> 8);
	outportb(0xC6, (word_count - 1) & 0xFF);
	outportb(0xC6, (word_count - 1) >> 8);
	outportb(0xD4, 1);
}

static int sb16_init(int rate, int channels) {
	sb_samplerate = rate;
	sb_channels = channels;
	sb_bps = rate * channels * 2;
	sb16_detect();
	mask_sb_irq(sb_irq);

	if (!sb16_dsp_reset(sb_port)) {
		printf("sb16: DSP reset failed on port 0x%X\n", sb_port);
		unmask_sb_irq(sb_irq);
		return 0;
	}

	if (!allocate_dma_buffer(total_size)) {
		printf("sb16: Failed to allocate DOS memory for DMA!\n");
		unmask_sb_irq(sb_irq);
		return 0;
	}

	uint8_t *temp = (uint8_t *)calloc(1, total_size);
	if (temp) {
		dosmemput(temp, total_size, dma_buffer_phys);
		free(temp);
	}

	dma_setup(dma_buffer_phys, total_size);

	sb16_dsp_write(sb_port, 0x41);
	sb16_dsp_write(sb_port, rate >> 8);
	sb16_dsp_write(sb_port, rate & 0xFF);

	write_pos = 0;
	bytes_written = 0;
	bytes_played = 0;
	play_called = 0;
	return 1;
}

static void sb16_start(void) {
	sb16_dsp_write(sb_port, 0xB6);
	if (sb_channels == 2) sb16_dsp_write(sb_port, 0x30);
	else sb16_dsp_write(sb_port, 0x10);

	// IRQ pro BLOCK (block_size Bytes), nicht pro vollem Puffer. So koennen wir
	// die Wiedergabeposition zuverlaessig aus der Anzahl der IRQs ableiten.
	// Laenge ist in 16-bit-Samples minus 1; block_size Bytes = block_size/2 Samples.
	uint16_t dsp_len = (block_size / 2) - 1;
	sb16_dsp_write(sb_port, dsp_len & 0xFF);
	sb16_dsp_write(sb_port, dsp_len >> 8);
}

static void sb16_uninit(void) {
	if (dma_buffer_phys) {
		sb16_dsp_write(sb_port, 0xD5);
		outportb(0xD4, 5);
		inportb(sb_port + 0xF);
		inportb(sb_port + 0xE);
		sb16_dsp_reset(sb_port);
		unmask_sb_irq(sb_irq);
		free_dma_buffer();
	}
}

// Pollt das SB16-Interrupt-Status-Register, quittiert anstehende Block-IRQs und
// zaehlt die abgespielten Bytes hoch. Muss oft genug aufgerufen werden, damit
// kein Block-IRQ verpasst wird (bei block_size=8192 -> ~46ms/Block, reichlich).
static inline void sb16_service(void) {
	outportb(sb_port + 0x4, 0x82);            // mixer index: IRQ status
	uint8_t st = inportb(sb_port + 0x5);      // mixer data
	if (st & 0x02) { inportb(sb_port + 0x0F); bytes_played += (uint32_t)block_size; } // 16-bit ack
	if (st & 0x01) { inportb(sb_port + 0x0E); }                                       // 8-bit ack
}

static int get_space(void) {
	if (!dma_buffer_phys) return 0;
	sb16_service();

	int buffered = (int)(bytes_written - bytes_played);
	if (buffered < 0) buffered = 0; // Underrun: aufgeholt
	int free_space = total_size - buffered;

	// Einen Block als Schutzabstand frei lassen, damit nie der gerade laufende
	// Block ueberschrieben wird.
	free_space -= block_size;
	if (free_space < 0) free_space = 0;
	return (free_space / block_size) * block_size;
}

static int sb16_play(void *data, int len) {
	if (!dma_buffer_phys) return 0;

	if (!play_called) {
		play_called = 1;
		// Prefill: bis zu (total_size - block_size) fuellen, einen Block Schutz lassen.
		int space = total_size - block_size;
		if (len > space) len = space;
	} else {
		int space = get_space();
		if (len > space) len = space;
	}
	if (len <= 0) return 0;

	int off = (int)(bytes_written % (uint32_t)total_size);
	int first = total_size - off;
	if (first > len) first = len;

	dosmemput(data, first, dma_buffer_phys + off);
	if (first < len) {
		dosmemput((uint8_t *)data + first, len - first, dma_buffer_phys);
	}
	bytes_written += (uint32_t)len;
	return len;
}

static float get_delay(void) {
	if (!dma_buffer_phys) return 0.0f;
	sb16_service();
	int buffered = (int)(bytes_written - bytes_played);
	if (buffered <= 0) return 0.0f;
	return (float)buffered / (float)sb_bps;
}
