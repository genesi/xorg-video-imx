/*
 * Copyright (C) 2009-2011 Freescale Semiconductor, Inc.  All Rights Reserved.
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

#include "xf86.h"
#include "xf86_OSproc.h"
#include "fbdevhw.h"
#include "exa.h"
#include "imx_type.h"
#include "z160.h"


/* Set if handles pixmap allocation and migration, i.e, EXA_HANDLES_PIXMAPS */
#define	IMX_EXA_ENABLE_HANDLES_PIXMAPS	(1 && (IMX_EXA_VERSION_COMPILED >= IMX_EXA_VERSION(2,5,0)))

/* Set minimum size (pixel area) for accelerating operations. */
#define	IMX_EXA_MIN_PIXEL_AREA_SOLID		32
#define	IMX_EXA_MIN_PIXEL_AREA_COPY		32
#define	IMX_EXA_MIN_PIXEL_AREA_COMPOSITE	32

/* This flag must be enabled to perform any debug logging */
#define IMX_EXA_DEBUG_MASTER		0

#define	IMX_EXA_DEBUG_INSTRUMENT_SYNCS	(0 && IMX_EXA_DEBUG_MASTER)
#define IMX_EXA_DEBUG_INSTRUMENT_SIZES	(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_PREPARE_SOLID	(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_SOLID		(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_PREPARE_COPY	(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_COPY		(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_CHECK_COMPOSITE	(0 && IMX_EXA_DEBUG_MASTER)

#if IMX_EXA_DEBUG_MASTER
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#endif


/* This is private data for the EXA driver to use */

typedef struct _IMXEXARec {

	int				scrnIndex;
	int				numScreenBytes;

	void*				gpuContext;
	Bool				gpuSynced;

	void*				savePixmapPtr[3];

	/* Parameters passed into PrepareSolid */
	int				solidALU;
	Pixel				solidPlaneMask;
	Pixel				solidColor;

	/* Parameters passed into PrepareCopy */
	int				copyALU;
	Pixel				copyPlaneMask;
	int				copyDirX;
	int				copyDirY;

	/* Pixmap and Z160-derived parameters passed into Prepare{Solid,Copy,Composite} */
	PixmapPtr			pPixmapDst;
	PixmapPtr			pPixmapSrc;
	Z160Buffer			z160BufferDst;	
	Z160Buffer			z160BufferSrc;	
	unsigned long			z160Color;

	/* Flag set if GPU has been setup for solid, copy, or */
	/* composite operation. */
	Bool				gpuOpSetup;

	/* Graphics context for software fallback in solid/copy/composite */
	GCPtr				pGC;

#if IMX_EXA_DEBUG_INSTRUMENT_SIZES
	unsigned long			numSolidFillRect100;
	unsigned long			numSolidFillRect1000;
	unsigned long			numSolidFillRect10000;
	unsigned long			numSolidFillRect100000;
	unsigned long			numSolidFillRectLarge;
	unsigned long			numScreenCopyRect100;
	unsigned long			numScreenCopyRect1000;
	unsigned long			numScreenCopyRect10000;
	unsigned long			numScreenCopyRect100000;
	unsigned long			numScreenCopyRectLarge;
#endif

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS
	unsigned long			numSolidBeforeSync;
	unsigned long			numCopyBeforeSync;
	unsigned long			numCompositeBeforeSync;
#endif

} IMXEXARec, *IMXEXAPtr;

#define IMXEXAPTR(p) ((IMXEXAPtr)((p)->exaDriverPrivate))


#if IMX_EXA_ENABLE_HANDLES_PIXMAPS

typedef struct _IMXEXAPixmapRec {

	/* Properties for pixmap header passed in CreatePixmap2 or */
	/* in ModifyPixmapHeader callbacks. */
	int			width;
	int			height;
	int			depth;
	int			bitsPerPixel;

	/* Common properties for allocated pixmap regardless where stored. */
	int			pitchBytes;	/* bytes per row */
	void*			ptr;		/* ptr to system/virtual addr */
	Bool			canAccel;	/* true if in GPU memory */
	void*			*gpuAddr;	/* physical GPU addr if accel */

	/* Properties for pixmap allocated from GPU FB (offscreen) memory */
	int			widthAligned;	/* aligned to 32 pixel horz */
	int			heightAligned;	/* aligned to 32 pixel vert */
	ExaOffscreenArea	*area;		/* ptr to GPU FB memory alloc */

	/* Properties for pixmap allocated from system memory. */
	int			sysAllocSize;	/* size of sys memory alloc */
	void*			sysPtr;		/* ptr to sys memory alloc */

} IMXEXAPixmapRec, *IMXEXAPixmapPtr;


/* Definitions for functions defined in imx_exa_offscreen.c */
extern Bool IMX_EXA_OffscreenInit(ScreenPtr pScreen);
extern ExaOffscreenArea* IMX_EXA_OffscreenAlloc(
				ScreenPtr pScreen, int size, int align,
                  		Bool locked, ExaOffscreenSaveProc save,
                  		pointer privData);
extern ExaOffscreenArea* IMX_EXA_OffscreenFree(
				ScreenPtr pScreen, ExaOffscreenArea* area);
extern void IMX_EXA_OffscreenFini(ScreenPtr pScreen);

#endif


static
PixmapPtr
Z160EXAGetDrawablePixmap(DrawablePtr pDrawable)
{
	/* Make sure there is a drawable. */
	if (NULL == pDrawable) {
		return NULL;
	}

	/* Check for a backing pixmap. */
	if (DRAWABLE_WINDOW == pDrawable->type) {

		WindowPtr pWindow = (WindowPtr)pDrawable;
		return pDrawable->pScreen->GetWindowPixmap(pWindow);
	}

	/* Otherwise, it's a regular pixmap. */
	return (PixmapPtr)pDrawable;
}

static PixmapPtr
Z160EXAGetPicturePixmap(PicturePtr pPicture)
{
	if (NULL != pPicture) {

		return Z160EXAGetDrawablePixmap(pPicture->pDrawable);
	}

	return NULL;
}



/* Called by IMXGetRec */
void IMX_EXA_GetRec(ScrnInfoPtr pScrn)
{
	IMXPtr imxPtr = IMXPTR(pScrn);

	if (NULL != imxPtr->exaDriverPrivate) {
		return;
	}

	imxPtr->exaDriverPrivate = xnfcalloc(sizeof(IMXEXARec), 1);

	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	fPtr->gpuContext = NULL;

	fPtr->gpuSynced = FALSE;
	fPtr->gpuOpSetup = FALSE;

	fPtr->savePixmapPtr[EXA_PREPARE_DEST] = NULL;
	fPtr->savePixmapPtr[EXA_PREPARE_SRC] = NULL;
	fPtr->savePixmapPtr[EXA_PREPARE_MASK] = NULL;

	fPtr->pGC = NULL;

#if IMX_EXA_DEBUG_INSTRUMENT_SIZES
	fPtr->numSolidFillRect100 = 0;
	fPtr->numSolidFillRect1000 = 0;
	fPtr->numSolidFillRect10000 = 0;
	fPtr->numSolidFillRect100000 = 0;
	fPtr->numSolidFillRectLarge = 0;

	fPtr->numScreenCopyRect100 = 0;
	fPtr->numScreenCopyRect1000 = 0;
	fPtr->numScreenCopyRect10000 = 0;
	fPtr->numScreenCopyRect100000 = 0;
	fPtr->numScreenCopyRectLarge = 0;
#endif

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS
	fPtr->numSolidBeforeSync = 0;
	fPtr->numCopyBeforeSync = 0;
	fPtr->numCompositeBeforeSync = 0;
#endif
}

/* Called by IMXFreeRec */
void IMX_EXA_FreeRec(ScrnInfoPtr pScrn)
{
	IMXPtr imxPtr = IMXPTR(pScrn);

	if (NULL == imxPtr->exaDriverPrivate) {
		return;
	}

	free(imxPtr->exaDriverPrivate);
	imxPtr->exaDriverPrivate = NULL;
}


#if IMX_DEBUG_MASTER

static unsigned long
Z160GetElapsedMicroseconds(struct timeval* pTimeStart, struct timeval* pTimeStop)
{
	/* If either time is missing, then return 0. */
	if ((NULL == pTimeStart) || (NULL == pTimeStop)) {
		return 0;
	}

	/* Start time after stop time (looking at seconds field only). */
	if (pTimeStart->tv_sec > pTimeStop->tv_sec) {

		return 0;

	/* Start time and stop time have same seconds field. */
	} else if (pTimeStart->tv_sec == pTimeStop->tv_sec) {

		/* Start time after stop time (looking at usec field only). */
		if (pTimeStart->tv_usec > pTimeStop->tv_usec) {

			return 0;

		} else {

			return pTimeStop->tv_usec - pTimeStart->tv_usec;
		}

	/* Start time is before stop time, but the seconds are different. */
	} else {

		unsigned long elapsedMicroseconds =
			(pTimeStop->tv_sec - pTimeStart->tv_sec) * 1000000U;

		elapsedMicroseconds += 
			(pTimeStop->tv_usec - pTimeStart->tv_usec);

		return elapsedMicroseconds;
	}
}
#endif

static
void*
Z160EXAGetPixmapAddress(PixmapPtr pPixmap)
{
#if IMX_EXA_ENABLE_HANDLES_PIXMAPS
	/* Make sure pixmap is defined. */
	if (NULL == pPixmap) {
		return NULL;
	}

	/* Access driver private data structure associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr)(exaGetPixmapDriverPrivate(pPixmap));
	if (NULL == fPixmapPtr) {
		return NULL;
	}

	return fPixmapPtr->ptr;
#else
	/* Access screen associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);

	/* Compute the physical address using relative offset. */
	return (unsigned char*)imxPtr->fbstart + exaGetPixmapOffset(pPixmap);
#endif
}

