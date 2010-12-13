/*
 * Copyright (C) 2009-2010 Freescale Semiconductor, Inc.  All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation 
 * files (the "Software"), to deal in the Software without 
 * restriction, including without limitation the rights to use, copy, 
 * modify, merge, publish, distribute, sublicense, and/or sell copies 
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS 
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN 
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN 
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
 * SOFTWARE.
 */

#ifndef __IMX_TYPE_H__
#define __IMX_TYPE_H__

#ifndef IMX_XVIDEO_ENABLE
#define	IMX_XVIDEO_ENABLE	0
#endif

#include "xf86.h"
#include "exa.h"


/* Macro converts the EXA_VERSION_* definitions for major, minor, and */
/* release into a single integer value that can be compared. */
#define	IMX_EXA_VERSION(maj,min,rel)	(((maj)*0x010000)|((min)*0x0100)|(rel))

#define	IMX_EXA_VERSION_COMPILED	IMX_EXA_VERSION( \
						EXA_VERSION_MAJOR, \
						EXA_VERSION_MINOR, \
						EXA_VERSION_RELEASE)


#if IMX_XVIDEO_ENABLE
#include "mxc_ipu_hl_lib.h"
#endif

/* -------------------------------------------------------------------- */
/* our private data, and two functions to allocate/free this            */

typedef struct {
	unsigned char*			fbstart;
	unsigned char*			fbmem;
	int				fboff;
	int				lineLength;
	int				rotate;
	Bool				shadowFB;
	void				*shadow;
	CloseScreenProcPtr		CloseScreen;
	CreateScreenResourcesProcPtr	CreateScreenResources;
	void				(*PointerMoved)(int index, int x, int y);
	EntityInfoPtr			pEnt;
	/* DGA info */
	DGAModePtr			pDGAMode;
	int				nDGAMode;
	OptionInfoPtr			Options;

#if IMX_XVIDEO_ENABLE
	/* for xvideo */
	DevUnion	                XVPortPrivate[1];
	Bool                    	isInit;
	short               		SrcX;
	short               		SrcY;
	short               		DstX;
	short               		DstY;
	short               		SrcW;
	short               		SrcH;
	short               		DstW;
	short               		DstH;
	int                 		Width;
	int                 		Height;
	char				fb_background[12];
	/* for using IPU's library */
	ipu_lib_input_param_t  		input_param;
	ipu_lib_output_param_t 		output_para;
	int                    		next_update_idx ;
	ipu_lib_handle_t       		ipu_handle;
	CARD32                 		colour_key;
#endif

	/* For EXA acceleration */
	ExaDriverPtr			exaDriverPtr;
	Bool				useAccel;
	void*				exaDriverPrivate;

	/* For EXA offscreen memory allocation. */
	ExaOffscreenArea*		offScreenAreas;
	unsigned			offScreenCounter;
	unsigned			numOffscreenAvailable;

} IMXRec, *IMXPtr;

#define IMXPTR(p) ((IMXPtr)((p)->driverPrivate))

#endif
