/* ***** BEGIN LICENSE BLOCK *****  
 * Source last modified: $Id: filefmt.c,v 1.1 2005/02/26 01:47:34 jrecker Exp $ 
 *   
 * Portions Copyright (c) 1995-2005 RealNetworks, Inc. All Rights Reserved.  
 *       
 * The contents of this file, and the files included with this file, 
 * are subject to the current version of the RealNetworks Public 
 * Source License (the "RPSL") available at 
 * http://www.helixcommunity.org/content/rpsl unless you have licensed 
 * the file under the current version of the RealNetworks Community 
 * Source License (the "RCSL") available at 
 * http://www.helixcommunity.org/content/rcsl, in which case the RCSL 
 * will apply. You may also obtain the license terms directly from 
 * RealNetworks.  You may not use this file except in compliance with 
 * the RPSL or, if you have a valid RCSL with RealNetworks applicable 
 * to this file, the RCSL.  Please see the applicable RPSL or RCSL for 
 * the rights, obligations and limitations governing use of the 
 * contents of the file. 
 *   
 * This file is part of the Helix DNA Technology. RealNetworks is the 
 * developer of the Original Code and owns the copyrights in the 
 * portions it created. 
 *   
 * This file, and the files included with this file, is distributed 
 * and made available on an 'AS IS' basis, WITHOUT WARRANTY OF ANY 
 * KIND, EITHER EXPRESS OR IMPLIED, AND REALNETWORKS HEREBY DISCLAIMS 
 * ALL SUCH WARRANTIES, INCLUDING WITHOUT LIMITATION, ANY WARRANTIES 
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, QUIET 
 * ENJOYMENT OR NON-INFRINGEMENT. 
 *  
 * Technology Compatibility Kit Test Suite(s) Location:  
 *    http://www.helixcommunity.org/content/tck  
 *  
 * Contributor(s):  
 *   
 * ***** END LICENSE BLOCK ***** */  

/**************************************************************************************
 * Fixed-point HE-AAC decoder
 * Jon Recker (jrecker@real.com)
 * February 2005
 *
 * filefmt.c - ADIF and ADTS header decoding, raw block handling
 **************************************************************************************/

#include "coder.h"

static uint32_t LatmGetValue(BitStreamInfo *s)
{
	uint32_t l, value;
	uint8_t bytesForValue = GetBits(s, 2);

	value = 0;
	for (l = 0; l <= bytesForValue; l++)
		value = (value << 8) | (uint8_t)GetBits(s, 8);

	return value;
}

static int AudioSpecificConfig(LATMHeader *header, BitStreamInfo *s)
{
	int samplingFrequency = 0;
	int channelConfiguration;
	int samplingFrequencyIndex;
	int audioObjectType = GetBits(s, 5);

	if (audioObjectType == 31)
		audioObjectType = GetBits(s, 6) + 32;

	samplingFrequencyIndex = GetBits(s, 4);
	if (samplingFrequencyIndex == 0xF)
		samplingFrequency = GetBits(s, 24);
	else
		samplingFrequency = sampRateTab[samplingFrequencyIndex];

	channelConfiguration = GetBits(s, 4);

	if (audioObjectType != AAC_PROFILE_LC + 1
			|| !samplingFrequency
			|| channelConfiguration > 7)
		return ERR_AAC_INVALID_HEADER;

	header->samplingFrequency = samplingFrequency;
	header->samplingFrequencyIndex = samplingFrequencyIndex;
	header->audioObjectType = audioObjectType;
	header->channelConfiguration = channelConfiguration;

	return ERR_AAC_NONE;
}

 /**************************************************************************************
 * Function:    UnpackLATMHeader
 *
 * Description: parse the LATM frame header and initialize decoder state
 *
 * Inputs:      valid AACDecInfo struct
 *              double pointer to buffer with complete LATM frame header
 *
 * Outputs:     filled in LATM struct
 *              updated buffer pointer
 *              updated bit offset
 *              updated number of available bits
 *
 * Return:      0 if successful, error code (< 0) if error
 *
 **************************************************************************************/