Bool
IMX_EXA_GetPixmapProperties(
	PixmapPtr pPixmap,
	void** pPhysAddr,
	int* pPitch)
{
	/* Initialize values to be returned. */
	*pPhysAddr = NULL;
	*pPitch = 0;

	/* Is there a pixmap? */
	if (NULL == pPixmap) {
		return FALSE;
	}

#if IMX_EXA_ENABLE_HANDLES_PIXMAPS

	/* Access driver private data structure associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr)(exaGetPixmapDriverPrivate(pPixmap));
	if (NULL == fPixmapPtr) {
		return FALSE;
	}

	/* Make sure pixmap is in GPU memory. */
	if (!fPixmapPtr->canAccel) {
		return FALSE;
	}

	/* Get the physical address of pixmap and its pitch */
	*pPhysAddr = fPixmapPtr->gpuAddr;
	*pPitch = fPixmapPtr->pitchBytes;

#else

	/* Access screen associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Make sure pixmap is in framebuffer */
	if (!exaDrawableIsOffscreen(&(pPixmap->drawable))) {
		return FALSE;
	}

	/* Get the physical address of pixmap and its pitch */
	*pPhysAddr = (void*)((unsigned char*)pScrn->memPhysBase + exaGetPixmapOffset(pPixmap));
	*pPitch = exaGetPixmapPitch(pPixmap);

#endif

	return TRUE;
}

static inline Bool
Z160IsDrawablePixelOnly(DrawablePtr pDrawable)
{
	/* we only return true if both width & height are equal to 1, saves branching entirely) */
	return (Bool)(0x00000001 & (pDrawable->width & pDrawable->height));
}

static Bool
Z160CanAcceleratePixmapRectangles(PixmapPtr pPixmap)
{
	/* Do not check pixmap size because EXA may want to call to */
	/* accelerate rectangles within a pixmap which is larger */
	/* than that allowed by the z160 size limits, as long as */
	/* those rectangles are within the z160 size limits bounds. */

	/* Pixmap must be defined */
	if (NULL == pPixmap) {
		return FALSE;
	}

#if IMX_EXA_ENABLE_HANDLES_PIXMAPS

	/* Access driver private data structure associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr)(exaGetPixmapDriverPrivate(pPixmap));
	if (NULL == fPixmapPtr) {
		return FALSE;
	}

	/* Make sure pixmap is in GPU memory. */
	if (!fPixmapPtr->canAccel) {
		return FALSE;
	}

	/* Pixmap pitch must be within z160 limits and must be aligned. */
	const unsigned pitchBytes = fPixmapPtr->pitchBytes;
	if ((pitchBytes > Z160_MAX_PITCH_BYTES) ||
		(0 != (pitchBytes & (Z160_ALIGN_PITCH-1)))) {

		return FALSE;
	}

	/* Pixmap must be offset aligned. */
	const void* gpuAddr = fPixmapPtr->gpuAddr;
	if (0 != ((int)gpuAddr & (Z160_ALIGN_OFFSET-1))) {
		return FALSE;
	}

#else

	/* Pixmap must be in frame buffer memory */
	if (!exaDrawableIsOffscreen(&(pPixmap->drawable))) {
		return FALSE;
	}

	/* Pixmap pitch must be within z160 limits and must be aligned. */
	unsigned pitchBytes = exaGetPixmapPitch(pPixmap);
	if ((pitchBytes > Z160_MAX_PITCH_BYTES) ||
		(0 != (pitchBytes & (Z160_ALIGN_PITCH-1)))) {

		return FALSE;
	}

	/* Pixmap must be offset aligned. */
	if (0 != (exaGetPixmapOffset(pPixmap) & (Z160_ALIGN_OFFSET-1))) {
		return FALSE;
	}

#endif

	/* If we get here, then operations on this pixmap can be accelerated. */
	return TRUE;
}

static Bool
Z160CanAcceleratePixmap(PixmapPtr pPixmap)
{
	/* Pixmap must be defined */
	if (NULL == pPixmap) {
		return FALSE;
	}

	/* Pixmap size must be within z160 limits */
	if ((pPixmap->drawable.width > Z160_MAX_WIDTH) ||
		(pPixmap->drawable.height > Z160_MAX_HEIGHT)) {

		return FALSE;
	}

	/* Check rest of pixmap properties for acceleration. */
	return Z160CanAcceleratePixmapRectangles(pPixmap);
}

static Bool 
Z160GetPixmapConfig(PixmapPtr pPixmap, Z160Buffer* pBuffer)
{
	/* Is there a pixmap? */
	if (NULL == pPixmap) {
		return FALSE;
	}

	/* Is there a buffer to store the results? */
	if (NULL == pBuffer) {
		return FALSE;
	}

	/* Get frame buffer properties about the pixmap. */
	if (!IMX_EXA_GetPixmapProperties(pPixmap, &pBuffer->base, &pBuffer->pitch)) {
		return FALSE;
	}

	/* Get other properties from the pixmap */
	pBuffer->width = pPixmap->drawable.width;
	pBuffer->height = pPixmap->drawable.height;
	pBuffer->bpp = pPixmap->drawable.bitsPerPixel;

	return TRUE;
}

static Bool 
Z160GetPictureConfig(ScrnInfoPtr pScrn, PicturePtr pPicture, Z160Buffer* pBuffer)
{
	/* Is there a picture? */
	if (NULL == pPicture) {
		return FALSE;
	}

	/* Is there a buffer to store the results? */
	if (NULL == pBuffer) {
		return FALSE;
	}

	/* Access the pixmap associated with this picture. */
	PixmapPtr pPixmap = Z160EXAGetDrawablePixmap(pPicture->pDrawable);
	if (NULL == pPixmap) {
		return FALSE;
	}

	/* Setup information from pixmap */
	if (!Z160GetPixmapConfig(pPixmap, pBuffer)) {
		return FALSE;
	}

	/* Setup based on the picture format. */
	switch (pPicture->format) {

		default:
			return FALSE;

		/* Alpha */

		case PICT_a8:
			pBuffer->format = Z160_FORMAT_A8;
			pBuffer->swapRB = FALSE;
			pBuffer->opaque = FALSE;
			break;

		/* 8-bit */

		case PICT_g8:
			pBuffer->format = Z160_FORMAT_8;
			pBuffer->swapRB = FALSE;
			pBuffer->opaque = FALSE;
			break;

		/* 16-bit */

		case PICT_r5g6b5:
			pBuffer->format = Z160_FORMAT_0565;
			pBuffer->swapRB = FALSE;
			pBuffer->opaque = FALSE;
			break;

		case PICT_b5g6r5:
			pBuffer->format = Z160_FORMAT_0565;
			pBuffer->swapRB = TRUE;
			pBuffer->opaque = FALSE;
			break;

		case PICT_a4r4g4b4:
			pBuffer->format = Z160_FORMAT_4444;
			pBuffer->swapRB = FALSE;
			pBuffer->opaque = FALSE;
			break;

		case PICT_x4r4g4b4:
			pBuffer->format = Z160_FORMAT_4444;
			pBuffer->swapRB = FALSE;
			pBuffer->opaque = TRUE;
			break;

		case PICT_a4b4g4r4:
			pBuffer->format = Z160_FORMAT_4444;
			pBuffer->swapRB = TRUE;
			pBuffer->opaque = FALSE;
			break;

		case PICT_x4b4g4r4:
			pBuffer->format = Z160_FORMAT_4444;
			pBuffer->swapRB = TRUE;
			pBuffer->opaque = TRUE;
			break;

		case PICT_a1r5g5b5:
			pBuffer->format = Z160_FORMAT_1555;
			pBuffer->swapRB = FALSE;
			pBuffer->opaque = FALSE;
			break;

		case PICT_x1r5g5b5:
			pBuffer->format = Z160_FORMAT_1555;
			pBuffer->swapRB = FALSE;
			pBuffer->opaque = TRUE;
			break;

		case PICT_a1b5g5r5:
			pBuffer->format = Z160_FORMAT_1555;
			pBuffer->swapRB = TRUE;
			pBuffer->opaque = FALSE;
			break;

		case PICT_x1b5g5r5:
			pBuffer->format = Z160_FORMAT_1555;
			pBuffer->swapRB = TRUE;
			pBuffer->opaque = TRUE;
			break;

		/* 32-bit  - normal format is ARGB */

		case PICT_a8r8g8b8:
			pBuffer->format = Z160_FORMAT_8888;
			pBuffer->swapRB = FALSE;
			pBuffer->opaque = FALSE;
			break;

		case PICT_x8r8g8b8:
			pBuffer->format = Z160_FORMAT_8888;
			pBuffer->swapRB = FALSE;
			pBuffer->opaque = TRUE;
			break;

		case PICT_a8b8g8r8:
			pBuffer->format = Z160_FORMAT_8888;
			pBuffer->swapRB = TRUE;
			pBuffer->opaque = FALSE;
			break;

		case PICT_x8b8g8r8:
			pBuffer->format = Z160_FORMAT_8888;
			pBuffer->swapRB = TRUE;
			pBuffer->opaque = TRUE;
			break;
	}

	pBuffer->alpha4 = pPicture->componentAlpha ? TRUE : FALSE;

	return TRUE;
}

#if 0
static unsigned long
Z160ConvertScreenColor(ScrnInfoPtr pScrn, Pixel color)
{
	/* How many bits assigned to alpha channel. */
	unsigned bitsAlpha = pScrn->bitsPerPixel - pScrn->weight.red -
				pScrn->weight.green - pScrn->weight.blue;

	unsigned long gpuColor = 0x00000000;

	/* Assign the alpha channel. */
	if (0 == bitsAlpha) {
		gpuColor = 0xFF000000;
	} else {
		/* TODO */
	}

	/* Assign the red channel. */
	unsigned long red = (color & pScrn->mask.red) >> pScrn->offset.red;
	gpuColor = gpuColor | (red << (24 - pScrn->weight.red));

	/* Assign the green channel. */
	unsigned long green = (color & pScrn->mask.green) >> pScrn->offset.green;
	gpuColor = gpuColor | (green << (16 - pScrn->weight.green));

	/* Assign the blue channel. */
	unsigned long blue = (color & pScrn->mask.blue) >> pScrn->offset.blue;
	gpuColor = gpuColor | (blue << (8 - pScrn->weight.blue));

	return gpuColor;
}
#endif

