/*
 * Copyright 2009-2010 Freescale Semiconductor, Inc. All Rights Reserved.
 */

#include "xf86.h"
#include "xf86_OSproc.h"
#include "fbdevhw.h"
#include "exa.h"
#include "imx_type.h"
#include "z160.h"

#define	IMX_EXA_ENABLE_OFFSCREEN_PIXMAPS	1	/* offscreen pixmap support? */
#define	IMX_EXA_ENABLE_SOLID			1	/* solid fill acceleration? */
#define	IMX_EXA_ENABLE_EXA_INTERNAL		1	/* EXA code is part of this driver */

/* Set minimum size (pixel area) for accelerating operations. */
#define	IMX_EXA_MIN_PIXEL_AREA_SOLID		150
#define	IMX_EXA_MIN_PIXEL_AREA_COPY		150
#define	IMX_EXA_MIN_PIXEL_AREA_COMPOSITE	150

/* This flag must be enabled to perform any debug logging */
#define IMX_EXA_DEBUG_MASTER		0

#define	IMX_EXA_DEBUG_INSTRUMENT_SYNCS	(0 && IMX_EXA_DEBUG_MASTER)
#define IMX_EXA_DEBUG_INSTRUMENT_SIZES	(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_PREPARE_SOLID	(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_SOLID		(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_PREPARE_COPY	(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_COPY		(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_CHECK_COMPOSITE	(0 && IMX_EXA_DEBUG_MASTER)
#define	IMX_EXA_DEBUG_GPU_IDLE_TIME	(0 && IMX_EXA_DEBUG_MASTER)

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

	ExaDriverPtr			exaDriverPtr;

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

	/* Flag set if solid/copy/composite Prepare has been called and */
	/* operation is completed (and flag cleared) when Done called. */
	Bool				gpuOpBusy;

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

#if !IMX_EXA_ENABLE_EXA_INTERNAL

/* Function symbols dynamically loaded from EXA module. */
static const char *exaSymbols[] = {
	"exaDriverAlloc",
	"exaDriverInit",
	"exaDriverFini",
	"exaOffscreenAlloc",
	"exaOffscreenFree",
	"exaGetPixmapOffset",
	"exaGetPixmapDriverPrivate",
	"exaGetPixmapPitch",
	"exaGetPixmapSize",
	"exaGetDrawablePixmap",
	"exaMarkSync",
	"exaWaitSync",
	NULL
};

#endif

/* Prototype for function not defined in exa.h */
extern PixmapPtr exaGetDrawablePixmap(DrawablePtr pDrawable);


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
	fPtr->gpuOpBusy = FALSE;

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
void IMXFreeRec(ScrnInfoPtr pScrn)
{
	IMXPtr imxPtr = IMXPTR(pScrn);

	if (NULL == imxPtr->exaDriverPrivate) {
		return;
	}

	xfree(imxPtr->exaDriverPrivate);
	imxPtr->exaDriverPrivate = NULL;
}


#if 0

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

Bool
IMX_GetPixmapProperties(
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

	/* Access screen associated with this pixmap. */
	ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

	/* Make sure pixmap is in framebuffer */
	if (!exaDrawableIsOffscreen(&(pPixmap->drawable))) {
		return FALSE;
	}

	/* Get the physical address of pixmap and its pitch */
	*pPhysAddr = (void*)((unsigned char*)pScrn->memPhysBase + exaGetPixmapOffset(pPixmap));
	*pPitch = exaGetPixmapPitch(pPixmap);

	return TRUE;
}

static
Bool
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

	/* If we get here, then operations on this pixmap can be accelerated. */
	return TRUE;
}

static
Bool
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
	if (!IMX_GetPixmapProperties(pPixmap, &pBuffer->base, &pBuffer->pitch)) {
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
	PixmapPtr pPixmap = exaGetDrawablePixmap(pPicture->pDrawable);
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

#if IMX_EXA_DEBUG_GPU_IDLE_TIME
		xf86DrvMsg(fPtr->scrnIndex, X_INFO,
			"GPU was off for %.2lf secs\n", offTime / 1000.0);
#endif

		/* Other initialization. */
		fPtr->gpuSynced = FALSE;
		fPtr->gpuOpSetup = FALSE;
		fPtr->gpuOpBusy = FALSE;
	}

	return fPtr->gpuContext;
}