int UnpackLATMHeader(AACDecInfo *aacDecInfo, unsigned char **buf, int *bitOffset, int *bitsAvail, int *bytesFrames)
{
	int bitsUsed, bytesHeader;
	PSInfoBase *psi;
	BitStreamInfo bsi;
	LATMHeader *fhLATM;

	/* validate pointers */
	if (!aacDecInfo || !aacDecInfo->psInfoBase)
		return ERR_AAC_NULL_POINTER;

	psi = (PSInfoBase *)(aacDecInfo->psInfoBase);
	fhLATM = &(psi->fhLATM);

	/* init bitstream reader */
	SetBitstreamPointer(&bsi, (*bitsAvail) >> 3, *buf);
	GetBits(&bsi, *bitOffset);

	if (!GetBits(&bsi, 1)) { // useSameStreamMux
		int audioMuxVersion = GetBits(&bsi, 1);
		if (audioMuxVersion && !GetBits(&bsi, 1)) { // audioMuxVersionA
			int ret;
			int ascLen, allStreamsSameTimeFraming, numSubFrames, numProgram, numLayer;
			if (audioMuxVersion)
				LatmGetValue(&bsi); //taraBufferFullness

			allStreamsSameTimeFraming = GetBits(&bsi, 1);
			numSubFrames = GetBits(&bsi, 6);
			numProgram = GetBits(&bsi, 4);
			numLayer = GetBits(&bsi, 3);
			if (numProgram > 0
					|| numLayer > 0
					|| numSubFrames > 0
					|| !allStreamsSameTimeFraming)
				return ERR_AAC_INVALID_HEADER;

			if (audioMuxVersion)
				ascLen = LatmGetValue(&bsi);
			bitsUsed = CalcBitsUsed(&bsi, *buf, *bitOffset);

			ret = AudioSpecificConfig(fhLATM, &bsi);
			if (ret < 0)
				return ret;

			GetBits(&bsi, ascLen - (CalcBitsUsed(&bsi, *buf, *bitOffset) - bitsUsed));

			psi->sampRateIdx = fhLATM->samplingFrequencyIndex;
			psi->nChans = channelMapTab[fhLATM->channelConfiguration];

			aacDecInfo->nChans = fhLATM->channelConfiguration;
			aacDecInfo->sampRate = sampRateTab[fhLATM->samplingFrequencyIndex];
			aacDecInfo->profile = fhLATM->audioObjectType;

			aacDecInfo->bitRate = 0;
			aacDecInfo->sbrEnabled = 0;
			aacDecInfo->prevBlockID = AAC_ID_INVALID;
			aacDecInfo->currBlockID = AAC_ID_INVALID;
			aacDecInfo->currInstTag = -1;
		}

		fhLATM->frameLengthType = GetBits(&bsi, 3);
		if (fhLATM->frameLengthType == 0) {
			fhLATM->frameLength = 0;
			GetBits(&bsi, 8); // latmBufferFullness
		} else if (fhLATM->frameLengthType == 1) {
			fhLATM->frameLength = GetBits(&bsi, 9);
			if (!fhLATM->frameLength)
				return ERR_AAC_INVALID_HEADER;
			fhLATM->frameLength = (fhLATM->frameLength + 20) * 8;
		} else
			return ERR_AAC_INVALID_HEADER;

		if (GetBits(&bsi, 1)) { // otherDataPresent
			int esc, tmp;
			int otherDataLenBits = 0;
			if (audioMuxVersion) {
				otherDataLenBits = LatmGetValue(&bsi);
			} else do {
				esc = GetBits(&bsi, 1);
				tmp = GetBits(&bsi, 8);
				otherDataLenBits = (otherDataLenBits << 8) + tmp;
			} while (esc);
		}

		if (GetBits(&bsi, 1)) // crcCheckPresent
			GetBits(&bsi, 8);

		fhLATM->init = 1;
	}

	bitsUsed = CalcBitsUsed(&bsi, *buf, *bitOffset);
	if ((bitsUsed - *bitOffset) >> 3 > LATM_HEADER_BYTES)
		return ERR_AAC_INVALID_HEADER;

	bytesHeader = LATM_HEADER_BYTES;
	if (fhLATM->init && !fhLATM->frameLengthType) {
		int tmp;
		do {
			tmp = (uint8_t)GetBits(&bsi, 8);
			fhLATM->frameLength += tmp;
			bytesHeader++;
		} while (tmp == 0xFF);
	} else
		return ERR_AAC_INVALID_HEADER;

	if (fhLATM->frameLength > 2048)
		return ERR_AAC_INVALID_HEADER;

	if (bytesFrames)
		*bytesFrames = fhLATM->frameLength + bytesHeader;

	bitsUsed = CalcBitsUsed(&bsi, *buf, *bitOffset);
	*buf += (bitsUsed + *bitOffset) >> 3;
	*bitOffset = (bitsUsed + *bitOffset) & 0x07;
	*bitsAvail -= bitsUsed;

	return ERR_AAC_NONE;
}