static void
Z160ContextRelease(IMXEXAPtr fPtr)
{
	/* Destroy the GPU context? */
	if ((NULL != fPtr) && (NULL != fPtr->gpuContext)) {

		z160_sync(fPtr->gpuContext);
		z160_disconnect(fPtr->gpuContext);
		fPtr->gpuContext = NULL;
	}
}

static void*
Z160ContextGet(IMXEXAPtr fPtr)
{
	/* If no connection, attempt to establish it. */
	if (NULL == fPtr->gpuContext) {

		/* Get context to access the GPU. */
		fPtr->gpuContext = z160_connect();
		if (NULL == fPtr->gpuContext) {

			xf86DrvMsg(fPtr->scrnIndex, X_ERROR,
				"Unable to access Z160 GPU\n");
			Z160ContextRelease(fPtr);
			return NULL;
		}

		/* Other initialization. */
		fPtr->gpuSynced = FALSE;
		fPtr->gpuOpSetup = FALSE;
	}

	return fPtr->gpuContext;
}

static void
Z160Sync(IMXEXAPtr fPtr)
{
	if (NULL == fPtr) {
		return;
	}

	/* If there is no GPU context, then no reason to sync. */
	/* Do not use Z160ContextGet because it will regain */
	/* access if we currently do not have it (because idle). */
	if (NULL == fPtr->gpuContext) {
		return;
	}

	/* Was there a GPU operation since the last sync? */
	if (!fPtr->gpuSynced) {

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

		/* Log how many calls were made to solid, copy, and composite before sync called. */
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"Z160EXAWaitMarker called after solid=%lu copy=%lu composite=%lu\n",
				fPtr->numSolidBeforeSync,
				fPtr->numCopyBeforeSync,
				fPtr->numCompositeBeforeSync);

		/* Reset counters */
		fPtr->numSolidBeforeSync = 0;
		fPtr->numCopyBeforeSync = 0;
		fPtr->numCompositeBeforeSync = 0;
#endif

		/* Do the wait */
		z160_sync(fPtr->gpuContext);

		/* Update state */
		fPtr->gpuSynced = TRUE;
	}
}

static void
Z160EXAPreparePipelinedAccess(PixmapPtr pPixmap, int index)
{
	/* Access screen associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Remember previous setting so it can be restored in *FinishPipelinedAccess */
	fPtr->savePixmapPtr[index] = pPixmap->devPrivate.ptr;

	/* Forces a real devPrivate.ptr for hidden pixmaps, so that they */
	/* can be passed down into fb* funtions. */
	if (NULL == pPixmap->devPrivate.ptr) {

		pPixmap->devPrivate.ptr = Z160EXAGetPixmapAddress(pPixmap);
	}
}

static void
Z160EXAFinishPipelinedAccess(PixmapPtr pPixmap, int index)
{
	/* Access screen associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Restore to previous setting before call to *PreparePipelinedAccess */
	pPixmap->devPrivate.ptr = fPtr->savePixmapPtr[index];
	fPtr->savePixmapPtr[index] = NULL;
}

#if IMX_EXA_ENABLE_HANDLES_PIXMAPS

/* BEGIN Functions for driver to handle pixmap allocation and migraion. */

/* Align an offset to an arbitrary alignment */
#define IMX_EXA_ALIGN(offset, align) (((offset) + (align) - 1) - \
	(((offset) + (align) - 1) % (align)))

static inline Bool
Z160EXAPrepareAccess(PixmapPtr pPixmap, int index)
{
	/* Since EXA_HANDLES_PIXMAPS flag is set, then there nothing to do. */
	/* But this callback has to be implemented. */

	return TRUE;
}

static inline void
Z160EXAFinishAccess(PixmapPtr pPixmap, int index)
{
	/* Since EXA_HANDLES_PIXMAPS flag is set, then there nothing to do. */
	/* But this callback has to be implemented. */
}

static inline int
Z160EXAComputeSystemMemoryPitch(int width, int bitsPerPixel)
{
	return ((width * bitsPerPixel + FB_MASK) >> FB_SHIFT) * sizeof(FbBits);
}

static void*
Z160EXACreatePixmap2(ScreenPtr pScreen, int width, int height,
			int depth, int usage_hint, int bitsPerPixel,
			int *pPitch)
{
	/* Allocate the private data structure to be stored with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr)xnfalloc(sizeof(IMXEXAPixmapRec));

	if (NULL == fPixmapPtr) {
		return NULL;
	}

	/* Initialize pixmap properties passed in. */
	fPixmapPtr->width = width;
	fPixmapPtr->height = height;
	fPixmapPtr->depth = depth;
	fPixmapPtr->bitsPerPixel = bitsPerPixel;

	/* Initialize common properties. */
	fPixmapPtr->pitchBytes = 0;
	fPixmapPtr->ptr = NULL;
	fPixmapPtr->canAccel = FALSE;
	fPixmapPtr->gpuAddr = NULL;

	/* Initialize properties for GPU frame buffer allocated memory. */
	fPixmapPtr->widthAligned = 0;
	fPixmapPtr->heightAligned = 0;
	fPixmapPtr->area = NULL;

	/* Initialize properties for system allocated memory. */
	fPixmapPtr->sysAllocSize = 0;
	fPixmapPtr->sysPtr = NULL;

	/* Nothing more to do if the width or height have no dimensions. */
	if ((0 == width) || (0 == height)) {
		*pPitch = 0;
		return fPixmapPtr;
	}

	/* Access the driver specific data. */
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);
	
	/* What is the start of screen (and offscreen) memory. */
	CARD8* screenMemoryBegin = (CARD8*)(imxPtr->exaDriverPtr->memoryBase);

	/* First try to allocate pixmap memory from GPU memory but */
	/* can only when bits per pixel >= 8. */
	if (bitsPerPixel >= 8) {

		/* Z160 has 32 pixel width and height alignment. */
		const int gpuAlignedWidth = IMX_EXA_ALIGN(width, 32);
		const int gpuAlignedHeight = IMX_EXA_ALIGN(height, 32);

		/* Compute number of pitch bytes for GPU allocated memory. */
		const int gpuPitchBytes = gpuAlignedWidth * bitsPerPixel / 8;

		/* Compute how much memory to allocate for GPU memory. */
		const int gpuAllocSize = gpuAlignedHeight * gpuPitchBytes;

		/* Attemp to allocate from GPU (offscreen) FB memory pool. */
		ExaOffscreenArea* area =
			IMX_EXA_OffscreenAlloc(
				pScreen,		/* ScreenPtr */
				gpuAllocSize,		/* size */
				Z160_ALIGN_OFFSET,	/* align */
				TRUE,			/* locked? */
				NULL,			/* save */
				NULL);			/* privData */

		/* If memory allocated, then assign values to private */
		/* data structure and return. */
		if (NULL != area) {

			fPixmapPtr->widthAligned = gpuAlignedWidth;
			fPixmapPtr->heightAligned = gpuAlignedHeight;
			fPixmapPtr->area = area;

			fPixmapPtr->canAccel = TRUE;
			fPixmapPtr->pitchBytes = gpuPitchBytes;
			fPixmapPtr->ptr = screenMemoryBegin + area->offset;
			fPixmapPtr->gpuAddr =
				(void*)((unsigned char*)pScrn->memPhysBase +
					area->offset);

			*pPitch = gpuPitchBytes;
		}
	}

	/* If we could not allocate pixmap memory from GPU memory, */
	/* then must try allocating from system memory. */
	if (NULL == fPixmapPtr->ptr) {

		/* Compute number of pitch bytes for system allocated memory. */
		/* The number of pitch bytes is passed in as the mis-named */
		/* "devKind" parameter. */
		const int sysPitchBytes =
			Z160EXAComputeSystemMemoryPitch(width, bitsPerPixel);

		/* Compute how much memory to allocate for system memory. */
		const int sysAllocSize = height * sysPitchBytes;

		/* Attempt to allocate pixmap memory from system memory. */
		void* sysPtr = xnfalloc(sysAllocSize);
		
		/* If memory allocated, then assign values to private */
		/* data structure and return. */
		if (NULL != sysPtr) {

			fPixmapPtr->sysAllocSize = sysAllocSize;
			fPixmapPtr->sysPtr = sysPtr;

			fPixmapPtr->pitchBytes = sysPitchBytes;
			fPixmapPtr->ptr = sysPtr;
			fPixmapPtr->canAccel = FALSE;

			*pPitch = sysPitchBytes;
		}
	}

	/* If we got here and still have no pixmap memory, then cleanup */
	/* and setup to return failure. */
	if (NULL == fPixmapPtr->ptr) {
		free(fPixmapPtr);
		fPixmapPtr = NULL;
	}

	return fPixmapPtr;
}

static void
Z160EXADestroyPixmap(ScreenPtr pScreen, void *driverPriv)
{
	/* Nothing to do if driver private pointer not defined. */
	if (NULL == driverPriv) {
		return;
	}

	/* Cast pointer to driver private data structure. */
	IMXEXAPixmapPtr fPixmapPtr = (IMXEXAPixmapPtr)driverPriv;

	/* Is pixmap allocated in offscreen frame buffer memory? */
	if (NULL != fPixmapPtr->area) {

		IMX_EXA_OffscreenFree(pScreen, fPixmapPtr->area);

	/* Is pixmap allocated in system memory? */
	} else if (NULL != fPixmapPtr->sysPtr) {

		free(fPixmapPtr->sysPtr);
	}

	/* Free the driver private data structure associated with pixmap. */
	free(fPixmapPtr);
}

