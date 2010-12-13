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

#include <X11/X.h>
#include <X11/Xproto.h>
#include <dixstruct.h>
#include <extension.h>

#include "imx_ext.h"

/* External functions defined elsewhere in the driver. */
extern Bool
IMXGetPixmapProperties(
	PixmapPtr pPixmap,	/* IN */
	void** pPhysAddr,	/* OUT: pixmap phys addr, NULL if not GPU mem */
	int* pPitch);		/* OUT: pixmap pitch, 0 if not in GPU mem */

static DISPATCH_PROC(Proc_IMX_EXT_Dispatch);
static DISPATCH_PROC(Proc_IMX_EXT_GetPixmapPhysAddr);
static DISPATCH_PROC(SProc_IMX_EXT_Dispatch);
static DISPATCH_PROC(SProc_IMX_EXT_GetPixmapPhysAddr);

void IMX_EXT_Init()
{
	AddExtension(IMX_EXT_NAME, 0, 0, Proc_IMX_EXT_Dispatch, SProc_IMX_EXT_Dispatch,
			NULL, StandardMinorOpcode);
}

static int
Proc_IMX_EXT_GetPixmapPhysAddr(ClientPtr client)
{
	int n;

	REQUEST(xIMX_EXT_GetPixmapPhysAddrReq);
	REQUEST_SIZE_MATCH(xIMX_EXT_GetPixmapPhysAddrReq);

	/* Initialize reply */
	xIMX_EXT_GetPixmapPhysAddrReply rep;
	rep.type = X_Reply;
	rep.sequenceNumber = client->sequence;
	rep.length = 0;
	rep.pixmapState = IMX_EXT_PixmapUndefined;
	rep.pixmapPhysAddr = (CARD32)NULL;
	rep.pixmapPitch = 0;

	/* Find the pixmap */
	PixmapPtr pPixmap;
	int rc = dixLookupResourceByType((pointer*)&pPixmap, stuff->pixmap, RT_PIXMAP, client,
					DixGetAttrAccess);
	if (Success == rc)
	{
		void* pPhysAddr;
		int pitch;

		/* Query the pixmap properties from the driver. */
		if (IMXGetPixmapProperties(pPixmap, &pPhysAddr, &pitch))
		{
			rep.pixmapState = IMX_EXT_PixmapFramebuffer;
			rep.pixmapPhysAddr = (CARD32)pPhysAddr;
			rep.pixmapPitch = pitch;

		/* Pixmap was defined, but is not in frame buffer */
		} else {

			rep.pixmapState = IMX_EXT_PixmapOther;
		}
	}

	/* Check if any reply values need byte swapping */
	if (client->swapped)
	{
		swaps(&rep.sequenceNumber, n);
		swapl(&rep.length, n);
		swapl(&rep.pixmapPhysAddr, n);
		swapl(&rep.pixmapPitch, n);
	}

	/* Reply to client */
	WriteToClient(client, sizeof(rep), (char*)&rep);
	return client->noClientException;
}

static int
Proc_IMX_EXT_Dispatch(ClientPtr client)
{
	REQUEST(xReq);
	switch(stuff->data)
	{
		case X_IMX_EXT_GetPixmapPhysAddr:
			return Proc_IMX_EXT_GetPixmapPhysAddr(client);
		default:
			return BadRequest;
	}
}

static int
SProc_IMX_EXT_GetPixmapPhysAddr(ClientPtr client)
{
	int n;

	REQUEST(xIMX_EXT_GetPixmapPhysAddrReq);

	swaps(&stuff->length, n);
	REQUEST_SIZE_MATCH(xIMX_EXT_GetPixmapPhysAddrReq);

	swapl(&stuff->pixmap, n);
	return Proc_IMX_EXT_GetPixmapPhysAddr(client);
}

static int
SProc_IMX_EXT_Dispatch(ClientPtr client)
{
	REQUEST(xReq);
	switch(stuff->data)
	{
		case X_IMX_EXT_GetPixmapPhysAddr:
			return SProc_IMX_EXT_GetPixmapPhysAddr(client);
		default:
			return BadRequest;
	}
}

