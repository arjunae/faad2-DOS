// DOS 44khz AAC Player (DJGPP)
// i586-pc-msdosdjgpp-g++ -O2 dos_play.cc -o dos_play.exe -L../ -lfaad
//
//   dos_play song.aac           Streaming-Wiedergabe ueber SB16
//   dos_play song.aac out.raw   PCM in Datei schreiben (kein Audio)
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dos.h>
#include <dpmi.h>
#include <conio.h>

#include "../include/neaacdec.h"
#include "sb16.h"

// ---- Tunables ----
#define RES_CAP        (8u * 1024u * 1024u)   // Software-Decode-Reservoir
#define PREFILL_BYTES  (1u * 1024u * 1024u)   // Vorlauf vor dem Start
#define MAX_FRAME      (64u * 1024u)          // Sicherheitsmarge pro AAC-Frame
#define PRODUCE_BURST  16                     // max. Frames pro Durchlauf, dann feed+yield

// ---- Decode ----
static NeAACDecHandle    hDec = 0;
static uint8_t          *ptr = 0;
static uint32_t          bLeft = 0;
static NeAACDecFrameInfo frameInfo;
static unsigned          decodeError = 0;

// ---- Reservoir (Ringpuffer) ----
static uint8_t *res = 0;
static uint32_t resHead = 0;   // Schreibposition (monoton)
static uint32_t resTail = 0;   // Leseposition   (monoton)

static inline uint32_t res_used(void) { return resHead - resTail; }
static inline uint32_t res_free(void) { return RES_CAP - res_used(); }

static void res_write(const uint8_t *src, uint32_t n) {
	uint32_t off = resHead % RES_CAP, first = RES_CAP - off;
	if (first > n) first = n;
	memcpy(res + off, src, first);
	if (first < n) memcpy(res, src + first, n - first);
	resHead += n;
}

// Reservoir -> SB16-DMA-Ring: ein get_space(), dann in einem Durchlauf voll machen.
static void feed_ring(void) {
	int space = get_space();
	while (space > 0 && res_used() > 0) {
		uint32_t off    = resTail % RES_CAP;
		uint32_t contig = RES_CAP - off;
		uint32_t chunk  = res_used();
		if (chunk > (uint32_t)space) chunk = (uint32_t)space;
		if (chunk > contig)          chunk = contig;
		int w = sb16_play(res + off, chunk);
		if (w <= 0) break;
		resTail += (uint32_t)w;
		space   -= w;
	}
}

// Einen AAC-Frame dekodieren und ins Reservoir schreiben.
// Rueckgabe: 1 = weiter produzieren, 0 = Quelle erschoepft / Fehler.
static int decode_one(void) {
	if (bLeft == 0) return 0;
	void *sb = NeAACDecDecode(hDec, &frameInfo, ptr, bLeft);
	if (frameInfo.error > 0) { decodeError = frameInfo.error; return 0; }
	if (frameInfo.samples > 0 && sb)
		res_write((const uint8_t *)sb, (uint32_t)frameInfo.samples * 2); // 16 bit
	if (frameInfo.bytesconsumed == 0) return 0;
	bLeft -= frameInfo.bytesconsumed;
	ptr   += frameInfo.bytesconsumed;
	return (bLeft > 0);
}