static Bool
Z160EXAModifyPixmapHeader(PixmapPtr pPixmap, int width, int height,
		int depth, int bitsPerPixel, int devKind, pointer pPixData)
{
	/* Make sure the pixmap is defined. */
	if (NULL == pPixmap) {
		return FALSE;
	}

	/* Access driver private data structure associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr)(exaGetPixmapDriverPrivate(pPixmap));
	if (NULL == fPixmapPtr) {
		return FALSE;
	}

	/* Access screen associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* What is the start of screen (and offscreen) memory and its size. */
	CARD8* screenMemoryBegin = (CARD8*)(imxPtr->exaDriverPtr->memoryBase);
	CARD8* screenMemoryEnd =
		screenMemoryBegin + imxPtr->exaDriverPtr->memorySize;

	/* Update the width if specified. */
	if (0 < width) {
		fPixmapPtr->width = width;
	}

	/* Update the height if specified. */
	if (0 < height) {
		fPixmapPtr->height = height;
	}

	/* Update the bits per pixel if specified */
	if (0 < bitsPerPixel) {
		fPixmapPtr->bitsPerPixel = bitsPerPixel;
	}

	/* Update the bits per pixel if specified */
	if (0 < depth) {
		fPixmapPtr->depth = depth;
	}

	/* Update the pointer to pixel data if specified. */
	if (0 != pPixData) {

		fPixmapPtr->ptr = pPixData;

		if ((screenMemoryBegin <= (CARD8*)(fPixmapPtr->ptr)) &&
			((CARD8*)(fPixmapPtr->ptr) < screenMemoryEnd)) {

			fPixmapPtr->canAccel = TRUE;

			/* Compute address relative to begin of FB memory. */
			const unsigned long offset =
				(CARD8*)(fPixmapPtr->ptr) - screenMemoryBegin;

			/* Store GPU address. */
			fPixmapPtr->gpuAddr =
				(void*)((unsigned char*)pScrn->memPhysBase +
					offset);

		} else {

			fPixmapPtr->canAccel = FALSE;
		}

		/* If the pixel buffer changed and the pitch was not */
		/* specified, then recompute the pitch. */
		if (0 >= devKind) {
			devKind =
				Z160EXAComputeSystemMemoryPitch(
					fPixmapPtr->width,
					fPixmapPtr->bitsPerPixel);
		}
	}

	/* Update the pitch bytes if specified, or if recomputed. */
	if (0 < devKind) {
		fPixmapPtr->pitchBytes = devKind;
	}

	/* Update the pixmap header with our info. */
	pPixmap->drawable.width = fPixmapPtr->width;
	pPixmap->drawable.height = fPixmapPtr->height;
	pPixmap->drawable.bitsPerPixel = fPixmapPtr->bitsPerPixel;
	pPixmap->drawable.depth = fPixmapPtr->depth;
	pPixmap->devPrivate.ptr = fPixmapPtr->ptr;
	pPixmap->devKind = fPixmapPtr->pitchBytes;

	return TRUE;
}

static Bool
Z160EXAPixmapIsOffscreen(PixmapPtr pPixmap)
{
	/* Make sure pixmap is defined. */
	if (NULL == pPixmap) {
		return FALSE;
	}

	/* Access driver private data structure associated with pixmap. */
	IMXEXAPixmapPtr fPixmapPtr =
		(IMXEXAPixmapPtr)(exaGetPixmapDriverPrivate(pPixmap));
	if (NULL == fPixmapPtr) {
		return FALSE;
	}

	return fPixmapPtr->canAccel;
}

/* END Functions for driver to handle pixmap allocation and migraion. */

#else

static inline Bool
Z160EXAPrepareAccess(PixmapPtr pPixmap, int index)
{
	/* Frame buffer memory is not allocated through Z160, so nothing to do. */
	/* But this callback has to be implemented since the offscreen pixmap test */
	/* callback has been overridden. */

	return TRUE;
}

static inline void
Z160EXAFinishAccess(PixmapPtr pPixmap, int index)
{
	/* Nothing to do, but this callback has to be implemented */
	/* if the prepare access callback is overridden. */
}

#endif

static Bool
Z160EXAPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask, Pixel fg)
{
	/* Make sure pixmap is defined. */
	if (NULL == pPixmap) {
		return FALSE;
	}

	/* Make sure operations can be accelerated on this pixmap. */
	if (!Z160CanAcceleratePixmap(pPixmap)) {
		return FALSE;
	}

	/* Determine number of pixels in target pixmap. */
	unsigned pixmapArea = pPixmap->drawable.width * pPixmap->drawable.height;

	/* Can't accelerate solid fill unless pixmap has minimum number of pixels. */
	if (pixmapArea < IMX_EXA_MIN_PIXEL_AREA_SOLID) {
		return FALSE;
	}

	/* Access screen associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Check the number of entities, and fail if it isn't one. */
	if (pScrn->numEntities != 1) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Z160EXAPrepareSolid called with number of screen entities (%d) not 1\n",
			pScrn->numEntities);
		return FALSE;
	}

	/* Make sure that the input planemask specifies a solid */
	if (!EXA_PM_IS_SOLID(&pPixmap->drawable, planemask)) {

#if DEBUG_PREPARE_SOLID
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Z160EXAPrepareSolid called with planemask=0x%08x which is not solid\n",
			(unsigned)planemask);
#endif
		return FALSE;
	}

	/* Make sure that only GXcopy is only raster op called for. */
	if (GXcopy != alu) {

#if DEBUG_PREPARE_SOLID
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Z160EXAPrepareSolid called with rop=0x%08x which is not GXcopy\n",
			(unsigned)alu);
#endif
		return FALSE;
	}

	/* Only 8, 16, and 32-bit pixmaps are supported. */
	/* Associate a pixel format which is required for configuring */
	/* the Z160.  It does not matter what format is chosen as long as it */
	/* is one that matchs the bitsPerPixel.  The format of the input */
	/* foreground color matches the format of the target pixmap, but */
	/* we will shift the bits around to match the chosen format. */
	if (!Z160GetPixmapConfig(pPixmap, &fPtr->z160BufferDst)) {
		return FALSE;
	}
	switch (pPixmap->drawable.bitsPerPixel) {

	case 8:
		fPtr->z160BufferDst.format = Z160_FORMAT_8;	/* value goes in blue channel */
		fPtr->z160BufferDst.swapRB = FALSE;
		fPtr->z160Color = fg & 0x000000FF;
		break;

	case 16:
		fPtr->z160BufferDst.format = Z160_FORMAT_4444;	/* upper nibble */
		fPtr->z160BufferDst.swapRB = FALSE;
		fPtr->z160Color =	((fg & 0x0000F000) << 16) |
					((fg & 0x00000F00) << 12) |
					((fg & 0x000000F0) <<  8) |
					((fg & 0x0000000F) <<  4);
		break;

	case 32:
		fPtr->z160BufferDst.format = Z160_FORMAT_8888;	/* ARGB */
		fPtr->z160BufferDst.swapRB = FALSE;
		fPtr->z160Color = fg;
		break;

	default:
#if DEBUG_PREPARE_SOLID
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Z160EXAPrepareSolid called with unsupported pixmap bitsPerPixel=%d\n",
			pPixmap>bitsPerPixel);
#endif
		return FALSE;
	}

	/* GPU setup deferred */
	fPtr->gpuOpSetup = FALSE;

	/* Remember the parameters passed in */
	fPtr->pPixmapDst = pPixmap;
	fPtr->solidALU = alu;
	fPtr->solidPlaneMask = planemask;
	fPtr->solidColor = fg;

	return TRUE;
}

static void
Z160EXASolid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
{
	/* Access screen associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Nothing to unless rectangle has area. */
	if ((x1 >= x2) || (y1 >= y2)) {
		return;
	}

	/* Compute the width and height of the rectangle to fill. */
	int width = x2 - x1;
	int height = y2 - y1;

	if (!fPtr->gpuOpSetup) {
		z160_setup_buffer_target(fPtr->gpuContext, &fPtr->z160BufferDst);
		z160_setup_fill_solid(fPtr->gpuContext, fPtr->z160Color);

		fPtr->gpuOpSetup = TRUE;
	}
	z160_fill_solid_rect(fPtr->gpuContext, x1, y1, width, height);

#if IMX_EXA_DEBUG_SOLID
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"Z160EXASolid called with rect=(%d-%d,%d-%d)\n",
		x1, x2, y1, y2);
#endif

#if IMX_EXA_DEBUG_INSTRUMENT_SIZES
	const unsigned long size =
		(unsigned long)(z160DstRect.width) * z160DstRect.height;

	if (size < 100) {

		++(fPtr->numSolidFillRect100);

	} else if (size < 1000) {

		++(fPtr->numSolidFillRect1000);

	} else if (size < 10000) {

		++(fPtr->numSolidFillRect10000);

	} else if (size < 100000) {

		++(fPtr->numSolidFillRect100000);

	} else {

		++(fPtr->numSolidFillRectLarge);
	}
#endif

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

	++(fPtr->numSolidBeforeSync);

#endif
}

static void
Z160EXADoneSolid(PixmapPtr pPixmap)
{
	/* Access screen associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Finalize any GPU operations if any where used */
	if (fPtr->gpuOpSetup) {

		/* Flush pending operations to the GPU. */
		z160_flush(fPtr->gpuContext);

		/* Update state. */
		fPtr->gpuSynced = FALSE;
		fPtr->gpuOpSetup = FALSE;
	}

	/* Release graphics context used for software fallback? */
	if (NULL != fPtr->pGC) {

		FreeScratchGC(fPtr->pGC);
		fPtr->pGC = NULL;

		Z160EXAFinishPipelinedAccess(fPtr->pPixmapDst, EXA_PREPARE_DEST);
	}
}