static void
Z160MarkOperationBusy(IMXEXAPtr fPtr, Bool busy)
{
	fPtr->gpuOpBusy = busy;

	if (!busy) {
		// Nothing to do
	}
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
	void* gpuContext = fPtr->gpuContext;
	if (NULL == gpuContext) {
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
		z160_sync(gpuContext);

		/* Update state */
		fPtr->gpuSynced = TRUE;
		Z160MarkOperationBusy(fPtr, FALSE);
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

		pPixmap->devPrivate.ptr =
			fPtr->exaDriverPtr->memoryBase + exaGetPixmapOffset(pPixmap);
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

static Bool
Z160EXAPrepareAccess(PixmapPtr pPixmap, int index)
{
	/* Frame buffer memory is not allocated through Z160, so nothing to do. */
	/* But this callback has to be implemented since the offscreen pixmap test */
	/* callback has been overridden. */

	return TRUE;
}

static void
Z160EXAFinishAccess(PixmapPtr pPixmap, int index)
{
	/* Nothing to do, but this callback has to be implemented */
	/* if the prepare access callback is overridden. */
}

static Bool
Z160EXAPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask, Pixel fg)
{
	/* Do we even accelerate solid fill? */
	if (! IMX_EXA_ENABLE_SOLID) {
		return FALSE;
	}

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

	/* Access GPU context */
	void* gpuContext = Z160ContextGet(fPtr);
	if (NULL == gpuContext) {
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
	Z160MarkOperationBusy(fPtr, TRUE);

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

	/* Access the GPU */
	void* gpuContext = Z160ContextGet(fPtr);
	if (NULL == gpuContext) {
		return;
	}

	/* Nothing to unless rectangle has area. */
	if ((x1 >= x2) || (y1 >= y2)) {
		return;
	}

	/* Compute the width and height of the rectangle to fill. */
	int width = x2 - x1;
	int height = y2 - y1;

	/* Determine number of pixels in operation */
//	unsigned opPixels = width * height;

	/* Flag set to accelerate when operation involves minimum number of pixels. */
	/* Or a previous acceleration was started and not yet synced to its completion. */
//	Bool accel = (opPixels >= IMX_EXA_MIN_PIXEL_AREA_SOLID) || !fPtr->gpuSynced;
	Bool accel = TRUE;

	/* Need to prepare for software fallback solid fill? */
	if (!accel && (NULL == fPtr->pGC)) {

		/* Prepare target pixmap for CPU access */
		Z160EXAPreparePipelinedAccess(fPtr->pPixmapDst, EXA_PREPARE_DEST);

		/* Create scratch graphics context */
		fPtr->pGC = GetScratchGC(fPtr->pPixmapDst->drawable.depth,
						fPtr->pPixmapDst->drawable.pScreen);

		/* Change the graphics context properties based on the PrepareSolid parameters. */
		CARD32 parms[3] = {fPtr->solidALU, fPtr->solidPlaneMask, fPtr->solidColor};
		ChangeGC(fPtr->pGC, GCFunction | GCPlaneMask | GCForeground, parms);

		/* Make sure the graphics context is properly setup. */
		ValidateGC(&fPtr->pPixmapDst->drawable, fPtr->pGC);

	/* Need to prepare the GPU for accelerated solid fill? */
	} else if (accel && !fPtr->gpuOpSetup) {

		z160_setup_buffer_target(gpuContext, &fPtr->z160BufferDst);
		z160_setup_fill_solid(gpuContext, fPtr->z160Color);

		fPtr->gpuOpSetup = TRUE;
	}


	/* Perform software fallback solid fill? */
	if (!accel) {

		fbFill(&fPtr->pPixmapDst->drawable, fPtr->pGC, x1, y1, width, height);

	/* Perform GPU accelerated solid fill? */
	} else {

		z160_fill_solid_rect(gpuContext, x1, y1, width, height);
	}

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

	/* Access the GPU */
	void* gpuContext = Z160ContextGet(fPtr);
	if (NULL == gpuContext) {
		return;
	}

	/* Finalize any GPU operations if any where used */
	if (fPtr->gpuOpSetup) {

		/* Flush pending operations to the GPU. */
		z160_flush(gpuContext);

		/* Update state. */
		fPtr->gpuSynced = FALSE;
		fPtr->gpuOpSetup = FALSE;
		Z160MarkOperationBusy(fPtr, FALSE);
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

	/* Access GPU context */
	void* gpuContext = Z160ContextGet(fPtr);
	if (NULL == gpuContext) {
		return FALSE;
	}

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
	Z160MarkOperationBusy(fPtr, TRUE);

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

	/* Access the GPU */
	void* gpuContext = Z160ContextGet(fPtr);
	if (NULL == gpuContext) {
		return;
	}

	/* Determine number of pixels in operation */
//	unsigned opPixels = width * height;

	/* Flag set to accelerate when operation involves minimum number of pixels. */
	/* Or a previous acceleration was started and not yet synced to its completion. */
//	Bool accel = (opPixels >= IMX_EXA_MIN_PIXEL_AREA_SOLID) || !fPtr->gpuSynced;
	Bool accel = TRUE;

	/* Need to prepare for software fallback copy? */
	if (!accel && (NULL == fPtr->pGC)) {

		/* Prepare source and target pixmaps for CPU access */
		Z160EXAPreparePipelinedAccess(fPtr->pPixmapDst, EXA_PREPARE_DEST);
		Z160EXAPreparePipelinedAccess(fPtr->pPixmapSrc, EXA_PREPARE_SRC);

		/* Create scratch graphics context */
		fPtr->pGC = GetScratchGC(fPtr->pPixmapDst->drawable.depth,
						fPtr->pPixmapDst->drawable.pScreen);

		/* Change the graphics context properties based on the PrepareCopy parameters. */
		CARD32 parms[2] = {fPtr->solidALU, fPtr->solidPlaneMask};
		ChangeGC(fPtr->pGC, GCFunction | GCPlaneMask, parms);

		/* Make sure the graphics context is properly setup. */
		ValidateGC(&fPtr->pPixmapDst->drawable, fPtr->pGC);

	/* Need to prepare the GPU for accelerated copy? */
	} else if (accel && !fPtr->gpuOpSetup) {

		z160_setup_buffer_target(gpuContext, &fPtr->z160BufferDst);
		z160_setup_copy(gpuContext, &fPtr->z160BufferSrc,
					fPtr->copyDirX, fPtr->copyDirY);

		fPtr->gpuOpSetup = TRUE;
	}

	/* Perform software fallback copy? */
	if (!accel) {

		fbCopyArea(
			&fPtr->pPixmapSrc->drawable,
			&fPtr->pPixmapDst->drawable,
			fPtr->pGC,
			srcX, srcY,
			width, height,
			dstX, dstY);

	/* Perform GPU accelerated copy? */
	} else {

		z160_copy_rect(gpuContext, dstX, dstY, width, height, srcX, srcY);
	}

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

	/* Access the GPU */
	void* gpuContext = Z160ContextGet(fPtr);
	if (NULL == gpuContext) {
		return;
	}

	/* Finalize any GPU operations if any where used */
	if (fPtr->gpuOpSetup) {

		/* Flush pending operations to the GPU. */
		z160_flush(gpuContext);

		/* Update state */
		fPtr->gpuSynced = FALSE;
		fPtr->gpuOpSetup = FALSE;
		Z160MarkOperationBusy(fPtr, FALSE);
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
	PixmapPtr pPixmapDst = exaGetDrawablePixmap(pPictureDst->pDrawable);
	PixmapPtr pPixmapSrc = exaGetDrawablePixmap(pPictureSrc->pDrawable);
	PixmapPtr pPixmapMask = 
		(NULL != pPictureMask) ? exaGetDrawablePixmap(pPictureMask->pDrawable) : NULL;

	/* Cannot perform blend unless screens associated with src and dst pictures are same. */
	if ((NULL == pPixmapSrc) || (NULL == pPixmapDst) ||
		(pPixmapSrc->drawable.pScreen->myNum != pPixmapDst->drawable.pScreen->myNum)) {

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

	/* Access screen associated with dst pixmap (same screen as for src pixmap). */
	ScrnInfoPtr pScrn = xf86Screens[pPixmapDst->drawable.pScreen->myNum];

	/* Check the number of entities, and fail if it isn't one. */
	if (pScrn->numEntities != 1) {

		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"Z160EXACheckComposite called with number of screen entities (%d) not 1\n",
			pScrn->numEntities);
		return FALSE;
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
	/* Check if mask picture is defined. */
	if (NULL != pPictureMask) {

		z160BufferMaskDefined = Z160GetPictureConfig(pScrn, pPictureMask, &z160BufferMask);
		if (!z160BufferMaskDefined) {
			canComposite = FALSE;
		}
	}
	
	/* If the target has no color channels, then make sure the source and optional */
	/* also do not have any color channels. */
	if (z160BufferDstDefined && (0 == PICT_FORMAT_RGB(pPictureDst->format))) {

		if (z160BufferSrcDefined && (0 != PICT_FORMAT_RGB(pPictureSrc->format))) {
			canComposite = FALSE;
		}
		if (z160BufferMaskDefined && (0 != PICT_FORMAT_RGB(pPictureMask->format))) {
			canComposite = FALSE;
		}
	}

	/* Do not support masks that have color channel data if the picture is not */
	/* configured for component alpha. */
	if (z160BufferMaskDefined) {
		if (0 == PICT_FORMAT_A(pPictureMask->format)) {
			canComposite = FALSE;
		}
	}

	/* Check if source picture has a transform. */
	if (NULL != pPictureSrc->transform) {
		canComposite = FALSE;
	}

	/* Check if mask picture has a transform. */
	if ((NULL != pPictureMask) && (NULL != pPictureMask->transform)) {
		canComposite = FALSE;
	}

	/* Can perform repeating source picture (pattern) blend when a mask */
	/* is used as long as the source picture is 1x1 (a constant). */
	if (pPictureSrc->repeat && (NULL != pPictureMask) &&
		((1 != pPictureSrc->pDrawable->width) || (1 != pPictureSrc->pDrawable->height))) {

		canComposite = FALSE;
	}

	/* Cannot perform blend if mask picture has repeat set. */
	if ((NULL != pPictureMask) && pPictureMask->repeat) {
		canComposite = FALSE;
	}

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

	/* Check whether logging of parameter data when composite is rejected. */
	if (! canComposite) {

		/* Source OP Target */
		if (NULL == pPictureMask) {

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"Z160EXACheckComposite not support: SRC(%s%dx%d,%s%d-%d:%d%d%d%d) op=%d DST(%d-%d:%d%d%d%d)\n",
				pPictureSrc->repeat ? "R" : "",
				pPictureSrc->pDrawable->width,
				pPictureSrc->pDrawable->height,
				(NULL != pPictureSrc->transform) ? "T" : "",
				PICT_FORMAT_TYPE(pPictureSrc->format),
				PICT_FORMAT_BPP(pPictureSrc->format),
				PICT_FORMAT_A(pPictureSrc->format),
				PICT_FORMAT_R(pPictureSrc->format),
				PICT_FORMAT_G(pPictureSrc->format),
				PICT_FORMAT_B(pPictureSrc->format),
				op,
				PICT_FORMAT_TYPE(pPictureDst->format),
				PICT_FORMAT_BPP(pPictureDst->format),
				PICT_FORMAT_A(pPictureDst->format),
				PICT_FORMAT_R(pPictureDst->format),
				PICT_FORMAT_G(pPictureDst->format),
				PICT_FORMAT_B(pPictureDst->format));


		/* (Source IN Mask) OP Target */
		} else {

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"Z160EXACheckComposite not support: SRC(%s%dx%d,%s%d-%d:%d%d%d%d) MASK(%s%dx%d,%s%d-%d:%s%d%d%d%d) op=%d DST(%d-%d:%d%d%d%d)\n",
				pPictureSrc->repeat ? "R" : "",
				pPictureSrc->pDrawable->width,
				pPictureSrc->pDrawable->height,
				(NULL != pPictureSrc->transform) ? "T" : "",
				PICT_FORMAT_TYPE(pPictureSrc->format),
				PICT_FORMAT_BPP(pPictureSrc->format),
				PICT_FORMAT_A(pPictureSrc->format),
				PICT_FORMAT_R(pPictureSrc->format),
				PICT_FORMAT_G(pPictureSrc->format),
				PICT_FORMAT_B(pPictureSrc->format),
				pPictureMask->repeat ? "R" : "",
				pPictureMask->pDrawable->width,
				pPictureMask->pDrawable->height,
				(NULL != pPictureMask->transform) ? "T" : "",
				PICT_FORMAT_TYPE(pPictureMask->format),
				PICT_FORMAT_BPP(pPictureMask->format),
				pPictureMask->componentAlpha ? "C" : "",
				PICT_FORMAT_A(pPictureMask->format),
				PICT_FORMAT_R(pPictureMask->format),
				PICT_FORMAT_G(pPictureMask->format),
				PICT_FORMAT_B(pPictureMask->format),
				op,
				PICT_FORMAT_TYPE(pPictureDst->format),
				PICT_FORMAT_BPP(pPictureDst->format),
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

	/* Access GPU context */
	void* gpuContext = Z160ContextGet(fPtr);
	if (NULL == gpuContext) {
		return FALSE;
	}

	/* Map the Xrender blend op into the Z160 blend op. */
	Z160_BLEND z160BlendOp = Z160SetupBlendOpTable[op];

	/* Setup the target buffer */
	z160_setup_buffer_target(gpuContext, &z160BufferDst);

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
			if ((1 == pPictureSrc->pDrawable->width) &&
				(1 == pPictureSrc->pDrawable->height)) {

				z160_setup_blend_const_masked(
					gpuContext,
					z160BlendOp,
					&z160BufferSrc,
					&z160BufferMask);

				fPtr->gpuOpSetup = TRUE;
			}

		/* Simple (source IN mask) blend */
		} else {

			z160_setup_blend_image_masked(
				gpuContext,
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
			if ((1 == pPictureSrc->pDrawable->width) &&
				(1 == pPictureSrc->pDrawable->height)) {

				z160_setup_blend_const(
					gpuContext,
					z160BlendOp,
					&z160BufferSrc);

				fPtr->gpuOpSetup = TRUE;

			/* Source is arbitrary sized repeat pattern? */
			} else {

#if 0
// Disabled until libz160 can support larger intermediate packet buffer.
				z160_setup_blend_pattern(
					gpuContext,
					z160BlendOp,
					&z160BufferSrc);

				fPtr->gpuOpSetup = TRUE;
#endif
			}

		/* Simple source blend */
		} else {

			z160_setup_blend_image(gpuContext, z160BlendOp, &z160BufferSrc);
			fPtr->gpuOpSetup = TRUE;
		}
	}

	/* Note if the composite operation is being accelerated. */
	if (fPtr->gpuOpSetup) {

		Z160MarkOperationBusy(fPtr, TRUE);
		return TRUE;
	}

#if IMX_EXA_DEBUG_CHECK_COMPOSITE

	/* Log info about which operations were not accelerated. */

	/* Source OP Target */
	if (NULL == pPictureMask) {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"Z160EXAPrepareComposite not support: SRC(%s%dx%d,%s%d-%d:%d%d%d%d) op=%d DST(%d-%d:%d%d%d%d)\n",
			pPictureSrc->repeat ? "R" : "",
			pPictureSrc->pDrawable->width,
			pPictureSrc->pDrawable->height,
			(NULL != pPictureSrc->transform) ? "T" : "",
			PICT_FORMAT_TYPE(pPictureSrc->format),
			PICT_FORMAT_BPP(pPictureSrc->format),
			PICT_FORMAT_A(pPictureSrc->format),
			PICT_FORMAT_R(pPictureSrc->format),
			PICT_FORMAT_G(pPictureSrc->format),
			PICT_FORMAT_B(pPictureSrc->format),
			op,
			PICT_FORMAT_TYPE(pPictureDst->format),
			PICT_FORMAT_BPP(pPictureDst->format),
			PICT_FORMAT_A(pPictureDst->format),
			PICT_FORMAT_R(pPictureDst->format),
			PICT_FORMAT_G(pPictureDst->format),
			PICT_FORMAT_B(pPictureDst->format));


	/* (Source IN Mask) OP Target */
	} else {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"Z160EXAPrepareComposite not support: SRC(%s%dx%d,%s%d-%d:%d%d%d%d) MASK(%s%dx%d,%s%d-%d:%s%d%d%d%d) op=%d DST(%d-%d:%d%d%d%d)\n",
			pPictureSrc->repeat ? "R" : "",
			pPictureSrc->pDrawable->width,
			pPictureSrc->pDrawable->height,
			(NULL != pPictureSrc->transform) ? "T" : "",
			PICT_FORMAT_TYPE(pPictureSrc->format),
			PICT_FORMAT_BPP(pPictureSrc->format),
			PICT_FORMAT_A(pPictureSrc->format),
			PICT_FORMAT_R(pPictureSrc->format),
			PICT_FORMAT_G(pPictureSrc->format),
			PICT_FORMAT_B(pPictureSrc->format),
			pPictureMask->repeat ? "R" : "",
			pPictureMask->pDrawable->width,
			pPictureMask->pDrawable->height,
			(NULL != pPictureMask->transform) ? "T" : "",
			PICT_FORMAT_TYPE(pPictureMask->format),
			PICT_FORMAT_BPP(pPictureMask->format),
			pPictureMask->componentAlpha ? "C" : "",
			PICT_FORMAT_A(pPictureMask->format),
			PICT_FORMAT_R(pPictureMask->format),
			PICT_FORMAT_G(pPictureMask->format),
			PICT_FORMAT_B(pPictureMask->format),
			op,
			PICT_FORMAT_TYPE(pPictureDst->format),
			PICT_FORMAT_BPP(pPictureDst->format),
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

	/* Access the GPU */
	void* gpuContext = Z160ContextGet(fPtr);
	if (NULL == gpuContext) {
		return;
	}

	/* Perform rectangle render based on setup in PrepareComposite */
	switch (z160_get_setup(gpuContext)) {

		case Z160_SETUP_BLEND_IMAGE:
			z160_blend_image_rect(
				gpuContext,
				dstX, dstY,
				width, height,
				srcX, srcY);
			break;

		case Z160_SETUP_BLEND_IMAGE_MASKED:
			z160_blend_image_masked_rect(
				gpuContext,
				dstX, dstY,
				width, height,
				srcX, srcY,
				maskX, maskY);
			break;

		case Z160_SETUP_BLEND_CONST:
			z160_blend_const_rect(
				gpuContext,
				dstX, dstY,
				width, height);
			break;

		case Z160_SETUP_BLEND_CONST_MASKED:
			z160_blend_const_masked_rect(
				gpuContext,
				dstX, dstY,
				width, height,
				maskX, maskY);
			break;

		case Z160_SETUP_BLEND_PATTERN:
			z160_blend_pattern_rect(
				gpuContext,
				dstX, dstY,
				width, height,
				srcX, srcY);
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

	/* Flush pending operations to the GPU. */
	/* Access the GPU */
	void* gpuContext = Z160ContextGet(fPtr);
	if (NULL == gpuContext) {
		return;
	}

	z160_flush(gpuContext);

	/* Update state. */
	fPtr->gpuSynced = FALSE;
	fPtr->gpuOpSetup = FALSE;
	Z160MarkOperationBusy(fPtr, FALSE);
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
	unsigned char* pBufferDst = fPtr->exaDriverPtr->memoryBase + exaGetPixmapOffset(pPixmapDst);

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
	unsigned char* pBufferSrc = fPtr->exaDriverPtr->memoryBase + exaGetPixmapOffset(pPixmapSrc);

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

#if !IMX_EXA_ENABLE_EXA_INTERNAL

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

	/* Load required EXA symbols */
	xf86LoaderReqSymLists(exaSymbols, NULL);
#endif

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
	fPtr->exaDriverPtr = exaDriverAlloc();
	if (NULL == fPtr->exaDriverPtr) {

		Z160ContextRelease(fPtr);

	} else {

		memset(fPtr->exaDriverPtr, 0, sizeof(*fPtr->exaDriverPtr));

		int flags = 0;
		unsigned long memorySize;

#if IMX_EXA_ENABLE_OFFSCREEN_PIXMAPS
		flags |= EXA_OFFSCREEN_PIXMAPS;
		memorySize = fbdevHWGetVidmem(pScrn);
#else
		memorySize = fPtr->numScreenBytes;
#endif

		fPtr->exaDriverPtr->flags = flags;
		fPtr->exaDriverPtr->exa_major = EXA_VERSION_MAJOR;
		fPtr->exaDriverPtr->exa_minor = EXA_VERSION_MINOR;
		fPtr->exaDriverPtr->memoryBase = imxPtr->fbstart;
		fPtr->exaDriverPtr->memorySize = memorySize;
		fPtr->exaDriverPtr->offScreenBase = fPtr->numScreenBytes;
		fPtr->exaDriverPtr->pixmapOffsetAlign = Z160_ALIGN_OFFSET;
//		fPtr->exaDriverPtr->pixmapPitchAlign = Z160_ALIGN_PITCH;
		fPtr->exaDriverPtr->pixmapPitchAlign = 32;	// 32 for z430; 4 for z160
		fPtr->exaDriverPtr->maxPitchBytes = Z160_MAX_PITCH_BYTES;
		fPtr->exaDriverPtr->maxX = Z160_MAX_WIDTH - 1;
		fPtr->exaDriverPtr->maxY = Z160_MAX_HEIGHT - 1;

		/* Required */
		fPtr->exaDriverPtr->WaitMarker = Z160EXAWaitMarker;

		/* Solid fill - required */
		fPtr->exaDriverPtr->PrepareSolid = Z160EXAPrepareSolid;
		fPtr->exaDriverPtr->Solid = Z160EXASolid;
		fPtr->exaDriverPtr->DoneSolid = Z160EXADoneSolid;

		/* Copy - required */
		fPtr->exaDriverPtr->PrepareCopy = Z160EXAPrepareCopy;
		fPtr->exaDriverPtr->Copy = Z160EXACopy;
		fPtr->exaDriverPtr->DoneCopy = Z160EXADoneCopy;

		/* Composite */
		fPtr->exaDriverPtr->CheckComposite = Z160EXACheckComposite;
		fPtr->exaDriverPtr->PrepareComposite = Z160EXAPrepareComposite;
		fPtr->exaDriverPtr->Composite = Z160EXAComposite;
		fPtr->exaDriverPtr->DoneComposite = Z160EXADoneComposite;

		/* Screen upload/download */
		fPtr->exaDriverPtr->UploadToScreen = Z160EXAUploadToScreen;
		fPtr->exaDriverPtr->DownloadFromScreen = Z160EXADownloadFromScreen;

#if EXA_VERSION_MINOR >= 2
		/* Prepare/Finish access */
		fPtr->exaDriverPtr->PrepareAccess = Z160EXAPrepareAccess;
		fPtr->exaDriverPtr->FinishAccess = Z160EXAFinishAccess;
#endif

		if (!exaDriverInit(pScreen, fPtr->exaDriverPtr)) {

			xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "EXA initialization failed.\n");
			xfree(fPtr->exaDriverPtr);
			fPtr->exaDriverPtr = NULL;
			Z160ContextRelease(fPtr);
			return FALSE;
		}
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
	if (fPtr->exaDriverPtr) {
		exaDriverFini(pScreen);
		xfree(fPtr->exaDriverPtr);
		fPtr->exaDriverPtr = NULL;
	}

	/* Shutdown the Z160 hardware access. */
	Z160ContextRelease(fPtr);

	return TRUE;
}
