/*
 *      pocsag.c -- POCSAG protocol decoder
 *
 *      Copyright (C) 1996
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
 *
 *      Copyright (C) 2012-2014
 *          Elias Oenal    (multimon-ng@eliasoenal.com)
 *
 *      Copyright (C) 2022
 *          Tobias Girstmair (https://gir.st/)
 *
 *      Copyright (C) 2024
 *          Jason Lingohr (jason@lucid.net.au)
 *
 *      POCSAG (Post Office Code Standard Advisory Group)
 *      Radio Paging Decoder
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* ---------------------------------------------------------------------- */

#include "multimon.h"
#include "bch.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "cJSON.h"

/* ---------------------------------------------------------------------- */

//#define CHARSET_LATIN1
//#define CHARSET_UTF8 //ÄÖÜäöüß

/* ---------------------------------------------------------------------- */



/*
 * some codewords with special POCSAG meaning
 */
#define POCSAG_SYNC     0x7cd215d8
#define POCSAG_IDLE     0x7a89c197
#define POCSAG_SYNCINFO 0x7cf21436 // what is this value?

#define POCSAG_SYNC_WORDS ((2000000 >> 3) << 13)

#define POCSAG_MESSAGE_DETECTION 0x80000000 // Most significant bit is a one

#define CAESAR_ALPHA 0
#define CAESAR_SKYPER 1 // skyper messages are ROT-1 enciphered

#define POSCAG
/* ---------------------------------------------------------------------- */

int pocsag_mode = POCSAG_MODE_STANDARD;
int pocsag_invert_input = 0;
int pocsag_error_correction = 2;
int pocsag_show_partial_decodes = 0;
int pocsag_heuristic_pruning = 0;
int pocsag_prune_empty = 0;
int pocsag_polarity = 0;  /* 0=auto, 1=normal only, 2=inverted only */

extern int json_mode;

#ifdef __EBCANDROID__
/*
 * ---------------------------------------------------------------------------
 * EBC Android integration
 * ---------------------------------------------------------------------------
 * There is no stdout in this build. The decoder runs inside an Android foreground service and
 * the JNI boundary is the only way out (AGENTS.md guardrail 2), so every json_mode output site
 * below hands its object to ebc_emit_json() instead of fprintf(stdout).
 *
 * announce_pocsag_message() lives in pocsagjni.cpp. g_pocsag_baud is set by multimon_bridge.c
 * immediately before each demodulator call, so it always names the bit rate of the demodulator
 * whose message is being emitted -- the three run over the same audio, one after the other.
 */
void announce_pocsag_message(const char *json);
extern int g_pocsag_baud;

/*
 * Codewords BCH-checked while in sync, and how many of those could not be repaired.
 *
 * These exist because struct l2_state_pocsag's own counters cannot answer "how good is the
 * traffic we are actually receiving". pocsag_brute_repair() is called from the NO_SYNC search
 * as well -- twice per bit, once per polarity, on each of the three demodulators -- and it
 * bumps the same uncorrected-error counter there. Searching for a sync word in noise fails
 * almost every time, so any rate derived from those counters reads as ~100 % errors on an idle
 * channel and stays there once real traffic arrives.
 *
 * Incremented only on the SYNC path below, where every attempt is a real codeword. Defined in
 * multimon_bridge.c, which turns them into the err_ppm the UI shows.
 */
extern uint32_t g_pocsag_sync_words;
extern uint32_t g_pocsag_sync_bad_words;

/*
 * Emit [json] through the JNI sink and hand back a fresh empty object.
 *
 * Upstream shares one cJSON object between the numeric, alpha and skyper branches and deletes
 * it in each of them. In POCSAG_MODE_AUTO -- and in any mode once `unsure` is set -- two or
 * three of those branches run for the same message, so the second delete operates on freed
 * memory. Upstream also drops the string cJSON_PrintUnformatted() allocates. Neither matters
 * to a program that prints a line and exits; both are fatal to a service that runs for hours.
 *
 * Renewing the object keeps every branch's output while giving each one an object it alone
 * owns, so the ownership rule for the whole function stays simple: exactly one live object at
 * any time, deleted once at the end.
 */
static cJSON *ebc_emit_json(cJSON *json)
{
    if (json) {
        cJSON_AddNumberToObject(json, "bitrate", g_pocsag_baud);
        addJsonTimestamp(json);
        char *text = cJSON_PrintUnformatted(json);
        if (text) {
            announce_pocsag_message(text);
            cJSON_free(text);
        }
        cJSON_Delete(json);
    }
    return cJSON_CreateObject();
}
#endif /* __EBCANDROID__ */

/* ---------------------------------------------------------------------- */