static Bool
Z160EXAPrepareCopy(
	PixmapPtr pPixmapSrc,
	PixmapPtr pPixmapDst,
	int xdir,
	int ydir,
	int alu,
	Pixel planemask)
{
	/* Make sure source and target pixmaps are defined. */
	if ((NULL == pPixmapDst) || (NULL == pPixmapSrc)) {
		return FALSE;
	}

	/* Make sure operations can be accelerated on the source and target */
	/* pixmaps.  As long as rectangles are within z160 size bounds */
	/* EXA will accelerate copy even if pixmaps are bigger than */
	/* size bounds.  EXA only does this for copies. */
	if (!Z160CanAcceleratePixmapRectangles(pPixmapDst) ||
		!Z160CanAcceleratePixmapRectangles(pPixmapSrc)) {

		return FALSE;
	}

	/* Determine number of pixels in target and source pixmaps. */
	unsigned pixmapAreaDst = pPixmapDst->drawable.width * pPixmapDst->drawable.height;
	unsigned pixmapAreaSrc = pPixmapSrc->drawable.width * pPixmapSrc->drawable.height;

	/* Can't accelerate copy unless pixmaps have minimum number of pixels. */
	if ((pixmapAreaDst < IMX_EXA_MIN_PIXEL_AREA_COPY) || 
		(pixmapAreaSrc < IMX_EXA_MIN_PIXEL_AREA_COPY)) {

		return FALSE;
	}

	/* Access the screen associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Determine the bits-per-pixels for src and dst pixmaps. */
	int dstPixmapBitsPerPixel = pPixmapDst->drawable.bitsPerPixel;
	int srcPixmapBitsPerPixel = pPixmapSrc->drawable.bitsPerPixel;

	/* Cannot perform copy unless these src and dst pixmaps have the same bits-per-pixel. */
	if (dstPixmapBitsPerPixel != srcPixmapBitsPerPixel) {
		return FALSE;
	}

	/* Check the number of entities, and fail if it isn't one. */
	if (pScrn->numEntities != 1) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Z160EXAPrepareCopy called with number of screen entities (%d) not 1\n",
			pScrn->numEntities);
		return FALSE;
	}

	/* Make sure that the input planemask specifies a solid */
	if (!EXA_PM_IS_SOLID(&pPixmapDst->drawable, planemask)) {

#if IMX_EXA_DEBUG_COPY
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Z160EXAPrepareCopy called with planemask=0x%08x which is not solid\n",
			(unsigned)planemask);
#endif
		return FALSE;
	}

	/* Make sure that only GXcopy is only raster op called for. */
	if (GXcopy != alu) {

#if IMX_EXA_DEBUG_COPY
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Z160EXAPrepareCopy called with rop=0x%08x which is not GXcopy\n",
			(unsigned)alu);
#endif
		return FALSE;
	}

	/* Setup buffer parameters based on target pixmap. */
	if (!Z160GetPixmapConfig(pPixmapDst, &fPtr->z160BufferDst)) {
		return FALSE;
	}

	/* Setup buffer parameters based on source pixmap. */
	if (!Z160GetPixmapConfig(pPixmapSrc, &fPtr->z160BufferSrc)) {
		return FALSE;
	}

	/* Only 8, 16, and 32-bit pixmaps are supported. */
	/* Associate a pixel format which is required for configuring */
	/* the Z160.  It does not matter what format is chosen as long as it */
	/* is one that matchs the bitsPerPixel. */
	Z160_FORMAT z160Format;
	switch (dstPixmapBitsPerPixel) {

		case 8:
			z160Format = Z160_FORMAT_8;	/* value goes in alpha channel */
			break;

		case 16:
			z160Format = Z160_FORMAT_4444;	/* upper nibble */
			break;

		case 32:
			z160Format = Z160_FORMAT_8888;	/* ARGB */
			break;

		default:
#if IMX_EXA_DEBUG_COPY
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Z160EXAPrepareCopy unsupported pixmap bits per pixel %dy\n",
				dstPixmapBitsPerPixel);
#endif
			return FALSE;
	}
	fPtr->z160BufferDst.format = fPtr->z160BufferSrc.format = z160Format;
	fPtr->z160BufferDst.swapRB = fPtr->z160BufferSrc.swapRB = FALSE;

	/* GPU setup deferred */
	fPtr->gpuOpSetup = FALSE;

	/* Remember the parameters passed in */
	fPtr->pPixmapDst = pPixmapDst;
	fPtr->pPixmapSrc = pPixmapSrc;
	fPtr->copyALU = alu;
	fPtr->copyPlaneMask = planemask;
	fPtr->copyDirX = xdir;
	fPtr->copyDirY = ydir;

	return TRUE;
}

static void
Z160EXACopy(PixmapPtr pPixmapDst, int srcX, int srcY, int dstX, int dstY, int width, int height)
{
	/* Access screen associated with dst pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);


	if (!fPtr->gpuOpSetup) {
		z160_setup_buffer_target(fPtr->gpuContext, &fPtr->z160BufferDst);
		z160_setup_copy(fPtr->gpuContext, &fPtr->z160BufferSrc,
					fPtr->copyDirX, fPtr->copyDirY);

		fPtr->gpuOpSetup = TRUE;
	}

	z160_copy_rect(fPtr->gpuContext, dstX, dstY, width, height, srcX, srcY);

#if IMX_EXA_DEBUG_INSTRUMENT_SIZES
	const unsigned long size =
		(unsigned long)(z160DstRect.width) * z160DstRect.height;

	if (size < 100) {

		++(fPtr->numScreenCopyRect100);

	} else if (size < 1000) {

		++(fPtr->numScreenCopyRect1000);

	} else if (size < 10000) {

		++(fPtr->numScreenCopyRect10000);

	} else if (size < 100000) {

		++(fPtr->numScreenCopyRect100000);

	} else {

		++(fPtr->numScreenCopyRectLarge);
	}
#endif

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

	++(fPtr->numCopyBeforeSync);

#endif

#if IMX_EXA_DEBUG_COPY
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"Z160EXACopy called with src=(%d-%d,%d-%d) dst=(%d-%d,%d-%d)\n",
		srcX, srcX+width, srcY, srcY+height, dstX, dstX+width, dstY, dstY+height);
#endif
}

static void
Z160EXADoneCopy(PixmapPtr pPixmapDst)
{
	/* Access screen associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Finalize any GPU operations if any where used */
	if (fPtr->gpuOpSetup) {

		/* Flush pending operations to the GPU. */
		z160_flush(fPtr->gpuContext);

		/* Update state */
		fPtr->gpuSynced = FALSE;
		fPtr->gpuOpSetup = FALSE;
	}

	/* Release graphics context used for software fallback? */
	if (NULL != fPtr->pGC) {

		FreeScratchGC(fPtr->pGC);
		fPtr->pGC = NULL;

		Z160EXAFinishPipelinedAccess(fPtr->pPixmapDst, EXA_PREPARE_DEST);
		Z160EXAFinishPipelinedAccess(fPtr->pPixmapSrc, EXA_PREPARE_SRC);
	}
}

static Z160_BLEND Z160SetupBlendOpTable[] = {
	Z160_BLEND_UNKNOWN,	/* 0 = PictOpMinimum, PictOpClear */
	Z160_BLEND_SRC,		/* 1 = PictOpSrc */
	Z160_BLEND_UNKNOWN,	/* 2 = PictOpDst */
	Z160_BLEND_OVER,	/* 3 = PictOpOver */
	Z160_BLEND_UNKNOWN,	/* 4 = PictOpOverReverse */
	Z160_BLEND_IN,		/* 5 = PictOpIn */
	Z160_BLEND_IN_REVERSE,	/* 6 = PictOpInReverse */
	Z160_BLEND_UNKNOWN,	/* 7 = PictOpOut */
	Z160_BLEND_OUT_REVERSE,	/* 8 = PictOpOutReverse */
	Z160_BLEND_UNKNOWN,	/* 9 = PictOpAtop */
	Z160_BLEND_UNKNOWN,	/* 10 = PictOpAtopReverse */
	Z160_BLEND_UNKNOWN,	/* 11 = PictOpXor */
	Z160_BLEND_ADD,		/* 12 = PictOpAdd */
	Z160_BLEND_UNKNOWN	/* 13 = PictOpSaturate, PictOpMaximum */
};
static int NumZ160SetupBlendOps =
	sizeof(Z160SetupBlendOpTable) / sizeof(Z160SetupBlendOpTable[0]);

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

static const char*
Z160GetPictureTypeName(PicturePtr pPicture)
{
	switch(PICT_FORMAT_TYPE(pPicture->format)) {
		case PICT_TYPE_OTHER:	return "other";
		case PICT_TYPE_A:	return "alpha";
		case PICT_TYPE_ARGB:	return "argb";
		case PICT_TYPE_ABGR:	return "abgr";
		case PICT_TYPE_COLOR:	return "color";
		case PICT_TYPE_GRAY:	return "gray";
		case PICT_TYPE_BGRA:	return "bgra";
		default:		return "???";
	}
}

#endif



/*
 * From /usr/include/xorg/exa.h:
 *
 * Notes on interpreting Picture structures:
 * - The Picture structures will always have a valid pDrawable.
 * - The Picture structures will never have alphaMap set.
 * - The mask Picture (and therefore pMask) may be NULL, in which case the
 *   operation is simply src OP dst instead of src IN mask OP dst, and
 *   mask coordinates should be ignored.
 * - pMarkPicture may have componentAlpha set, which greatly changes
 *   the behavior of the Composite operation.  componentAlpha has no effect
 *   when set on pSrcPicture or pDstPicture.
 * - The source and mask Pictures may have a transformation set
 *   (Picture->transform != NULL), which means that the source coordinates
 *   should be transformed by that transformation, resulting in scaling,
 *   rotation, etc.  The PictureTransformPoint() call can transform
 *   coordinates for you.  Transforms have no effect on Pictures when used
 *   as a destination.
 * - The source and mask pictures may have a filter set.  PictFilterNearest
 *   and PictFilterBilinear are defined in the Render protocol, but others
 *   may be encountered, and must be handled correctly (usually by
 *   PrepareComposite failing, and falling back to software).  Filters have
 *   no effect on Pictures when used as a destination.
 * - The source and mask Pictures may have repeating set, which must be
 *   respected.  Many chipsets will be unable to support repeating on
 *   pixmaps that have a width or height that is not a power of two.
 */

