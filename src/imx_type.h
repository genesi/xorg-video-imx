/*
 * Copyright 2009-2010 Freescale Semiconductor, Inc. All Rights Reserved.
 */

#ifndef __IMX_TYPE_H__
#define __IMX_TYPE_H__

#ifndef IMX_XVIDEO_ENABLE
#define	IMX_XVIDEO_ENABLE	0
#endif

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
	Bool				useAccel;
	void*				exaDriverPrivate;

} IMXRec, *IMXPtr;

#define IMXPTR(p) ((IMXPtr)((p)->driverPrivate))

#endif