int GetADTSFrameLength(unsigned char *inbuf, int bitsAvail, int *bytesFrames)
{
	unsigned char layer;                          /* MPEG layer - should be 0 */
	unsigned char profile;                        /* 0 = main, 1 = LC, 2 = SSR, 3 = reserved */
	unsigned char sampRateIdx;                    /* sample rate index range = [0, 11] */
	unsigned char channelConfig;                  /* 0 = implicit, >0 = use default table */
	unsigned int frameLength;                     /* frame length */
	BitStreamInfo bsi;

	if ((bitsAvail + 7 ) >> 3 < ADTS_HEADER_BYTES) {
		if (bytesFrames)
			*bytesFrames = ADTS_HEADER_BYTES;
		return ERR_AAC_INDATA_HEADER_UNDERFLOW;
	}

	/* init bitstream reader */
	SetBitstreamPointer(&bsi, (bitsAvail + 7) >> 3, inbuf);

	/* verify that first 12 bits of header are syncword */
	if (GetBits(&bsi, 12) != 0x0fff) {
		return ERR_AAC_INVALID_HEADER;
	}

	GetBits(&bsi, 1);
	layer =         GetBits(&bsi, 2);
	GetBits(&bsi, 1);
	profile =       GetBits(&bsi, 2);
	sampRateIdx =   GetBits(&bsi, 4);
	GetBits(&bsi, 1);
	channelConfig = GetBits(&bsi, 3);
	GetBits(&bsi, 4);
	frameLength =   GetBits(&bsi, 13);

	if (layer != 0 || profile != AAC_PROFILE_LC ||
		sampRateIdx >= NUM_SAMPLE_RATES || channelConfig >= NUM_DEF_CHAN_MAPS)
		return ERR_AAC_INVALID_HEADER;

	if (frameLength > 2048)
		return ERR_AAC_INVALID_HEADER;

	if (bytesFrames)
		*bytesFrames = frameLength;

	return 0;
}

 /**************************************************************************************
 * Function:    UnpackADTSHeader
 *
 * Description: parse the ADTS frame header and initialize decoder state
 *
 * Inputs:      valid AACDecInfo struct
 *              double pointer to buffer with complete ADTS frame header (byte aligned)
 *                header size = 7 bytes, plus 2 if CRC
 *
 * Outputs:     filled in ADTS struct
 *              updated buffer pointer
 *              updated bit offset
 *              updated number of available bits
 *
 * Return:      0 if successful, error code (< 0) if error
 *
 * TODO:        test CRC
 *              verify that fixed fields don't change between frames
 **************************************************************************************/