static Bool
Z160EXACheckComposite(int op, PicturePtr pPictureSrc, PicturePtr pPictureMask, PicturePtr pPictureDst)
{
	/* Pictures for src and dst must be defined. */
	if ((NULL == pPictureSrc) || (NULL == pPictureDst)) {
		return FALSE;
	}

	/* Access the pixmap associated with each picture */
	PixmapPtr pPixmapDst = Z160EXAGetPicturePixmap(pPictureDst);
	PixmapPtr pPixmapSrc = Z160EXAGetPicturePixmap(pPictureSrc);
	PixmapPtr pPixmapMask = Z160EXAGetPicturePixmap(pPictureMask);

	/* Cannot perform blend if there is no target pixmap. */
	if (NULL == pPixmapDst) {
		return FALSE;
	}

	/* Access screen associated with dst pixmap (same screen as for src pixmap). */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Check the number of entities, and fail if it isn't one. */
	if (pScrn->numEntities != 1) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Z160EXACheckComposite called with number of screen entities (%d) not 1\n",
			pScrn->numEntities);
		return FALSE;
	}

	/* Cannot perform blend if there is no source pixmap. */
	if (NULL == pPixmapSrc) {
		return FALSE;
	}

	/* Cannot perform blend unless screens associated with src and dst pictures are same. */
	if ((pPixmapSrc->drawable.pScreen->myNum !=
		pPixmapDst->drawable.pScreen->myNum)) {
		return FALSE;
	}

	/* Make sure operations can be accelerated on the target pixmap. */
	if (!Z160CanAcceleratePixmap(pPixmapDst)) {
		return FALSE;
	}

	/* Make sure operations can be accelerated on the source pixmap. */
	if (!Z160CanAcceleratePixmap(pPixmapSrc)) {
		return FALSE;
	}

	/* Make sure operations can be accelerated on the optional mask pixmap. */
	if ((NULL != pPixmapMask) && !Z160CanAcceleratePixmap(pPixmapMask)) {
		return FALSE;
	}

	/* Can't accelerate composite unless target pixmap has minimum number of pixels. */
	unsigned pixmapAreaDst = pPixmapDst->drawable.width * pPixmapDst->drawable.height;
	if (pixmapAreaDst < IMX_EXA_MIN_PIXEL_AREA_COMPOSITE) {
		return FALSE;
	}

	/* Can't accelerate composite unless pixmap from non-repeating source picture */
	/* has a minimum number of pixels. */
	if (! pPictureSrc->repeat) {

		unsigned pixmapArea = pPixmapSrc->drawable.width * pPixmapSrc->drawable.height;
		if (pixmapArea < IMX_EXA_MIN_PIXEL_AREA_COMPOSITE) {
			return FALSE;
		}
	}

	/* Can't accelerate composite unless pixmap from defined non-repeating mask picture */
	/* has a minimum number of pixels. */
	if ((NULL != pPictureMask) && !pPictureMask->repeat) {

		unsigned pixmapArea = pPixmapMask->drawable.width * pPixmapMask->drawable.height;
		if (pixmapArea < IMX_EXA_MIN_PIXEL_AREA_COMPOSITE) {
			return FALSE;
		}
	}

	/* Reset this variable if cannot support composite. */
	Bool canComposite = TRUE;

	/* Check if blending operation is supported. */
	if ((0 > op) || (NumZ160SetupBlendOps <= op) ||
		(Z160_BLEND_UNKNOWN == Z160SetupBlendOpTable[op])) {

		canComposite = FALSE;
	}

	/* Determine Z160 config that matches color format used in target picture. */
	Z160Buffer z160BufferDst;
	Bool z160BufferDstDefined = Z160GetPictureConfig(pScrn, pPictureDst, &z160BufferDst);
	if (!z160BufferDstDefined) {
		canComposite = FALSE;
	}

	/* Determine Z160 config that matches color format used in source picture. */
	Z160Buffer z160BufferSrc;
	Bool z160BufferSrcDefined = Z160GetPictureConfig(pScrn, pPictureSrc, &z160BufferSrc);
	if (!z160BufferSrcDefined) {
		canComposite = FALSE;
	}

	/* Determine Z160 config that matches color format used in mask picture. */
	Z160Buffer z160BufferMask;
	Bool z160BufferMaskDefined = FALSE;
	if (NULL != pPictureMask) {

		z160BufferMaskDefined = Z160GetPictureConfig(pScrn, pPictureMask, &z160BufferMask);
		if (!z160BufferMaskDefined) {
			canComposite = FALSE;
		}
	}

	/* Do not accelerate masks that do not have an alpha channel. */
	if (z160BufferMaskDefined) {
		if (0 == PICT_FORMAT_A(pPictureMask->format)) {
			canComposite = FALSE;
		}
	}

	/* Do not accelerate sources with a transform. */
	if (NULL != pPictureSrc->transform) {
		canComposite = FALSE;
	}

	/* Do not accelerate masks, if defined, that have a transform. */
	if ((NULL != pPictureMask) && (NULL != pPictureMask->transform)) {
		canComposite = FALSE;
	}

	/* Do not accelerate when mask, if defined, is repeating. */
	if ((NULL != pPictureMask) && pPictureMask->repeat) {
		canComposite = FALSE;
	}

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

	/* Check whether logging of parameter data when composite is rejected. */
	if (! canComposite) {

		/* Source OP Target */
		if (NULL == pPictureMask) {

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"Z160EXACheckComposite not support: SRC(%s%dx%d,%s%d%s:%d%d%d%d) op=%d DST(%d%s:%d%d%d%d)\n",
				pPictureSrc->repeat ? "R" : "",
				pPictureSrc->pDrawable->width,
				pPictureSrc->pDrawable->height,
				(NULL != pPictureSrc->transform) ? "T" : "",
				PICT_FORMAT_BPP(pPictureSrc->format),
				Z160GetPictureTypeName(pPictureSrc),
				PICT_FORMAT_A(pPictureSrc->format),
				PICT_FORMAT_R(pPictureSrc->format),
				PICT_FORMAT_G(pPictureSrc->format),
				PICT_FORMAT_B(pPictureSrc->format),
				op,
				PICT_FORMAT_BPP(pPictureDst->format),
				Z160GetPictureTypeName(pPictureDst),
				PICT_FORMAT_A(pPictureDst->format),
				PICT_FORMAT_R(pPictureDst->format),
				PICT_FORMAT_G(pPictureDst->format),
				PICT_FORMAT_B(pPictureDst->format));


		/* (Source IN Mask) OP Target */
		} else {

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"Z160EXACheckComposite not support: SRC(%s%dx%d,%s%d%s:%d%d%d%d) MASK(%s%dx%d,%s%d%s:%s%d%d%d%d) op=%d DST(%d%s:%d%d%d%d)\n",
				pPictureSrc->repeat ? "R" : "",
				pPictureSrc->pDrawable->width,
				pPictureSrc->pDrawable->height,
				(NULL != pPictureSrc->transform) ? "T" : "",
				PICT_FORMAT_BPP(pPictureSrc->format),
				Z160GetPictureTypeName(pPictureSrc),
				PICT_FORMAT_A(pPictureSrc->format),
				PICT_FORMAT_R(pPictureSrc->format),
				PICT_FORMAT_G(pPictureSrc->format),
				PICT_FORMAT_B(pPictureSrc->format),
				pPictureMask->repeat ? "R" : "",
				pPictureMask->pDrawable->width,
				pPictureMask->pDrawable->height,
				(NULL != pPictureMask->transform) ? "T" : "",
				PICT_FORMAT_BPP(pPictureMask->format),
				Z160GetPictureTypeName(pPictureMask),
				pPictureMask->componentAlpha ? "C" : "",
				PICT_FORMAT_A(pPictureMask->format),
				PICT_FORMAT_R(pPictureMask->format),
				PICT_FORMAT_G(pPictureMask->format),
				PICT_FORMAT_B(pPictureMask->format),
				op,
				PICT_FORMAT_BPP(pPictureDst->format),
				Z160GetPictureTypeName(pPictureDst),
				PICT_FORMAT_A(pPictureDst->format),
				PICT_FORMAT_R(pPictureDst->format),
				PICT_FORMAT_G(pPictureDst->format),
				PICT_FORMAT_B(pPictureDst->format));
		}

	}

#endif

	return canComposite;
}