enum states{
    NO_SYNC = 0,            //0b00000000
    SYNC = 64,              //0b10000000
    LOSING_SYNC = 65,       //0b10000001
    LOST_SYNC = 66,         //0b10000010
    ADDRESS = 67,           //0b10000011
    MESSAGE = 68,           //0b10000100
    END_OF_MESSAGE = 69,    //0b10000101
};


static inline unsigned char even_parity(uint32_t data)
{
    unsigned int temp = data ^ (data >> 16);

    temp = temp ^ (temp >> 8);
    temp = temp ^ (temp >> 4);
    temp = temp ^ (temp >> 2);
    temp = temp ^ (temp >> 1);
    return temp & 1;
}

/* ---------------------------------------------------------------------- */
	// ISO 646 national variant: US / IRV (1991)
    char *trtab[128] = {
			"<NUL>", 	//  0x0
			"<SOH>", 	//  0x1
			"<STX>", 	//  0x2
			"<ETX>", 	//  0x3
			"<EOT>", 	//  0x4
			"<ENQ>", 	//  0x5
			"<ACK>", 	//  0x6
			"<BEL>", 	//  0x7
			"<BS>",  	//  0x8
			"<HT>",  	//  0x9
			"<LF>",  	//  0xa
			"<VT>",  	//  0xb
			"<FF>",  	//  0xc
			"<CR>",  	//  0xd
			"<SO>",  	//  0xe
			"<SI>",  	//  0xf
			"<DLE>", 	// 0x10
			"<DC1>", 	// 0x11
			"<DC2>", 	// 0x12
			"<DC3>", 	// 0x13
			"<DC4>", 	// 0x14
			"<NAK>", 	// 0x15
			"<SYN>", 	// 0x16
			"<ETB>", 	// 0x17
			"<CAN>", 	// 0x18
			"<EM>",  	// 0x19
			"<SUB>", 	// 0x1a
			"<ESC>", 	// 0x1b
			"<FS>",  	// 0x1c
			"<GS>",  	// 0x1d
			"<RS>",  	// 0x1e
			"<US>",  	// 0x1f
			" ", 		// 0x20
			"!", 		// 0x21
			"\"", 		// 0x22

						// national variant
			"#", 		// 0x23
			"$", 		// 0x24

			"%", 		// 0x25
			"&", 		// 0x26
			"'", 		// 0x27
			"(", 		// 0x28
			")", 		// 0x29
			"*", 		// 0x2a
			"+", 		// 0x2b
			",", 		// 0x2c
			"-", 		// 0x2d
			".", 		// 0x2e
			"/", 		// 0x2f
			"0", 		// 0x30
			"1", 		// 0x31
			"2", 		// 0x32
			"3", 		// 0x33
			"4", 		// 0x34
			"5", 		// 0x35
			"6", 		// 0x36
			"7", 		// 0x37
			"8", 		// 0x38
			"9", 		// 0x39
			":", 		// 0x3a
			";", 		// 0x3b
			"<", 		// 0x3c
			"=", 		// 0x3d
			">", 		// 0x3e
			"?", 		// 0x3f
			"@", 		// 0x40
			"A", 		// 0x41
			"B", 		// 0x42
			"C", 		// 0x43
			"D", 		// 0x44
			"E", 		// 0x45
			"F", 		// 0x46
			"G", 		// 0x47
			"H", 		// 0x48
			"I", 		// 0x49
			"J", 		// 0x4a
			"K", 		// 0x4b
			"L", 		// 0x4c
			"M", 		// 0x4d
			"N", 		// 0x4e
			"O", 		// 0x4f
			"P", 		// 0x50
			"Q", 		// 0x51
			"R", 		// 0x52
			"S", 		// 0x53
			"T", 		// 0x54
			"U", 		// 0x55
			"V", 		// 0x56
			"W", 		// 0x57
			"X", 		// 0x58
			"Y", 		// 0x59
			"Z", 		// 0x5a
			
						// national variant
			"[", 		// 0x5b
			"\\", 		// 0x5c
			"]", 		// 0x5d
			"^", 		// 0x5e
			
			"_", 		// 0x5f

						// national variant
			"`", 		// 0x60

			"a", 		// 0x61
			"b", 		// 0x62
			"c", 		// 0x63
			"d", 		// 0x64
			"e", 		// 0x65
			"f", 		// 0x66
			"g", 		// 0x67
			"h", 		// 0x68
			"i", 		// 0x69
			"j", 		// 0x6a
			"k", 		// 0x6b
			"l", 		// 0x6c
			"m", 		// 0x6d
			"n", 		// 0x6e
			"o", 		// 0x6f
			"p", 		// 0x70
			"q", 		// 0x71
			"r", 		// 0x72
			"s", 		// 0x73
			"t", 		// 0x74
			"u", 		// 0x75
			"v", 		// 0x76
			"w", 		// 0x77
			"x", 		// 0x78
			"y", 		// 0x79
			"z", 		// 0x7a
			
						// national variant
			"{", 		// 0x7b
			"|", 		// 0x7c
			"}", 		// 0x7d
			"~", 		// 0x7e
			
			"<DEL>"		// 0x7f  
		};