int UnpackADTSHeader(AACDecInfo *aacDecInfo, unsigned char **buf, int *bitOffset, int *bitsAvail, int *bytesFrames)
{
	int bitsUsed;
	PSInfoBase *psi;
	BitStreamInfo bsi;
	ADTSHeader *fhADTS;

	/* validate pointers */
	if (!aacDecInfo || !aacDecInfo->psInfoBase)
		return ERR_AAC_NULL_POINTER;

	if ((*bitsAvail + 7 ) >> 3 < ADTS_HEADER_BYTES) {
		if (bytesFrames)
			*bytesFrames = ADTS_HEADER_BYTES;
		return ERR_AAC_INDATA_HEADER_UNDERFLOW;
	}

	psi = (PSInfoBase *)(aacDecInfo->psInfoBase);
	fhADTS = &(psi->fhADTS);

	/* init bitstream reader */
	SetBitstreamPointer(&bsi, (*bitsAvail + 7) >> 3, *buf);
	GetBits(&bsi, *bitOffset);

	/* verify that first 12 bits of header are syncword */
	if (GetBits(&bsi, 12) != 0x0fff) {
		return ERR_AAC_INVALID_HEADER;
	}

	/* fixed fields - should not change from frame to frame */ 
	fhADTS->id =               GetBits(&bsi, 1);
	fhADTS->layer =            GetBits(&bsi, 2);
	fhADTS->protectBit =       GetBits(&bsi, 1);
	fhADTS->profile =          GetBits(&bsi, 2);
	fhADTS->sampRateIdx =      GetBits(&bsi, 4);
	fhADTS->privateBit =       GetBits(&bsi, 1);
	fhADTS->channelConfig =    GetBits(&bsi, 3);
	fhADTS->origCopy =         GetBits(&bsi, 1);
	fhADTS->home =             GetBits(&bsi, 1);

	/* variable fields - can change from frame to frame */ 
	fhADTS->copyBit =          GetBits(&bsi, 1);
	fhADTS->copyStart =        GetBits(&bsi, 1);
	fhADTS->frameLength =      GetBits(&bsi, 13);
	fhADTS->bufferFull =       GetBits(&bsi, 11);
	fhADTS->numRawDataBlocks = GetBits(&bsi, 2) + 1;

	/* note - MPEG4 spec, correction 1 changes how CRC is handled when protectBit == 0 and numRawDataBlocks > 1 */
	if (fhADTS->protectBit == 0)
		fhADTS->crcCheckWord = GetBits(&bsi, 16);

	/* byte align */
	ByteAlignBitstream(&bsi);	/* should always be aligned anyway */

	/* check validity of header */
	if (fhADTS->layer != 0 || fhADTS->profile != AAC_PROFILE_LC ||
		fhADTS->sampRateIdx >= NUM_SAMPLE_RATES || fhADTS->channelConfig >= NUM_DEF_CHAN_MAPS)
		return ERR_AAC_INVALID_HEADER;

#ifndef AAC_ENABLE_MPEG4
	if (fhADTS->id != 1)
		return ERR_AAC_MPEG4_UNSUPPORTED;
#endif

	/* update codec info */
	psi->sampRateIdx = fhADTS->sampRateIdx;
	if (!psi->useImpChanMap)
		psi->nChans = channelMapTab[fhADTS->channelConfig];

	/* syntactic element fields will be read from bitstream for each element */
	aacDecInfo->prevBlockID = AAC_ID_INVALID;
	aacDecInfo->currBlockID = AAC_ID_INVALID;
	aacDecInfo->currInstTag = -1;

	/* fill in user-accessible data (TODO - calc bitrate, handle tricky channel config cases) */
	aacDecInfo->bitRate = 0;
	aacDecInfo->nChans = psi->nChans;
	aacDecInfo->sampRate = sampRateTab[psi->sampRateIdx];
	aacDecInfo->profile = fhADTS->profile;
	aacDecInfo->sbrEnabled = 0;
	aacDecInfo->adtsBlocksLeft = fhADTS->numRawDataBlocks;

	if (bytesFrames)
		*bytesFrames = fhADTS->frameLength;

	/* update bitstream reader */
	bitsUsed = CalcBitsUsed(&bsi, *buf, *bitOffset);
	*buf += (bitsUsed + *bitOffset) >> 3;
	*bitOffset = (bitsUsed + *bitOffset) & 0x07;
	*bitsAvail -= bitsUsed ;
	if (*bitsAvail < 0)
		return ERR_AAC_INDATA_UNDERFLOW;

	return ERR_AAC_NONE;
}

/**************************************************************************************
 * Function:    GetADTSChannelMapping
 *
 * Description: determine the number of channels from implicit mapping rules
 *
 * Inputs:      valid AACDecInfo struct
 *              pointer to start of raw_data_block
 *              bit offset
 *              bits available 
 *
 * Outputs:     updated number of channels
 *
 * Return:      0 if successful, error code (< 0) if error
 *
 * Notes:       calculates total number of channels using rules in 14496-3, 4.5.1.2.1
 *              does not attempt to deduce speaker geometry
 **************************************************************************************/