static Bool
Z160EXAPrepareComposite(
	int op,
	PicturePtr pPictureSrc,
	PicturePtr pPictureMask,
	PicturePtr pPictureDst,
	PixmapPtr pPixmapSrc,
	PixmapPtr pPixmapMask,
	PixmapPtr pPixmapDst)
{
	/* Access screen associated with dst pixmap (same screen as for src pixmap). */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* NOTE - many preconditions were already verified in the CheckComposite callback. */

	/* Determine Z160 config that matches pixel format used in target picture. */
	Z160Buffer z160BufferDst;
	if (!Z160GetPictureConfig(pScrn, pPictureDst, &z160BufferDst)) {
		return FALSE;
	}

	/* Determine Z160 config that matches pixel format used in source picture. */
	Z160Buffer z160BufferSrc;
	if (!Z160GetPictureConfig(pScrn, pPictureSrc, &z160BufferSrc)) {
		return FALSE;
	}

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Map the Xrender blend op into the Z160 blend op. */
	Z160_BLEND z160BlendOp = Z160SetupBlendOpTable[op];

	/* Setup the target buffer */
	z160_setup_buffer_target(fPtr->gpuContext, &z160BufferDst);

	/* Mask blend? */
	fPtr->gpuOpSetup = FALSE;
	if (NULL != pPictureMask) {
		/* Determine Z160 config that matches pixel format used in mask picture */
		Z160Buffer z160BufferMask;
		if (!Z160GetPictureConfig(pScrn, pPictureMask, &z160BufferMask)) {
			return FALSE;
		}

		/* Blend repeating source using a mask */
		if (pPictureSrc->repeat) {
			/* Source is 1x1 (constant) repeat pattern? */
			if (Z160IsDrawablePixelOnly(pPictureSrc->pDrawable)) {

				z160_setup_blend_const_masked(
					fPtr->gpuContext,
					z160BlendOp,
					&z160BufferSrc,
					&z160BufferMask);

				fPtr->gpuOpSetup = TRUE;
			/* Source is arbitrary sized repeat pattern? */
			} else {

				z160_setup_blend_pattern_masked(
					fPtr->gpuContext,
 					z160BlendOp,
 					&z160BufferSrc,
 					&z160BufferMask);

 				fPtr->gpuOpSetup = TRUE;
 			}

		/* Simple (source IN mask) blend */
		} else {
			z160_setup_blend_image_masked(
				fPtr->gpuContext,
				z160BlendOp,
				&z160BufferSrc,
				&z160BufferMask);

			fPtr->gpuOpSetup = TRUE;
		}
	/* Source only (no mask) blend */
	} else {
		/* Repeating source (pattern)? */
		if (pPictureSrc->repeat) {
			/* Source is 1x1 (constant) repeat pattern? */
			if (Z160IsDrawablePixelOnly(pPictureSrc->pDrawable)) {

				z160_setup_blend_const(
					fPtr->gpuContext,
					z160BlendOp,
					&z160BufferSrc);

				fPtr->gpuOpSetup = TRUE;
			/* Source is arbitrary sized repeat pattern? */
			} else {
				z160_setup_blend_pattern(
					fPtr->gpuContext,
					z160BlendOp,
					&z160BufferSrc);

				fPtr->gpuOpSetup = TRUE;
			}
		/* Simple source blend */
		} else {
			z160_setup_blend_image(fPtr->gpuContext, z160BlendOp, &z160BufferSrc);
			fPtr->gpuOpSetup = TRUE;
		}
	}

	/* Note if the composite operation is being accelerated. */
	if (fPtr->gpuOpSetup) {
		return TRUE;
	}

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

	/* Log info about which operations were not accelerated. */

	/* Source OP Target */
	if (NULL == pPictureMask) {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"Z160EXAPrepareComposite not support: SRC(%s%dx%d,%s%d%s:%d%d%d%d) op=%d DST(%d%s:%d%d%d%d)\n",
			pPictureSrc->repeat ? "R" : "",
			pPictureSrc->pDrawable->width,
			pPictureSrc->pDrawable->height,
			(NULL != pPictureSrc->transform) ? "T" : "",
			PICT_FORMAT_BPP(pPictureSrc->format),
			Z160GetPictureTypeName(pPictureSrc),
			PICT_FORMAT_A(pPictureSrc->format),
			PICT_FORMAT_R(pPictureSrc->format),
			PICT_FORMAT_G(pPictureSrc->format),
			PICT_FORMAT_B(pPictureSrc->format),
			op,
			PICT_FORMAT_BPP(pPictureDst->format),
			Z160GetPictureTypeName(pPictureDst),
			PICT_FORMAT_A(pPictureDst->format),
			PICT_FORMAT_R(pPictureDst->format),
			PICT_FORMAT_G(pPictureDst->format),
			PICT_FORMAT_B(pPictureDst->format));


	/* (Source IN Mask) OP Target */
	} else {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"Z160EXAPrepareComposite not support: SRC(%s%dx%d,%s%d%s:%d%d%d%d) MASK(%s%dx%d,%s%d%s:%s%d%d%d%d) op=%d DST(%d%s:%d%d%d%d)\n",
			pPictureSrc->repeat ? "R" : "",
			pPictureSrc->pDrawable->width,
			pPictureSrc->pDrawable->height,
			(NULL != pPictureSrc->transform) ? "T" : "",
			PICT_FORMAT_BPP(pPictureSrc->format),
			Z160GetPictureTypeName(pPictureSrc),
			PICT_FORMAT_A(pPictureSrc->format),
			PICT_FORMAT_R(pPictureSrc->format),
			PICT_FORMAT_G(pPictureSrc->format),
			PICT_FORMAT_B(pPictureSrc->format),
			pPictureMask->repeat ? "R" : "",
			pPictureMask->pDrawable->width,
			pPictureMask->pDrawable->height,
			(NULL != pPictureMask->transform) ? "T" : "",
			PICT_FORMAT_BPP(pPictureMask->format),
			Z160GetPictureTypeName(pPictureMask),
			pPictureMask->componentAlpha ? "C" : "",
			PICT_FORMAT_A(pPictureMask->format),
			PICT_FORMAT_R(pPictureMask->format),
			PICT_FORMAT_G(pPictureMask->format),
			PICT_FORMAT_B(pPictureMask->format),
			op,
			PICT_FORMAT_BPP(pPictureDst->format),
			Z160GetPictureTypeName(pPictureDst),
			PICT_FORMAT_A(pPictureDst->format),
			PICT_FORMAT_R(pPictureDst->format),
			PICT_FORMAT_G(pPictureDst->format),
			PICT_FORMAT_B(pPictureDst->format));
	}

#endif

	return FALSE;
}

static void
Z160EXAComposite(
	PixmapPtr pPixmapDst,
	int srcX,
	int srcY,
	int maskX,
	int maskY,
	int dstX,
	int dstY,
	int width,
	int height)
{
	/* Access screen associated with dst pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Perform rectangle render based on setup in PrepareComposite */
	switch (z160_get_setup(fPtr->gpuContext)) {

		case Z160_SETUP_BLEND_IMAGE:
			z160_blend_image_rect(
				fPtr->gpuContext,
				dstX, dstY,
				width, height,
				srcX, srcY);
			break;

		case Z160_SETUP_BLEND_IMAGE_MASKED:
			z160_blend_image_masked_rect(
				fPtr->gpuContext,
				dstX, dstY,
				width, height,
				srcX, srcY,
				maskX, maskY);
			break;

		case Z160_SETUP_BLEND_CONST:
			z160_blend_const_rect(
				fPtr->gpuContext,
				dstX, dstY,
				width, height);
			break;

		case Z160_SETUP_BLEND_CONST_MASKED:
			z160_blend_const_masked_rect(
				fPtr->gpuContext,
				dstX, dstY,
				width, height,
				maskX, maskY);
			break;

		case Z160_SETUP_BLEND_PATTERN:
			z160_blend_pattern_rect(
				fPtr->gpuContext,
				dstX, dstY,
				width, height,
				srcX, srcY);
			break;
		case Z160_SETUP_BLEND_PATTERN_MASKED:
			z160_blend_pattern_masked_rect(
				fPtr->gpuContext,
				dstX, dstY,
				width, height,
				srcX, srcY,
				maskX, maskY);
			break;
		default:
			return;
	}

#if IMX_EXA_DEBUG_INSTRUMENT_SYNCS

	++(fPtr->numCompositeBeforeSync);

#endif

#if 0
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		"Z160EXAComposite called with src=(%d-%d,%d-%d) dst=(%d-%d,%d-%d)\n",
		srcX, srcX+width, srcY, srcY+height, dstX, dstX+width, dstY, dstY+height);
#endif
}

static void
Z160EXADoneComposite(PixmapPtr pPixmapDst)
{
	/* Access screen associated with this pixmap */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	z160_flush(fPtr->gpuContext);

	/* Update state. */
	fPtr->gpuSynced = FALSE;
	fPtr->gpuOpSetup = FALSE;
}

static Bool
Z160EXAUploadToScreen(
	PixmapPtr pPixmapDst,
	int dstX,
	int dstY,
	int width,
	int height,
	char* pBufferSrc,
	int pitchSrc)
{
	/* Access screen associated with this pixmap */
	ScreenPtr pScreen = pPixmapDst->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

	/* Cannot support target pixmaps with less than 8 bits per pixel */
	if (8 > pPixmapDst->drawable.bitsPerPixel) {
		return FALSE;
	}

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Wait for the GPU to become idle. */
	exaWaitSync(pScreen);

	/* Compute number of bytes per pixel to transfer. */
	int bytesPerPixel = pPixmapDst->drawable.bitsPerPixel / 8;

	/* Access the pitch for the target pixmap. */
	int pitchDst = exaGetPixmapPitch(pPixmapDst);

	/* Access the starting address for the pixmap. */
	unsigned char* pBufferDst =
		(unsigned char*)Z160EXAGetPixmapAddress(pPixmapDst);

	/* Advance to the starting pixel. */
	pBufferDst += (dstY * pitchDst + dstX * bytesPerPixel);

	/* How many bytes to copy per line of rectangle? */
	int lineCopyBytes = width * bytesPerPixel;

	/* Do the copy */
	while (height-- > 0) {

		memcpy(pBufferDst, pBufferSrc, lineCopyBytes);
		pBufferDst += pitchDst;
		pBufferSrc += pitchSrc;
	}

	return TRUE;
}

static Bool
Z160EXADownloadFromScreen(
	PixmapPtr pPixmapSrc,
	int srcX,
	int srcY,
	int width,
	int height,
	char* pBufferDst,
	int pitchDst)
{
	/* Access screen associated with this pixmap */
	ScreenPtr pScreen = pPixmapSrc->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

	/* Cannot support target pixmaps with less than 8 bits per pixel */
	if (8 > pPixmapSrc->drawable.bitsPerPixel) {
		return FALSE;
	}

	/* Access driver specific data */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Wait for the GPU to become idle. */
	exaWaitSync(pScreen);

	/* Compute number of bytes per pixel to transfer. */
	int bytesPerPixel = pPixmapSrc->drawable.bitsPerPixel / 8;

	/* Access the pitch for the source pixmap. */
	int pitchSrc = exaGetPixmapPitch(pPixmapSrc);

	/* Access the starting address for the pixmap. */
	unsigned char* pBufferSrc =
		(unsigned char*)Z160EXAGetPixmapAddress(pPixmapSrc);

	/* Advance to the starting pixel. */
	pBufferSrc += (srcY * pitchSrc + srcX * bytesPerPixel);

	/* How many bytes to copy per line of rectangle? */
	int lineCopyBytes = width * bytesPerPixel;

	/* Do the copy */
	while (height-- > 0) {

		memcpy(pBufferDst, pBufferSrc, lineCopyBytes);
		pBufferDst += pitchDst;
		pBufferSrc += pitchSrc;
	}

	return TRUE;
}

static void
Z160EXAWaitMarker(ScreenPtr pScreen, int marker)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

	/* Access driver specific data associated with the screen. */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	Z160Sync(fPtr);
}