/*
						// national variant
			"#", 		// 0x23
			"$", 		// 0x24

			"[", 		// 0x5b
			"\\", 		// 0x5c
			"]", 		// 0x5d
			"^", 		// 0x5e

			"`", 		// 0x60

			"{", 		// 0x7b
			"|", 		// 0x7c
			"}", 		// 0x7d
			"~", 		// 0x7e
*/

bool pocsag_init_charset(char *charset)
{
#ifdef __EBCANDROID__
	/*
	 * Restore the ISO 646 IRV defaults before applying a national variant.
	 *
	 * Upstream only ever overwrites individual trtab entries, which is safe for a program
	 * that parses its arguments once. This app can start a second session with a different
	 * charset in the same process, and without this reset the German umlauts would survive
	 * into a following US session and mistranslate 0x5b..0x7e for the rest of the app's life.
	 */
	static char *trtab_default[128];
	static int trtab_saved = 0;
	if (!trtab_saved) {
		memcpy(trtab_default, trtab, sizeof(trtab_default));
		trtab_saved = 1;
	} else {
		memcpy(trtab, trtab_default, sizeof(trtab_default));
	}
#endif
	if(strcmp(charset,"DE")==0) // German charset
	{
		#ifdef CHARSET_UTF8
			trtab[0x5b] = "Ä";
			trtab[0x5c] = "Ö";
			trtab[0x5d] = "Ü";

			trtab[0x7b] = "ä";
			trtab[0x7c] = "ö";
			trtab[0x7d] = "ü";
			trtab[0x7e] = "ß";
		#elif defined CHARSET_LATIN1
			trtab[0x5b] = "\304";
			trtab[0x5c] = "\326";
			trtab[0x5d] = "\334";

			trtab[0x7b] = "\344";
			trtab[0x7c] = "\366";
			trtab[0x7d] = "\374";
			trtab[0x7e] = "\337";
		#else
			trtab[0x5b] = "AE";
			trtab[0x5c] = "OE";
			trtab[0x5d] = "UE";

			trtab[0x7b] = "ae";
			trtab[0x7c] = "oe";
			trtab[0x7d] = "ue";
			trtab[0x7e] = "ss";
		#endif
	}
	else if (strcmp(charset,"DK")==0) // Danish charset /JT Ref. https://www.ascii-code.com
	{
		#ifdef CHARSET_UTF8
			trtab[0x5b] = "Æ";
			trtab[0x5c] = "Ø";
			trtab[0x5d] = "Å";

			trtab[0x7b] = "æ";
			trtab[0x7c] = "ø";
			trtab[0x7d] = "å";
		#elif defined CHARSET_LATIN1
			trtab[0x5b] = "\306";
			trtab[0x5c] = "\330";
			trtab[0x5d] = "\305";

			trtab[0x7b] = "\346";
			trtab[0x7c] = "\370";
			trtab[0x7d] = "\345";
		#else
			trtab[0x5b] = "AE";
			trtab[0x5c] = "OE";
			trtab[0x5d] = "Aa";

			trtab[0x7b] = "ae";
			trtab[0x7c] = "oe";
			trtab[0x7d] = "aa";
		#endif
	}
	else if (strcmp(charset,"SE")==0) // Swedish charset
	{
		#ifdef CHARSET_UTF8
			trtab[0x5b] = "Ä";
			trtab[0x5c] = "Ö";
			trtab[0x5d] = "Å";

			trtab[0x7b] = "ä";
			trtab[0x7c] = "ö";
			trtab[0x7d] = "å";
		#elif defined CHARSET_LATIN1
			trtab[0x5b] = "\304";
			trtab[0x5c] = "\326";
			trtab[0x5d] = "\305";

			trtab[0x7b] = "\344";
			trtab[0x7c] = "\366";
			trtab[0x7d] = "\345";
		#else
			trtab[0x5b] = "AE";
			trtab[0x5c] = "OE";
			trtab[0x5d] = "AO";

			trtab[0x7b] = "ae";
			trtab[0x7c] = "oe";
			trtab[0x7d] = "ao";
		#endif
	}
	else if (strcmp(charset,"FR")==0) // French charset
	{
		trtab[0x24] = "£";

		trtab[0x40] = "à";

		trtab[0x5b] = "°";
		trtab[0x5c] = "ç";
		trtab[0x5d] = "§";
		trtab[0x5e] = "^";
		trtab[0x5f] = "_";
		trtab[0x60] = "µ";

		trtab[0x7b] = "é";
		trtab[0x7c] = "ù";
		trtab[0x7d] = "è";
		trtab[0x7e] = "¨";
	}
	else if (strcmp(charset,"SI")==0) // Slovenian charset
	{
		#ifdef CHARSET_UTF8
			trtab[0x40] = "Ž";
			trtab[0x5b] = "Š";
			trtab[0x5c] = "Đ";
			trtab[0x5d] = "Ć";
			trtab[0x5e] = "Č";

			trtab[0x60] = "ž";
			trtab[0x7b] = "š";
			trtab[0x7c] = "đ";
			trtab[0x7d] = "ć";
			trtab[0x7e] = "č";
		#else
			trtab[0x40] = "Z";
			trtab[0x5b] = "S";
			trtab[0x5c] = "Dj";
			trtab[0x5d] = "C";
			trtab[0x5e] = "C";

			trtab[0x60] = "z";
			trtab[0x7b] = "s";
			trtab[0x7c] = "dj";
			trtab[0x7d] = "c";
			trtab[0x7e] = "c";
		#endif
	}
	else if (strcmp(charset,"US")==0) // US charset
	{
		// default
	}
	else
	{
		fprintf(stderr, "Error: invalid POCSAG charset %s\n", charset);
		fprintf(stderr, "Use: US,FR,DE,DK,SE,SI\n");
		charset = "US";
		return false; 
	}
	return true;
}