int GetADTSChannelMapping(AACDecInfo *aacDecInfo, unsigned char *buf, int bitOffset, int bitsAvail)
{
	int ch, nChans, elementChans, err;
	PSInfoBase *psi;

	/* validate pointers */
	if (!aacDecInfo || !aacDecInfo->psInfoBase)
		return ERR_AAC_NULL_POINTER;
	psi = (PSInfoBase *)(aacDecInfo->psInfoBase);

	nChans = 0;
	do {
		/* parse next syntactic element */
		err = DecodeNextElement(aacDecInfo, &buf, &bitOffset, &bitsAvail);
		if (err)
			return err;

		elementChans = elementNumChans[aacDecInfo->currBlockID];
		if (nChans + elementChans > AAC_MAX_NCHANS)
			return ERR_AAC_NCHANS_TOO_HIGH;

		nChans += elementChans;

		for (ch = 0; ch < elementChans; ch++) {
			err = DecodeNoiselessData(aacDecInfo, &buf, &bitOffset, &bitsAvail, ch);
			if (err)
				return err;
		}
	} while (aacDecInfo->currBlockID != AAC_ID_END);

	if (nChans <= 0)
		return ERR_AAC_CHANNEL_MAP;

	/* update number of channels in codec state and user-accessible info structs */ 
	psi->nChans = nChans;
	aacDecInfo->nChans = psi->nChans;
	psi->useImpChanMap = 1;

	return ERR_AAC_NONE;
}

/**************************************************************************************
 * Function:    GetNumChannelsADIF
 *
 * Description: get number of channels from program config elements in an ADIF file
 *
 * Inputs:      array of filled-in program config element structures
 *              number of PCE's
 *
 * Outputs:     none
 *
 * Return:      total number of channels in file
 *              -1 if error (invalid number of PCE's or unsupported mode)
 **************************************************************************************/
static int GetNumChannelsADIF(ProgConfigElement *fhPCE, int nPCE)
{
	int i, j, nChans;

	if (nPCE < 1 || nPCE > MAX_NUM_PCE_ADIF)
		return -1;

	nChans = 0;
	for (i = 0; i < nPCE; i++) {
		/* for now: only support LC, no channel coupling */
		if (fhPCE[i].profile != AAC_PROFILE_LC || fhPCE[i].numCCE > 0)
			return -1;

		/* add up number of channels in all channel elements (assume all single-channel) */
        nChans += fhPCE[i].numFCE;
        nChans += fhPCE[i].numSCE;
        nChans += fhPCE[i].numBCE;
        nChans += fhPCE[i].numLCE;

		/* add one more for every element which is a channel pair */
        for (j = 0; j < fhPCE[i].numFCE; j++) {
            if (CHAN_ELEM_IS_CPE(fhPCE[i].fce[j]))
                nChans++;
        }
        for (j = 0; j < fhPCE[i].numSCE; j++) {
            if (CHAN_ELEM_IS_CPE(fhPCE[i].sce[j]))
                nChans++;
        }
        for (j = 0; j < fhPCE[i].numBCE; j++) {
            if (CHAN_ELEM_IS_CPE(fhPCE[i].bce[j]))
                nChans++;
        }

	}

	return nChans;
}

/**************************************************************************************
 * Function:    GetSampleRateIdxADIF
 *
 * Description: get sampling rate index from program config elements in an ADIF file
 *
 * Inputs:      array of filled-in program config element structures
 *              number of PCE's
 *
 * Outputs:     none
 *
 * Return:      sample rate of file
 *              -1 if error (invalid number of PCE's or sample rate mismatch)
 **************************************************************************************/
static int GetSampleRateIdxADIF(ProgConfigElement *fhPCE, int nPCE)
{
	int i, idx;

	if (nPCE < 1 || nPCE > MAX_NUM_PCE_ADIF)
		return -1;

	/* make sure all PCE's have the same sample rate */
	idx = fhPCE[0].sampRateIdx;
	for (i = 1; i < nPCE; i++) {
		if (fhPCE[i].sampRateIdx != idx)
			return -1;
	}

	return idx;
}