/* Called by IMXPreInit */
Bool IMX_EXA_PreInit(ScrnInfoPtr pScrn)
{

	XF86ModReqInfo req;
	int errmaj, errmin;
	memset(&req, 0, sizeof(req));
	req.majorversion = EXA_VERSION_MAJOR;
	req.minorversion = EXA_VERSION_MINOR;
	if (!LoadSubModule(pScrn->module, "exa", NULL, NULL, NULL, &req,
				&errmaj, &errmin)) {
		LoaderErrorMsg(NULL, "exa", errmaj, errmin);
		return FALSE;
	}

	/* initialize state of Z160 data structures */
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);
	fPtr->gpuContext = NULL;

	return TRUE;
}


/* Called by IMXScreenInit */
Bool IMX_EXA_ScreenInit(int scrnIndex, ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

	/* Remember the index associated with this screen. */
	fPtr->scrnIndex = pScrn->scrnIndex;

	/* Compute the number of bytes per pixel */
	unsigned bytesPerPixel = ((pScrn->bitsPerPixel + 7) / 8);

	/* Compute the number of bytes used by the screen. */
	fPtr->numScreenBytes = pScrn->displayWidth * pScrn->virtualY * bytesPerPixel;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "physAddr=0x%08x fbstart=0x%08x fbmem=0x%08x fboff=0x%08x\n",
			(int)(pScrn->memPhysBase),
			(int)(imxPtr->fbstart),
			(int)(imxPtr->fbmem),
			imxPtr->fboff);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "framebuffer: size=%dx%d bits=%d screenbytes=%d stride=%u\n",
		pScrn->virtualX,
		pScrn->virtualY,
		pScrn->bitsPerPixel,
		fPtr->numScreenBytes,
		bytesPerPixel * pScrn->displayWidth);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "rgbOffset=%d,%d,%d rgbMask=0x%08x,0x%08x,0x%08x\n",
			(int)(pScrn->offset.red),
			(int)(pScrn->offset.green),
			(int)(pScrn->offset.blue),
			(int)(pScrn->mask.red),
			(int)(pScrn->mask.green),
			(int)(pScrn->mask.blue));

	/* Initialize the Z160 hardware */
	if (!Z160ContextGet(fPtr)) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Initialize Z160 interfaces failed.\n");
		return FALSE;
	}

	/* Initialize EXA. */
	imxPtr->exaDriverPtr = exaDriverAlloc();
	if (NULL == imxPtr->exaDriverPtr) {

		Z160ContextRelease(fPtr);

	} else {

		memset(imxPtr->exaDriverPtr, 0, sizeof(*imxPtr->exaDriverPtr));

		/* Alignment of pixmap pitch is 32 pixels for z430 */
		/* (4 pixels for z160), but times 4 bytes max per pixel. */
		unsigned long pixmapPitchAlign = 32 * 4;

		imxPtr->exaDriverPtr->flags = EXA_OFFSCREEN_PIXMAPS;
		imxPtr->exaDriverPtr->exa_major = EXA_VERSION_MAJOR;
		imxPtr->exaDriverPtr->exa_minor = EXA_VERSION_MINOR;
		imxPtr->exaDriverPtr->memoryBase = imxPtr->fbstart;
		imxPtr->exaDriverPtr->memorySize = fbdevHWGetVidmem(pScrn);
		imxPtr->exaDriverPtr->offScreenBase = fPtr->numScreenBytes;
		imxPtr->exaDriverPtr->pixmapOffsetAlign = Z160_ALIGN_OFFSET;
		imxPtr->exaDriverPtr->pixmapPitchAlign = pixmapPitchAlign;
		imxPtr->exaDriverPtr->maxPitchBytes = Z160_MAX_PITCH_BYTES;
		imxPtr->exaDriverPtr->maxX = Z160_MAX_WIDTH - 1;
		imxPtr->exaDriverPtr->maxY = Z160_MAX_HEIGHT - 1;

		/* Required */
		imxPtr->exaDriverPtr->WaitMarker = Z160EXAWaitMarker;

		/* Solid fill - required */
		imxPtr->exaDriverPtr->PrepareSolid = Z160EXAPrepareSolid;
		imxPtr->exaDriverPtr->Solid = Z160EXASolid;
		imxPtr->exaDriverPtr->DoneSolid = Z160EXADoneSolid;

		/* Copy - required */
		imxPtr->exaDriverPtr->PrepareCopy = Z160EXAPrepareCopy;
		imxPtr->exaDriverPtr->Copy = Z160EXACopy;
		imxPtr->exaDriverPtr->DoneCopy = Z160EXADoneCopy;

		/* Composite */
		imxPtr->exaDriverPtr->CheckComposite = Z160EXACheckComposite;
		imxPtr->exaDriverPtr->PrepareComposite = Z160EXAPrepareComposite;
		imxPtr->exaDriverPtr->Composite = Z160EXAComposite;
		imxPtr->exaDriverPtr->DoneComposite = Z160EXADoneComposite;

		/* Screen upload/download */
		imxPtr->exaDriverPtr->UploadToScreen = Z160EXAUploadToScreen;
		imxPtr->exaDriverPtr->DownloadFromScreen = Z160EXADownloadFromScreen;

#if IMX_EXA_VERSION_COMPILED > IMX_EXA_VERSION(2,2,0)
		/* Prepare/Finish access */
		imxPtr->exaDriverPtr->PrepareAccess = Z160EXAPrepareAccess;
		imxPtr->exaDriverPtr->FinishAccess = Z160EXAFinishAccess;
#endif

#if IMX_EXA_ENABLE_HANDLES_PIXMAPS
		/* For driver pixmap allocation. */
		imxPtr->exaDriverPtr->flags |= EXA_HANDLES_PIXMAPS;
		imxPtr->exaDriverPtr->flags |= EXA_SUPPORTS_PREPARE_AUX;
		imxPtr->exaDriverPtr->flags |= EXA_SUPPORTS_OFFSCREEN_OVERLAPS;

		imxPtr->exaDriverPtr->CreatePixmap2 = Z160EXACreatePixmap2;
		imxPtr->exaDriverPtr->DestroyPixmap = Z160EXADestroyPixmap;
		imxPtr->exaDriverPtr->ModifyPixmapHeader = Z160EXAModifyPixmapHeader;
		imxPtr->exaDriverPtr->PixmapIsOffscreen = Z160EXAPixmapIsOffscreen;
#endif

		if (!exaDriverInit(pScreen, imxPtr->exaDriverPtr)) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "EXA initialization failed.\n");
			free(imxPtr->exaDriverPtr);
			imxPtr->exaDriverPtr = NULL;
			Z160ContextRelease(fPtr);
			return FALSE;
		}

#if IMX_EXA_ENABLE_HANDLES_PIXMAPS
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Driver handles allocation of pixmaps\n");
		unsigned long numAvailPixmapBytes =
			imxPtr->exaDriverPtr->memorySize -
				imxPtr->exaDriverPtr->offScreenBase;
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Offscreen pixmap area of %luK bytes\n", numAvailPixmapBytes / 1024);

		/* Driver allocation of pixmaps will use the built-in */
		/* EXA offscreen memory manager. */
		IMX_EXA_OffscreenInit(pScreen);
#endif
	}

	return TRUE;
}


/* Called by IMXCloseScreen */
Bool IMX_EXA_CloseScreen(int scrnIndex, ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
	IMXPtr imxPtr = IMXPTR(pScrn);
	IMXEXAPtr fPtr = IMXEXAPTR(imxPtr);

#if IMX_EXA_DEBUG_INSTRUMENT_SIZES

	syslog(LOG_INFO | LOG_USER,
		"Z160 Xorg driver: %d = num solid fill rects < 100 pixels\n",
		fPtr->numSolidFillRect100);

	syslog(LOG_INFO | LOG_USER,
		"Z160 Xorg driver: %d = num solid fill rects 101 - 1000 pixels\n",
		fPtr->numSolidFillRect1000);

	syslog(LOG_INFO | LOG_USER,
		"Z160 Xorg driver: %d = num solid fill rects 1001 - 10000 pixels\n",
		fPtr->numSolidFillRect10000);

	syslog(LOG_INFO | LOG_USER,
		"Z160 Xorg driver: %d = num solid fill rects 10001 - 100000 pixels\n",
		fPtr->numSolidFillRect100000);

	syslog(LOG_INFO | LOG_USER,
		"Z160 Xorg driver: %d = num solid fill rects > 100000 pixels\n",
		fPtr->numSolidFillRectLarge);

	syslog(LOG_INFO | LOG_USER,
		"Z160 Xorg driver: %d = num screen copy rects < 100 pixels\n",
		fPtr->numScreenCopyRect100);

	syslog(LOG_INFO | LOG_USER,
		"Z160 Xorg driver: %d = num screen copy rects 101 - 1000 pixels\n",
		fPtr->numScreenCopyRect1000);

	syslog(LOG_INFO | LOG_USER,
		"Z160 Xorg driver: %d = num screen copy rects 1001 - 10000 pixels\n",
		fPtr->numScreenCopyRect10000);

	syslog(LOG_INFO | LOG_USER,
		"Z160 Xorg driver: %d = num screen copy rects 10001 - 100000 pixels\n",
		fPtr->numScreenCopyRect100000);

	syslog(LOG_INFO | LOG_USER,
		"Z160 Xorg driver: %d = num screen copy rects > 100000 pixels\n",
		fPtr->numScreenCopyRectLarge);
#endif

	/* EXA cleanup */
	if (imxPtr->exaDriverPtr) {

#if IMX_EXA_ENABLE_HANDLES_PIXMAPS
		/* Driver allocation of pixmaps will use the built-in */
		/* EXA offscreen memory manager. */
		IMX_EXA_OffscreenFini(pScreen);
#endif

		exaDriverFini(pScreen);
		free(imxPtr->exaDriverPtr);
		imxPtr->exaDriverPtr = NULL;
	}

	/* Shutdown the Z160 hardware access. */
	Z160ContextRelease(fPtr);

	return TRUE;
}