static char *translate_alpha(unsigned char chr)
{
	return trtab[chr & 0x7f];
}

/* ---------------------------------------------------------------------- */
static int guesstimate_alpha(const unsigned char cp)
{
    if((cp > 0 && cp < 32) || cp == 127)
        return -5; // Non printable characters are uncommon
    else if((cp > 32 && cp < 48)
            || (cp > 57 && cp < 65)
            || (cp > 90 && cp < 97)
            || (cp > 122 && cp < 127))
        return -2; // Penalize special characters
    else
        return 1;
}

static int guesstimate_numeric(const unsigned char cp, int pos)
{
    if(cp == 'U')
        return -10;
    else if(cp == '[' || cp == ']')
        return -5;
    else if(cp == ' ' || cp == '.' || cp == '-')
        return -2;
    else if(pos < 10) // Penalize long messages
        return 5;
    else
        return 0;
}

static int print_msg_numeric(struct l2_state_pocsag *rx, char* buff, unsigned int size)
{
    static const char *conv_table = "084 2.6]195-3U7[";
    unsigned char *bp = rx->buffer;
    int len = rx->numnibbles;
    char* cp = buff;
    int guesstimate = 0;

    if ( (unsigned int) len >= size)
        len = size-1;
    for (; len > 0; bp++, len -= 2) {
        *cp++ = conv_table[(*bp >> 4) & 0xf];
        if (len > 1)
            *cp++ = conv_table[*bp & 0xf];
    }
    *cp = '\0';

    cp = buff;
    for(int i = 0; *(cp+i); i++)
        guesstimate += guesstimate_numeric(*(cp+i), i);
    return guesstimate;
}

static unsigned char get7(const unsigned char *buf, int n)
{
    /* returns the n-th seven bit word */
    return ( buf[(n*7)/8]<<8 | buf[(n*7+6)/8] ) >> (n+1)%8;
}

static unsigned char rev7(unsigned char b)
{
    /* reverses the bit order of a seven bit word */
    return ((b << 6) & 64) | ((b >> 6) & 1) |
           ((b << 4) & 32) | ((b >> 4) & 2) |
           ((b << 2) & 16) | ((b >> 2) & 4) |
           ((b << 0) & 8);
}

static int print_msg_alpha(struct l2_state_pocsag *rx, char* buff, unsigned int size, int caesar)
{
    int len = rx->numnibbles * 4 / 7;
    char* cp = buff;
    int buffree = size-1;
    unsigned char curchr;
    char *tstr;
    int guesstimate = 0;

    for (int i = 0; i < len; i++)
    {
        curchr = rev7(get7(rx->buffer, i)) - caesar;

        guesstimate += guesstimate_alpha(curchr);

        tstr = translate_alpha(curchr);
        if (tstr)
        {
            int tlen = strlen(tstr);
            if (buffree >= tlen)
            {
                memcpy(cp, tstr, tlen);
                cp += tlen;
                buffree -= tlen;
            }
        } else if (buffree > 0) {
            *cp++ = curchr;
            buffree--;
        }
    }
    *cp = '\0';

    return guesstimate;
}

/* ---------------------------------------------------------------------- */