/**************************************************************************************
 * Function:    UnpackADIFHeader
 *
 * Description: parse the ADIF file header and initialize decoder state
 *
 * Inputs:      valid AACDecInfo struct
 *              double pointer to buffer with complete ADIF header 
 *                (starting at 'A' in 'ADIF' tag)
 *              pointer to bit offset
 *              pointer to number of valid bits remaining in inbuf
 *
 * Outputs:     filled-in ADIF struct
 *              updated buffer pointer
 *              updated bit offset
 *              updated number of available bits
 *
 * Return:      0 if successful, error code (< 0) if error
 **************************************************************************************/
int UnpackADIFHeader(AACDecInfo *aacDecInfo, unsigned char **buf, int *bitOffset, int *bitsAvail)
{
	int i, bitsUsed;
	PSInfoBase *psi;
	BitStreamInfo bsi;
	ADIFHeader *fhADIF;
	ProgConfigElement *pce;

	/* validate pointers */
	if (!aacDecInfo || !aacDecInfo->psInfoBase)
		return ERR_AAC_NULL_POINTER;
	psi = (PSInfoBase *)(aacDecInfo->psInfoBase);

	/* init bitstream reader */
	SetBitstreamPointer(&bsi, (*bitsAvail + 7) >> 3, *buf);
	GetBits(&bsi, *bitOffset);

	/* unpack ADIF file header */
	fhADIF = &(psi->fhADIF);
	pce = psi->pce;

	/* verify that first 32 bits of header are "ADIF" */
	if (GetBits(&bsi, 8) != 'A' || GetBits(&bsi, 8) != 'D' || GetBits(&bsi, 8) != 'I' || GetBits(&bsi, 8) != 'F')
		return ERR_AAC_INVALID_HEADER;

	/* read ADIF header fields */
	fhADIF->copyBit = GetBits(&bsi, 1);
	if (fhADIF->copyBit) {
		for (i = 0; i < ADIF_COPYID_SIZE; i++)
			fhADIF->copyID[i] = GetBits(&bsi, 8);
	}
	fhADIF->origCopy = GetBits(&bsi, 1);
	fhADIF->home =     GetBits(&bsi, 1);
	fhADIF->bsType =   GetBits(&bsi, 1);
	fhADIF->bitRate =  GetBits(&bsi, 23);
	fhADIF->numPCE =   GetBits(&bsi, 4) + 1;	/* add 1 (so range = [1, 16]) */
	if (fhADIF->bsType == 0)
		fhADIF->bufferFull = GetBits(&bsi, 20);

	/* parse all program config elements */
	for (i = 0; i < fhADIF->numPCE; i++)
		DecodeProgramConfigElement(pce + i, &bsi);

	/* byte align */
	ByteAlignBitstream(&bsi);

	/* update codec info */
	psi->nChans = GetNumChannelsADIF(pce, fhADIF->numPCE);
	psi->sampRateIdx = GetSampleRateIdxADIF(pce, fhADIF->numPCE);

	/* check validity of header */
	if (psi->nChans < 0 || psi->sampRateIdx < 0 || psi->sampRateIdx >= NUM_SAMPLE_RATES)
		return ERR_AAC_INVALID_HEADER;
								
	/* syntactic element fields will be read from bitstream for each element */
	aacDecInfo->prevBlockID = AAC_ID_INVALID;
	aacDecInfo->currBlockID = AAC_ID_INVALID;
	aacDecInfo->currInstTag = -1;

	/* fill in user-accessible data */
	aacDecInfo->bitRate = 0;
	aacDecInfo->nChans = psi->nChans;
	aacDecInfo->sampRate = sampRateTab[psi->sampRateIdx];
	aacDecInfo->profile = pce[0].profile;
	aacDecInfo->sbrEnabled = 0;

	/* update bitstream reader */
	bitsUsed = CalcBitsUsed(&bsi, *buf, *bitOffset);
	*buf += (bitsUsed + *bitOffset) >> 3;
	*bitOffset = (bitsUsed + *bitOffset) & 0x07;
	*bitsAvail -= bitsUsed ;
	if (*bitsAvail < 0)
		return ERR_AAC_INDATA_UNDERFLOW;

	return ERR_AAC_NONE;
}

