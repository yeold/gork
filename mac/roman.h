/*
 * roman.h -- Mac Roman and Unicode (roman.c).
 */
#ifndef GORK_MAC_ROMAN_H
#define GORK_MAC_ROMAN_H

char roman_from_unicode(unsigned long u);       /* or `?` */
int  roman_to_utf8(unsigned char c, char *out); /* out[3]: its length */

#endif