static void pocsag_printmessage(struct demod_state *s, bool sync)
{
    if(!pocsag_show_partial_decodes && ((s->l2.pocsag.address == -2) || (s->l2.pocsag.function == -2) || !sync))
        return; // Hide partial decodes
    if(pocsag_prune_empty && (s->l2.pocsag.numnibbles == 0))
        return;

    cJSON *json_output = cJSON_CreateObject();

    if((s->l2.pocsag.address != -1) || (s->l2.pocsag.function != -1))
    {
        if(s->l2.pocsag.numnibbles == 0)
        {
            if (!json_mode) {
                verbprintf(0, "%s: Address: %7lu  Function: %1hhi ",s->dem_par->name,
                           s->l2.pocsag.address, s->l2.pocsag.function);
                if(!sync) verbprintf(2,"<LOST SYNC>");
                verbprintf(0,"\n");
            }
            else {
                cJSON_AddStringToObject(json_output, "demod_name", s->dem_par->name);
                cJSON_AddNumberToObject(json_output, "address", s->l2.pocsag.address);
                cJSON_AddNumberToObject(json_output, "function", s->l2.pocsag.function);
#ifdef __EBCANDROID__
                json_output = ebc_emit_json(json_output);
#else
                addJsonTimestamp(json_output);
                fprintf(stdout, "%s\n", cJSON_PrintUnformatted(json_output));
                cJSON_Delete(json_output);
#endif
            }
        }
        else
        {
            char num_string[1024];
            char alpha_string[1024];
            char skyper_string[1024];
            int guess_num = 0;
            int guess_alpha = 0;
            int guess_skyper = 0;
            int unsure = 0;
            int func = 0;

            guess_num = print_msg_numeric(&s->l2.pocsag, num_string, sizeof(num_string));
            guess_alpha = print_msg_alpha(&s->l2.pocsag, alpha_string, sizeof(alpha_string), CAESAR_ALPHA);
            guess_skyper = print_msg_alpha(&s->l2.pocsag, skyper_string, sizeof(skyper_string), CAESAR_SKYPER);

            func = s->l2.pocsag.function;

            if(guess_num < 20 && guess_alpha < 20 && guess_skyper < 20)
            {
                if(pocsag_heuristic_pruning)
                {
#ifdef __EBCANDROID__
                    cJSON_Delete(json_output);   /* upstream leaks the object on this path */
#endif
                    return;
                }
                unsure = 1;
            }

            if((pocsag_mode == POCSAG_MODE_NUMERIC) || ((pocsag_mode == POCSAG_MODE_STANDARD) && (func == 0)) || ((pocsag_mode == POCSAG_MODE_AUTO) && (guess_num >= 20 || unsure)))
            {
                if((s->l2.pocsag.address != -2) || (s->l2.pocsag.function != -2))
                {
                    if (!json_mode)
                        verbprintf(0, "%s: Address: %7lu  Function: %1hhi  ",s->dem_par->name,
                               s->l2.pocsag.address, s->l2.pocsag.function);
                    else {
                        cJSON_AddStringToObject(json_output, "demod_name", s->dem_par->name);
                        cJSON_AddNumberToObject(json_output, "address", s->l2.pocsag.address);
                        cJSON_AddNumberToObject(json_output, "function", s->l2.pocsag.function);
                    }
                }
                else
                {
                    if (!json_mode)
                        verbprintf(0, "%s: Address:       -  Function: -  ",s->dem_par->name);
                    else {
                        cJSON_AddStringToObject(json_output, "demod_name", s->dem_par->name);
                        cJSON_AddNullToObject(json_output, "address");
                        cJSON_AddNullToObject(json_output, "function");
                    }
                }
                if(pocsag_mode == POCSAG_MODE_AUTO)
                    verbprintf(3, "Certainty: %5i  ", guess_num);
                if (!json_mode) {
                    verbprintf(0, "Numeric: %s", num_string);
                    if(!sync) verbprintf(2,"<LOST SYNC>");
                    verbprintf(0,"\n");
                }
                else {
                    cJSON_AddStringToObject(json_output, "numeric", num_string);
#ifdef __EBCANDROID__
                    json_output = ebc_emit_json(json_output);
#else
                    addJsonTimestamp(json_output);
                    fprintf(stdout, "%s\n", cJSON_PrintUnformatted(json_output));
                    fflush(stdout);
                    cJSON_Delete(json_output);
#endif
                }
            }

            if((pocsag_mode == POCSAG_MODE_ALPHA) || ((pocsag_mode == POCSAG_MODE_STANDARD) && (func != 0)) || ((pocsag_mode == POCSAG_MODE_AUTO) && (guess_alpha >= guess_skyper || unsure)))
            {
                if((s->l2.pocsag.address != -2) || (s->l2.pocsag.function != -2))
                {
                    if (!json_mode)
                        verbprintf(0, "%s: Address: %7lu  Function: %1hhi  ",s->dem_par->name,
                               s->l2.pocsag.address, s->l2.pocsag.function);
                    else {
                        cJSON_AddStringToObject(json_output, "demod_name", s->dem_par->name);
                        cJSON_AddNumberToObject(json_output, "address", s->l2.pocsag.address);
                        cJSON_AddNumberToObject(json_output, "function", s->l2.pocsag.function);
                    }
                }
                else
                {
                    if (!json_mode)
                        verbprintf(0, "%s: Address:       -  Function: -  ",s->dem_par->name);
                    else {
                        cJSON_AddStringToObject(json_output, "demod_name", s->dem_par->name);
                        cJSON_AddNullToObject(json_output, "address");
                        cJSON_AddNullToObject(json_output, "function");
                    }
                }
                if(pocsag_mode == POCSAG_MODE_AUTO)
                    verbprintf(3, "Certainty: %5i  ", guess_alpha);
                if (!json_mode) {
                    verbprintf(0, "Alpha:   %s", alpha_string);
                    if(!sync) verbprintf(2,"<LOST SYNC>");
                    verbprintf(0,"\n");
                }
                else {
                    cJSON_AddStringToObject(json_output, "alpha", alpha_string);
#ifdef __EBCANDROID__
                    json_output = ebc_emit_json(json_output);
#else
                    addJsonTimestamp(json_output);
                    fprintf(stdout, "%s\n", cJSON_PrintUnformatted(json_output));
                    fflush(stdout);
                    cJSON_Delete(json_output);
#endif
                }
            }

            if((pocsag_mode == POCSAG_MODE_SKYPER) || ((pocsag_mode == POCSAG_MODE_AUTO) && (guess_skyper >= guess_alpha || unsure))) // Only output SKYPER if we're explicitly asking for it or we're auto guessing! (because it's not part of one of the standards, right?!)
            {
                if((s->l2.pocsag.address != -2) || (s->l2.pocsag.function != -2))
                    if (!json_mode)
                        verbprintf(0, "%s: Address: %7lu  Function: %1hhi  ",s->dem_par->name,
                               s->l2.pocsag.address, s->l2.pocsag.function);
                    else {
                        cJSON_AddStringToObject(json_output, "demod_name", s->dem_par->name);
                        cJSON_AddNumberToObject(json_output, "address", s->l2.pocsag.address);
                        cJSON_AddNumberToObject(json_output, "function", s->l2.pocsag.function);
                    }
                else
                    if (!json_mode)
                        verbprintf(0, "%s: Address:       -  Function: -  ",s->dem_par->name);
                    else {
                        cJSON_AddStringToObject(json_output, "demod_name", s->dem_par->name);
                        cJSON_AddNullToObject(json_output, "address");
                        cJSON_AddNullToObject(json_output, "function");
                    }
                if(pocsag_mode == POCSAG_MODE_AUTO)
                    verbprintf(3, "Certainty: %5i  ", guess_skyper);
                if (!json_mode) {
                    verbprintf(0, "Skyper:  %s", skyper_string);
                    if(!sync) verbprintf(2,"<LOST SYNC>");
                    verbprintf(0,"\n");
                }
                else {
                    cJSON_AddStringToObject(json_output, "skyper", skyper_string);
#ifdef __EBCANDROID__
                    json_output = ebc_emit_json(json_output);
#else
                    addJsonTimestamp(json_output);
                    fprintf(stdout, "%s\n", cJSON_PrintUnformatted(json_output));
                    fflush(stdout);
                    cJSON_Delete(json_output);
#endif
                }
            }
        }
    }

#ifdef __EBCANDROID__
    /*
     * One object in, one object out. ebc_emit_json() always leaves a live empty object behind
     * and the paths that produce no output never touch it, so exactly one object is
     * outstanding here however the function was traversed.
     */
    cJSON_Delete(json_output);
#endif
}