static int abort_pressed(void) {
	if (kbhit()) { int c = getch(); if (c == 27 || c == 'q' || c == 3) return 1; }
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("Usage: %s <input.aac> [output.raw]\n", argv[0]);
		printf("  ohne Outfile: Streaming-Wiedergabe ueber SB16\n");
		printf("  mit Outfile : PCM in Datei schreiben (kein Audio)\n");
		return 1;
	}

	// --- AAC komplett in RAM laden ---
	FILE *fIn = fopen(argv[1], "rb");
	if (!fIn) { printf("Error opening %s\n", argv[1]); return 1; }
	fseek(fIn, 0, SEEK_END);
	long fsizeL = ftell(fIn);
	fseek(fIn, 0, SEEK_SET);
	if (fsizeL <= 0) { printf("Empty/invalid input\n"); fclose(fIn); return 1; }
	uint32_t fsize = (uint32_t)fsizeL;

	uint8_t *inBuf = (uint8_t *)malloc(fsize);
	if (!inBuf) { printf("malloc(%u) failed\n", (unsigned)fsize); fclose(fIn); return 1; }
	if (fread(inBuf, 1, fsize, fIn) != fsize) { printf("read error\n"); fclose(fIn); free(inBuf); return 1; }
	fclose(fIn);
	printf("Input loaded (%u bytes)\n", (unsigned)fsize);

	// --- FAAD2 konfigurieren ---
	hDec = NeAACDecOpen();
	if (!hDec) { printf("NeAACDecOpen failed\n"); free(inBuf); return 1; }
	NeAACDecConfigurationPtr conf = NeAACDecGetCurrentConfiguration(hDec);
	conf->outputFormat = FAAD_FMT_16BIT;
	NeAACDecSetConfiguration(hDec, conf);

	unsigned long samplerate = 0;
	unsigned char channels   = 0;
	long bCons = NeAACDecInit(hDec, inBuf, fsize, &samplerate, &channels);
	if (bCons < 0) { printf("NeAACDecInit error\n"); NeAACDecClose(hDec); free(inBuf); return 1; }
	ptr   = inBuf + bCons;
	bLeft = fsize - (uint32_t)bCons;
	printf("Format: %lu Hz, %u Kanaele\n", samplerate, (unsigned)channels);

	// --- Datei-Modus: PCM schreiben, kein Audio ---
	if (argc >= 3) {
		FILE *fOut = fopen(argv[2], "wb");
		if (!fOut) { printf("Error opening %s\n", argv[2]); NeAACDecClose(hDec); free(inBuf); return 1; }
		printf("Schreibe PCM nach %s ...\n", argv[2]);
		uint32_t total = 0;
		while (bLeft > 0) {
			void *sb = NeAACDecDecode(hDec, &frameInfo, ptr, bLeft);
			if (frameInfo.error > 0) {
				printf("FAAD error %u: %s\n", frameInfo.error, NeAACDecGetErrorMessage(frameInfo.error));
				break;
			}
			if (frameInfo.samples > 0 && sb) {
				uint32_t bytes = (uint32_t)frameInfo.samples * 2;
				fwrite(sb, 1, bytes, fOut);
				total += bytes;
			}
			if (frameInfo.bytesconsumed == 0) break;
			bLeft -= frameInfo.bytesconsumed;
			ptr   += frameInfo.bytesconsumed;
			if (abort_pressed()) { printf("\nAborted by user!\n"); break; }
		}
		fclose(fOut);
		printf("Fertig: %u bytes geschrieben\n", (unsigned)total);
		NeAACDecClose(hDec); free(inBuf);
		return 0;
	}

	// --- SB16-Wiedergabe ---
	res = (uint8_t *)malloc(RES_CAP);
	if (!res) { printf("Reservoir malloc(%u) failed\n", (unsigned)RES_CAP); NeAACDecClose(hDec); free(inBuf); return 1; }

	if (!sb16_init((uint32_t)samplerate, (uint32_t)channels)) {
		printf("SB16 init failed\n"); free(res); NeAACDecClose(hDec); free(inBuf); return 1;
	}

	int producing = (bLeft > 0);

	// Reservoir vorfuellen, Ring fuellen, dann starten (erst Daten, dann sb16_start).
	printf("Buffering...\n");
	while (producing && res_used() < PREFILL_BYTES) {
		if (!decode_one()) { producing = 0; break; }
		if (res_free() <= MAX_FRAME) break;
	}
	feed_ring();
	sb16_start();

	printf("Playing... (ESC/q/Ctrl+C zum Stoppen)\n");
	uint32_t lastReport = 0;
	int aborted = 0;

	while (1) {
		feed_ring();                                  // Ring zuerst auffuellen

		if (abort_pressed()) { aborted = 1; break; }
		if (decodeError) {
			printf("\nFAAD error %u: %s\n", decodeError, NeAACDecGetErrorMessage(decodeError));
			decodeError = 0;
		}

		// Produktion in kleinem Burst, damit das Fuettern nie lange pausiert
		int burst = 0;
		while (producing && res_free() > MAX_FRAME && burst < PRODUCE_BURST) {
			if (!decode_one()) { producing = 0; break; }
			burst++;
		}

		feed_ring();                                  // nachfuettern ...
		__dpmi_yield();                               // ... für VMs CPU abgeben 

		if (resTail - lastReport >= 1048576) {
			printf("\rPlayed %lu KB   ", (unsigned long)(resTail >> 10));
			lastReport = resTail;
		}

		if (!producing && res_used() == 0 && get_delay() <= 0.05f) break;
	}
	if (aborted) printf("\nAborted by user!\n");
	printf("\n");

	while (get_delay() > 0.05f) delay(10);
	sb16_uninit();

	free(res);
	NeAACDecClose(hDec);
	free(inBuf);
	return 0;
}