/**************************************************************************************
 * Function:    SetRawBlockParams
 *
 * Description: set internal state variables for decoding a stream of raw data blocks
 *
 * Inputs:      valid AACDecInfo struct
 *              flag indicating source of parameters (from previous headers or passed 
 *                explicitly by caller)
 *              number of channels
 *              sample rate
 *              profile ID
 *
 * Outputs:     updated state variables in aacDecInfo
 *
 * Return:      0 if successful, error code (< 0) if error
 *
 * Notes:       if copyLast == 1, then psi->nChans, psi->sampRateIdx, and 
 *                aacDecInfo->profile are not changed (it's assumed that we already 
 *                set them, such as by a previous call to UnpackADTSHeader())
 *              if copyLast == 0, then the parameters we passed in are used instead
 **************************************************************************************/
int SetRawBlockParams(AACDecInfo *aacDecInfo, int copyLast, int nChans, int sampRate, int profile)
{
	int idx;
	PSInfoBase *psi;

	/* validate pointers */
	if (!aacDecInfo || !aacDecInfo->psInfoBase)
		return ERR_AAC_NULL_POINTER;
	psi = (PSInfoBase *)(aacDecInfo->psInfoBase);

	if (!copyLast) {
		aacDecInfo->profile = profile;
		psi->nChans = nChans;
		for (idx = 0; idx < NUM_SAMPLE_RATES; idx++) {
			if (sampRate == sampRateTab[idx]) {
				psi->sampRateIdx = idx;
				break;
			}
		}
		if (idx == NUM_SAMPLE_RATES)
			return ERR_AAC_INVALID_FRAME;
	}
	aacDecInfo->nChans = psi->nChans;
	aacDecInfo->sampRate = sampRateTab[psi->sampRateIdx];

	/* check validity of header */
	if (psi->sampRateIdx >= NUM_SAMPLE_RATES || psi->sampRateIdx < 0 || aacDecInfo->profile != AAC_PROFILE_LC)
		return ERR_AAC_RAWBLOCK_PARAMS;

	return ERR_AAC_NONE;
}

/**************************************************************************************
 * Function:    PrepareRawBlock
 *
 * Description: reset per-block state variables for raw blocks (no ADTS/ADIF headers)
 *
 * Inputs:      valid AACDecInfo struct
 *
 * Outputs:     updated state variables in aacDecInfo
 *
 * Return:      0 if successful, error code (< 0) if error
 **************************************************************************************/
int PrepareRawBlock(AACDecInfo *aacDecInfo)
{
//	PSInfoBase *psi;

	/* validate pointers */
	if (!aacDecInfo || !aacDecInfo->psInfoBase)
		return ERR_AAC_NULL_POINTER;
//	psi = (PSInfoBase *)(aacDecInfo->psInfoBase);

	/* syntactic element fields will be read from bitstream for each element */
	aacDecInfo->prevBlockID = AAC_ID_INVALID;
	aacDecInfo->currBlockID = AAC_ID_INVALID;
	aacDecInfo->currInstTag = -1;

	/* fill in user-accessible data */
	aacDecInfo->bitRate = 0;
	aacDecInfo->sbrEnabled = 0;

	return ERR_AAC_NONE;
}

/**************************************************************************************
 * Function:    FlushCodec
 *
 * Description: flush internal codec state (after seeking, for example)
 *
 * Inputs:      valid AACDecInfo struct
 *
 * Outputs:     updated state variables in aacDecInfo
 *
 * Return:      0 if successful, error code (< 0) if error
 *
 * Notes:       only need to clear data which is persistent between frames 
 *                (such as overlap buffer)
 **************************************************************************************/
int FlushCodec(AACDecInfo *aacDecInfo)
{
	PSInfoBase *psi;

	/* validate pointers */
	if (!aacDecInfo || !aacDecInfo->psInfoBase)
		return ERR_AAC_NULL_POINTER;
	psi = (PSInfoBase *)(aacDecInfo->psInfoBase);
	
	ClearBuffer(psi->overlap, AAC_MAX_NCHANS * AAC_MAX_NSAMPS * sizeof(int));
	ClearBuffer(psi->prevWinShape, AAC_MAX_NCHANS * sizeof(int));

	return ERR_AAC_NONE;
}