/* ---------------------------------------------------------------------- */

void pocsag_init(struct demod_state *s)
{
    memset(&s->l2.pocsag, 0, sizeof(s->l2.pocsag));
    s->l2.pocsag.address = -1;
    s->l2.pocsag.function = -1;
}

void pocsag_deinit(struct demod_state *s)
{
    if(s->l2.pocsag.pocsag_total_error_count)
        verbprintf(1, "\n===%s stats===\n"
                   "Words BCH checked: %u\n"
                   "Corrected errors: %u\n"
                   "Corrected 1bit errors: %u\n"
                   "Corrected 2bit errors: %u\n"
                   "Invalid word or >2 bits errors: %u\n\n"
                   "Total bits processed: %u\n"
                   "Bits processed while in sync: %u\n"
                   "Bits processed while out of sync: %u\n"
                   "Successfully decoded: %f%%\n",
                   s->dem_par->name,
                   s->l2.pocsag.pocsag_total_error_count,
                   s->l2.pocsag.pocsag_corrected_error_count,
                   s->l2.pocsag.pocsag_corrected_1bit_error_count,
                   s->l2.pocsag.pocsag_corrected_2bit_error_count,
                   s->l2.pocsag.pocsag_uncorrected_error_count,
                   s->l2.pocsag.pocsag_total_bits_received,
                   s->l2.pocsag.pocsag_bits_processed_while_synced,
                   s->l2.pocsag.pocsag_bits_processed_while_not_synced,
                   (100./s->l2.pocsag.pocsag_total_bits_received)*s->l2.pocsag.pocsag_bits_processed_while_synced);
    fflush(stdout);
}

