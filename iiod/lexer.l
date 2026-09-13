%{
// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2014 Analog Devices, Inc.
 * Author: Paul Cercueil <paul.cercueil@analog.com>
 */

#include "parser.h"
#include "ops.h"

#include <string.h>
%}

%option noyywrap reentrant bison-bridge nounistd nounput noinput

WORD (([[:alpha:]]+,)|(iio:))?(-|_|\.|[[:alnum:]])+

%s WANT_DEVICE
%s WANT_CHN_OR_ATTR
%s WANT_CHN
%s WANT_ATTR
%s WANT_VALUE
%s WANT_DRAIN_DEVICE
%s WANT_DRAIN_CAPACITY
%s WANT_FEEDBACK_DEVICE
%s WANT_FEEDBACK_PAYLOAD
%%

<INITIAL>VERSION|version {
	return VERSION;
}

<INITIAL>PRINT|print {
	return PRINT;
}

<INITIAL>ZPRINT|zprint {
	return ZPRINT;
}

<INITIAL>EXIT|exit|QUIT|quit {
	return EXIT;
}

<INITIAL>HELP|help {
	return HELP;
}

<INITIAL>TIMEOUT|timeout {
	return TIMEOUT;
}

<INITIAL>OPEN|open {
	BEGIN(WANT_DEVICE);
	return OPEN;
}

<INITIAL>OPENM|openm {
	BEGIN(WANT_DEVICE);
	return OPENM;
}

<INITIAL>CLOSE|close {
	BEGIN(WANT_DEVICE);
	return CLOSE;
}

<INITIAL>READ|read {
	BEGIN(WANT_DEVICE);
	return READ;
}

<INITIAL>READBUF|readbuf {
	BEGIN(WANT_DEVICE);
	return READBUF;
}

<INITIAL>READBUFM|readbufm {
	BEGIN(WANT_DEVICE);
	return READBUFM;
}

<INITIAL>READBUFMA|readbufma {
	BEGIN(WANT_DEVICE);
	return READBUFMA;
}

<INITIAL>READBUFMSTAT|readbufmstat {
	BEGIN(WANT_DEVICE);
	return READBUFMSTAT;
}

<INITIAL>CANCELBUFM|cancelbufm {
	BEGIN(WANT_DEVICE);
	return CANCELBUFM;
}

<INITIAL>DRAINBUFM|drainbufm {
	BEGIN(WANT_DRAIN_DEVICE);
	return DRAINBUFM;
}

<INITIAL>WRITEBUF|writebuf {
	BEGIN(WANT_DEVICE);
	return WRITEBUF;
}

<INITIAL>FEEDBACKBUFM|feedbackbufm {
	BEGIN(WANT_FEEDBACK_DEVICE);
	return FEEDBACKBUFM;
}

<WANT_FEEDBACK_DEVICE>[^ \t\r\n]+ {
	struct parser_pdata *pdata=yyget_extra(yyscanner);
	yylval->dev=iio_context_find_device(pdata->ctx,yytext);
	BEGIN(WANT_FEEDBACK_PAYLOAD);
	return DEVICE;
}

<WANT_FEEDBACK_PAYLOAD>[^ \t\r\n]+ {
	yylval->word=strdup(yyleng<=512 && strlen(yytext)==(size_t)yyleng ? yytext : "");
	return WORD;
}

<INITIAL>WRITE|write {
	BEGIN(WANT_DEVICE);
	return WRITE;
}

<INITIAL>SETTRIG|settrig {
	BEGIN(WANT_DEVICE);
	return SETTRIG;
}

<INITIAL>GETTRIG|gettrig {
	BEGIN(WANT_DEVICE);
	return GETTRIG;
}

<INITIAL>SET|set {
	BEGIN(WANT_DEVICE);
	return SET;
}

<WANT_DRAIN_DEVICE>[^ \t\r\n]+ {
	struct parser_pdata *pdata = yyget_extra(yyscanner);
	yylval->dev = iio_context_find_device(pdata->ctx, yytext);
	BEGIN(WANT_DRAIN_CAPACITY);
	return DEVICE;
}

<WANT_DRAIN_CAPACITY>[^ \t\r\n]+ {
	/* Retain every character: the legacy catch-all would discard '+' and
	 * accidentally turn an invalid capacity into a valid one. */
	yylval->word = strdup(yyleng <= 20 && strlen(yytext) == (size_t)yyleng ? yytext : "");
	return WORD;
}

<WANT_DEVICE>{WORD} {
	struct parser_pdata *pdata = yyget_extra(yyscanner);
	struct iio_device *dev = iio_context_find_device(pdata->ctx, yytext);
	yylval->dev = dev;
	pdata->dev = dev;
	BEGIN(WANT_CHN_OR_ATTR);
	return DEVICE;
}

<WANT_CHN_OR_ATTR>BUFFERS_COUNT|buffers_count {
	BEGIN(WANT_VALUE);
	return BUFFERS_COUNT;
}

<WANT_CHN_OR_ATTR>DEBUG|debug {
	BEGIN(WANT_ATTR);
	return DEBUG_ATTR;
}

<WANT_CHN_OR_ATTR>BUFFER|buffer {
	BEGIN(WANT_ATTR);
	return BUFFER_ATTR;
}

<WANT_CHN_OR_ATTR>INPUT|input|OUTPUT|output {
	struct parser_pdata *pdata = yyget_extra(yyscanner);
	pdata->channel_is_output = yytext[0] == 'o' || yytext[0] == 'O';
	BEGIN(WANT_CHN);
	return IN_OUT;
}

<WANT_CHN>{WORD} {
	struct parser_pdata *pdata = yyget_extra(yyscanner);
	struct iio_channel *chn = NULL;
	if (pdata->dev)
		chn = iio_device_find_channel(pdata->dev,
					yytext, pdata->channel_is_output);
	yylval->chn = chn;
	pdata->chn = chn;
	BEGIN(WANT_ATTR);
	return CHANNEL;
}

<WANT_VALUE>{WORD} {
	char *end;
	char errstr[100];

	errno = 0;
	yylval->value = strtol(yytext, &end, 10);
	if (yytext == end || errno == ERANGE) {
		snprintf(errstr, sizeof(errstr), "lex : bad long constant: %s",(char*)yytext);
		perror(errstr);
	}
	return VALUE;
}

CYCLIC|cyclic {
	return CYCLIC;
}

{WORD} {
	yylval->word = strdup(yytext);
	return WORD;
}

[ \t]+ {
	return SPACE;
}

[ \t]*\r?\n {
	BEGIN(INITIAL);
	return END;
}

. {
	BEGIN(INITIAL);
}