/* ---------------------------------------------------------------------- */

// BCH error correction using unified bch library
int pocsag_brute_repair(struct l2_state_pocsag *rx, uint32_t* data)
{
    int result = bch_pocsag_correct(data);
    
    if (result == 0) {
        return 0;  /* No errors */
    }
    
    rx->pocsag_total_error_count++;
    verbprintf(6, "Error in syndrome detected!\n");
    
    if (pocsag_error_correction == 0) {
        rx->pocsag_uncorrected_error_count++;
        verbprintf(6, "Couldn't correct error!\n");
        return 1;
    }
    
    if (result < 0) {
        /* Uncorrectable error */
        rx->pocsag_uncorrected_error_count++;
        verbprintf(6, "Couldn't correct error!\n");
        return 1;
    }
    
    /* Check if we're allowed to correct this many errors */
    if (result > pocsag_error_correction) {
        rx->pocsag_uncorrected_error_count++;
        verbprintf(6, "Couldn't correct error!\n");
        return 1;
    }
    
    /* Correction was applied by bch_pocsag_correct */
    rx->pocsag_corrected_error_count++;
    
    if (result == 1) {
        rx->pocsag_corrected_1bit_error_count++;
    } else {
        rx->pocsag_corrected_2bit_error_count++;
    }
    
    return 0;
}

static inline bool word_complete(struct demod_state *s)
{    
    // Do nothing for 31 bits
    // When the word is complete let the program counter pass
    s->l2.pocsag.rx_bit = (s->l2.pocsag.rx_bit + 1) % 32;
    return s->l2.pocsag.rx_bit == 0;
}

static inline bool is_sync(const uint32_t * const rx_data)
{
    if(*rx_data == POCSAG_SYNC)
        return true; // Sync found!
    return false;
}

static inline bool is_idle(const uint32_t * const rx_data)
{
    if(*rx_data == POCSAG_IDLE)
        return true; // Idle found!
    return false;
}

static void do_one_bit(struct demod_state *s, uint32_t rx_data)
{
    s->l2.pocsag.pocsag_total_bits_received++;

    switch(s->l2.pocsag.state & SYNC)
    {
    case NO_SYNC:
    {
        uint32_t rx_data_try;
        
        s->l2.pocsag.pocsag_bits_processed_while_not_synced++;

        /* Try normal polarity with error correction (unless inverted-only mode) */
        if(pocsag_polarity != 2)
        {
            rx_data_try = rx_data;
            pocsag_brute_repair(&s->l2.pocsag, &rx_data_try);
            if(rx_data_try == POCSAG_SYNC)
            {
                verbprintf(4, "Acquired sync!\n");
                s->l2.pocsag.state = SYNC;
                s->l2.pocsag.inverted = 0;
                return;
            }
        }
        
        /* Try inverted polarity with error correction (unless normal-only mode) */
        if(pocsag_polarity != 1)
        {
            rx_data_try = ~rx_data;
            pocsag_brute_repair(&s->l2.pocsag, &rx_data_try);
            if(rx_data_try == POCSAG_SYNC)
            {
                verbprintf(3, "Acquired sync (inverted polarity detected)!\n");
                s->l2.pocsag.state = SYNC;
                s->l2.pocsag.inverted = 1;
                return;
            }
        }
        return;
    }

    case SYNC:
    {
        s->l2.pocsag.pocsag_bits_processed_while_synced++;

        /* Apply inversion if auto-detected */
        if(s->l2.pocsag.inverted)
            rx_data = ~rx_data;

        if(!word_complete(s))
            return; // Wait for more bits to arrive.

        // it is always 17 words: position 0 is sync, positions 1-16 are data
        unsigned char rxword = s->l2.pocsag.rx_word; // for address calculation
        s->l2.pocsag.rx_word = (s->l2.pocsag.rx_word + 1) % 17;

        if(s->l2.pocsag.state == SYNC)
            s->l2.pocsag.state = ADDRESS; // We're in sync, move on.

#ifdef __EBCANDROID__
        g_pocsag_sync_words++;
#endif
        if(pocsag_brute_repair(&s->l2.pocsag, &rx_data))
        {
#ifdef __EBCANDROID__
            g_pocsag_sync_bad_words++;
#endif
            // Arbitration lost
            if(s->l2.pocsag.state != LOST_SYNC)
                s->l2.pocsag.state = LOSING_SYNC;
        }
        else
        {
            if(s->l2.pocsag.state == LOST_SYNC)
            {
                verbprintf(4, "Recovered sync!\n");
                s->l2.pocsag.state = ADDRESS;
            }
        }

        if(is_sync(&rx_data))
            return; // Already sync'ed.

        while(true)
            switch(s->l2.pocsag.state)
            {
            case LOSING_SYNC:
            {
                verbprintf(4, "Losing sync!\n");
                // Output what we've received so far.
                pocsag_printmessage(s, false);
                s->l2.pocsag.numnibbles = 0;
                s->l2.pocsag.address = -1;
                s->l2.pocsag.function = -1;
                s->l2.pocsag.state = LOST_SYNC;
                return;
            }

            case LOST_SYNC:
            {
                verbprintf(4, "Lost sync!\n");
                s->l2.pocsag.state = NO_SYNC;
                s->l2.pocsag.rx_word = 0;
                return;
            }

            case ADDRESS:
            {
                if(is_idle(&rx_data)) // Idle codewords have a magic address
                    return;

                if(rx_data & POCSAG_MESSAGE_DETECTION)
                {
                    verbprintf(4, "Got a message: %u\n", rx_data);
                    s->l2.pocsag.function = -2;
                    s->l2.pocsag.address  = -2;
                    s->l2.pocsag.state = MESSAGE;
                    break; // Performing partial decode
                }

                verbprintf(4, "Got an address: %u\n", rx_data);
                s->l2.pocsag.function = (rx_data >> 11) & 3;
                s->l2.pocsag.address  = ((rx_data >> 10) & 0x1ffff8) | ((rxword >> 1) & 7);
                s->l2.pocsag.state = MESSAGE;
                return;
            }

            case MESSAGE:
            {
                if(rx_data & POCSAG_MESSAGE_DETECTION)
                    verbprintf(4, "Got a message: %u\n", rx_data);
                else
                {
                    // Address/idle signals end of message
                    verbprintf(4, "Got an address: %u\n", rx_data);
                    s->l2.pocsag.state = END_OF_MESSAGE;
                    break;
                }

                if (s->l2.pocsag.numnibbles > sizeof(s->l2.pocsag.buffer)*2 - 5) {
#ifdef __EBCANDROID__
                    /*
                     * No stdout, and the upstream json_mode form is not valid JSON anyway --
                     * it puts a raw newline inside a string. This is a decoder-is-eating-noise
                     * indicator, not a page, so it belongs in the log rather than in the
                     * message stream the UI shows the user.
                     */
                    verbprintf(2, "%s: Warning: Message too long\n", s->dem_par->name);
#else
                    if (!json_mode) {
                        verbprintf(2, "%s: Warning: Message too long\n",
                                   s->dem_par->name);
                    } else {
                        fprintf(stdout, "{\"error\": \"%s: Warning: Message too long\n\"}", s->dem_par->name);
                        fflush(stdout);
                    }
#endif
                    /* Message too long indicates we're decoding garbage - lose sync */
                    s->l2.pocsag.state = LOSING_SYNC;
                    break;
                }

                uint32_t data;
                unsigned char *bp;
                bp = s->l2.pocsag.buffer + (s->l2.pocsag.numnibbles >> 1);
                data = (rx_data >> 11);
                if (s->l2.pocsag.numnibbles & 1) {
                    bp[0] = (bp[0] & 0xf0) | ((data >> 16) & 0xf);
                    bp[1] = data >> 8;
                    bp[2] = data;
                } else {
                    bp[0] = data >> 12;
                    bp[1] = data >> 4;
                    bp[2] = data << 4;
                }
                s->l2.pocsag.numnibbles += 5;
                verbprintf(5, "We received something!\n");
                return;
            }

            case END_OF_MESSAGE:
            {
                verbprintf(4, "End of message!\n");
                pocsag_printmessage(s, true);
                s->l2.pocsag.numnibbles = 0;
                s->l2.pocsag.address = -1;
                s->l2.pocsag.function = -1;
                s->l2.pocsag.state = ADDRESS;
                break;
            }

            default:
                break;
            }

    }

    default:
        break;
    }
}



/* ---------------------------------------------------------------------- */

void pocsag_rxbit(struct demod_state *s, int32_t bit)
{
    s->l2.pocsag.rx_data <<= 1;
    s->l2.pocsag.rx_data |= !bit;
    verbprintf(9, " %c ", '1'-(s->l2.pocsag.rx_data & 1));
    if(pocsag_invert_input)
        do_one_bit(s, ~(s->l2.pocsag.rx_data)); // this tries the inverted signal
    else
        do_one_bit(s, s->l2.pocsag.rx_data);
}

/* ---------------------------------------------------------------------- */
