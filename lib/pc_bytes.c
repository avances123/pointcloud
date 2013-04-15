/***********************************************************************
* pc_bytes.c
*
*  Support for "dimensional compression", which is a catch-all
*  term for applying compression separately on each dimension
*  of a PCPATCH collection of PCPOINTS.
*
*  Depending on the character of the data, one of these schemes
*  will be used:
*
*  - run-length encoding
*  - significant-bit removal
*  - deflate
*
*  PgSQL Pointcloud is free and open source software provided 
*  by the Government of Canada
*  Copyright (c) 2013 Natural Resources Canada
*
***********************************************************************/

#include <stdarg.h>
#include <assert.h>
#include <math.h>
#include <values.h>
#include "pc_api_internal.h"
#include "zlib.h"



void
pc_bytes_free(PCBYTES pcb)
{
    if ( ! pcb.readonly )
        pcfree(pcb.bytes);
}

PCBYTES
pc_bytes_make(const PCDIMENSION *dim, uint32_t npoints)
{
    PCBYTES pcb;
    pcb.size = dim->size * npoints;
    pcb.bytes = pcalloc(pcb.size);
    pcb.npoints = npoints;
    pcb.interpretation = dim->interpretation;
    pcb.compression = PC_DIM_NONE;
    pcb.readonly = PC_FALSE;
    return pcb;
}

static PCBYTES
pc_bytes_clone(PCBYTES pcb)
{
    PCBYTES pcbnew = pcb;
    pcbnew.bytes = pcalloc(pcb.size);
    memcpy(pcbnew.bytes, pcb.bytes, pcb.size);
    return pcbnew;
}

PCBYTES
pc_bytes_encode(PCBYTES pcb, int compression)
{
    PCBYTES epcb;
    switch ( compression )
    {
        case PC_DIM_RLE:
        {
            epcb = pc_bytes_run_length_encode(pcb);
            break;
        }
        case PC_DIM_SIGBITS:
        {
            epcb = pc_bytes_sigbits_encode(pcb);
            break;
        }
        case PC_DIM_ZLIB:
        {
            epcb = pc_bytes_zlib_encode(pcb);
            break;
        }
        case PC_DIM_NONE:
        {
            epcb = pc_bytes_clone(pcb);
            break;
        }
        default:
        {
            pcerror("pc_bytes_encode: Uh oh");
        }
    }
    return epcb;
}

PCBYTES
pc_bytes_decode(PCBYTES epcb)
{
    PCBYTES pcb;
    switch ( epcb.compression )
    {
        case PC_DIM_RLE:
        {
            pcb = pc_bytes_run_length_decode(epcb);
            break;
        }
        case PC_DIM_SIGBITS:
        {
            pcb = pc_bytes_sigbits_decode(epcb);
            break;
        }
        case PC_DIM_ZLIB:
        {
            pcb = pc_bytes_zlib_decode(epcb);
            break;
        }
        case PC_DIM_NONE:
        {
            pcb = pc_bytes_clone(epcb);
            break;
        }
        default:
        {
            pcerror("pc_bytes_decode: Uh oh");
        }
    }
    return pcb;
}


/**
* How many distinct runs of values are there in this array?
* One? Two? Five? Great news for run-length encoding!
* N? Not so great news.
*/
uint32_t
pc_bytes_run_count(const PCBYTES *pcb)
{
	int i;
	const uint8_t *ptr0;
	const uint8_t *ptr1;
	size_t size = pc_interpretation_size(pcb->interpretation);
	uint32_t runcount = 1;

	for ( i = 1; i < pcb->npoints; i++ )
	{
		ptr0 = pcb->bytes + (i-1)*size;
		ptr1 = pcb->bytes + i*size;
		if ( memcmp(ptr0, ptr1, size) != 0 )
		{
			runcount++;
		}
	}
	return runcount;
}

/**
* Take the uncompressed bytes and run-length encode (RLE) them.
* Structure of RLE array as:
* <uint8> number of elements
* <val> value
* ...
*/
PCBYTES
pc_bytes_run_length_encode(const PCBYTES pcb)
{
	int i;
	uint8_t *buf, *bufptr;
	const uint8_t *bytesptr;
	const uint8_t *runstart;
	uint8_t *bytes_rle;
	size_t size = pc_interpretation_size(pcb.interpretation);
	uint8_t runlength = 1;
    PCBYTES pcbout = pcb;

	/* Allocate more size than we need (worst case: n elements, n runs) */
	buf = pcalloc(pcb.npoints*size + sizeof(uint8_t)*pcb.npoints);
	bufptr = buf;

	/* First run starts at the start! */
	runstart = pcb.bytes;

	for ( i = 1; i <= pcb.npoints; i++ )
	{
		bytesptr = pcb.bytes + i*size;
		/* Run continues... */
		if ( i < pcb.npoints && runlength < 255 && memcmp(runstart, bytesptr, size) == 0  )
		{
			runlength++;
		}
		else
		{
			/* Write # elements in the run */
			*bufptr = runlength;
			bufptr += 1;
			/* Write element value */
			memcpy(bufptr, runstart, size);
			bufptr += size;
			/* Advance read head */
			runstart = bytesptr;
			runlength = 1;
		}
	}
	/* Length of buffer */
    pcbout.size = (bufptr - buf);
	/* Write out shortest buffer possible */
	bytes_rle = pcalloc(pcbout.size);
	memcpy(bytes_rle, buf, pcbout.size);
	pcfree(buf);
	/* We're going to replace the current buffer */
    pcbout.bytes = bytes_rle;
    pcbout.compression = PC_DIM_RLE;
    pcbout.readonly = PC_FALSE;
	return pcbout;
}

/**
* Take the compressed bytes and run-length dencode (RLE) them.
* Structure of RLE array is:
* <uint8> number of elements
* <val> value
* ...
*/
PCBYTES
pc_bytes_run_length_decode(const PCBYTES pcb)
{
	int i, n;
	uint8_t *bytes;
	uint8_t *bytes_ptr;
	const uint8_t *bytes_rle_ptr = pcb.bytes;
	const uint8_t *bytes_rle_end = pcb.bytes + pcb.size;

	size_t size = pc_interpretation_size(pcb.interpretation);
    size_t size_out;
	uint8_t runlength;
	uint32_t npoints = 0;
    PCBYTES pcbout = pcb;

    assert(pcb.compression == PC_DIM_RLE);

	/* Count up how big our output is. */
	while( bytes_rle_ptr < bytes_rle_end )
	{
		npoints += *bytes_rle_ptr;
		bytes_rle_ptr += 1 + size;
	}

    assert(npoints == pcb.npoints);

	/* Alocate output and fill it up */
    size_out = size * npoints;
	bytes = pcalloc(size_out);
	bytes_ptr = bytes;
	bytes_rle_ptr = pcb.bytes;
	while ( bytes_rle_ptr < bytes_rle_end )
	{
		n = *bytes_rle_ptr;
		bytes_rle_ptr += 1;
		for ( i = 0; i < n; i++ )
		{
			memcpy(bytes_ptr, bytes_rle_ptr, size);
			bytes_ptr += size;
		}
		bytes_rle_ptr += size;
	}
    pcbout.compression = PC_DIM_NONE;
    pcbout.size = size_out;
    pcbout.bytes = bytes;
    pcbout.readonly = PC_FALSE;
	return pcbout;
}


/**
* RLE bytes consist of a <byte:count><word:value><byte:count><word:value> pattern
* so we can hope from word to word and flip each one in place.
*/
static PCBYTES
pc_bytes_run_length_flip_endian(PCBYTES pcb)
{
    int i, n;
	uint8_t *bytes_ptr = pcb.bytes;
    uint8_t *end_ptr = pcb.bytes + pcb.size;
    uint8_t tmp;
	size_t size = pc_interpretation_size(pcb.interpretation);

    assert(pcb.compression == PC_DIM_RLE);
    assert(pcb.npoints > 0);

    /* If the type isn't multibyte, it doesn't need flipping */
    if ( size < 2 )
        return pcb;

    /* Don't try to modify read-only memory, make some fresh memory */
    if ( pcb.readonly == PC_TRUE )
    {
        uint8_t *oldbytes = pcb.bytes;
        pcb.bytes = pcalloc(pcb.size);
        memcpy(pcb.bytes, oldbytes, pcb.size);
        pcb.readonly = PC_FALSE;
    }

    bytes_ptr++; /* Advance past count */

    /* Visit each entry and flip the word, skip the count */
    while( bytes_ptr < end_ptr )
    {

        /* Swap the bytes in a way that makes sense for this word size */
        for ( n = 0; n < size/2; n++ )
        {
            tmp = bytes_ptr[n];
            bytes_ptr[n] = bytes_ptr[size-n-1];
            bytes_ptr[size-n-1] = tmp;
        }

        /* Move past this word */
        bytes_ptr += size;
        /* Advance past next count */
        bytes_ptr++;
    }

    return pcb;
}


uint8_t
pc_bytes_sigbits_count_8(const PCBYTES *pcb, uint32_t *nsigbits)
{
	static uint8_t nbits = 8;
    uint8_t *bytes = (uint8_t*)(pcb->bytes);
	uint8_t elem_and = bytes[0];
	uint8_t elem_or = bytes[0];
	uint32_t commonbits = nbits;
	int i;

	for ( i = 0; i < pcb->npoints; i++ )
	{
		elem_and &= bytes[i];
		elem_or |= bytes[i];
	}

	while ( elem_and != elem_or )
	{
		elem_and >>= 1;
		elem_or >>= 1;
		commonbits -= 1;
	}
	elem_and <<= nbits - commonbits;
	if ( nsigbits ) *nsigbits = commonbits;
	return elem_and;
}

uint16_t
pc_bytes_sigbits_count_16(const PCBYTES *pcb, uint32_t *nsigbits)
{
	static int nbits = 16;
	uint16_t *bytes = (uint16_t*)(pcb->bytes);
	uint16_t elem_and = bytes[0];
	uint16_t elem_or = bytes[0];
	uint32_t commonbits = nbits;
	int i;

	for ( i = 0; i < pcb->npoints; i++ )
	{
		elem_and &= bytes[i];
		elem_or |= bytes[i];
	}

	while ( elem_and != elem_or )
	{
		elem_and >>= 1;
		elem_or >>= 1;
		commonbits -= 1;
	}
	elem_and <<= nbits - commonbits;
	if ( nsigbits ) *nsigbits = commonbits;
	return elem_and;
}

uint32_t
pc_bytes_sigbits_count_32(const PCBYTES *pcb, uint32_t *nsigbits)
{
	static int nbits = 32;
	uint32_t *bytes = (uint32_t*)(pcb->bytes);
	uint32_t elem_and = bytes[0];
	uint32_t elem_or = bytes[0];
	uint32_t commonbits = nbits;
	int i;

	for ( i = 0; i < pcb->npoints; i++ )
	{
		elem_and &= bytes[i];
		elem_or |= bytes[i];
	}

	while ( elem_and != elem_or )
	{
		elem_and >>= 1;
		elem_or >>= 1;
		commonbits -= 1;
	}
	elem_and <<= nbits - commonbits;
	if ( nsigbits ) *nsigbits = commonbits;
	return elem_and;
}

uint64_t
pc_bytes_sigbits_count_64(const PCBYTES *pcb, uint32_t *nsigbits)
{
	static int nbits = 64;
	uint64_t *bytes = (uint64_t*)(pcb->bytes);
	uint64_t elem_and = bytes[0];
	uint64_t elem_or = bytes[0];
	uint32_t commonbits = nbits;
	int i;

	for ( i = 0; i < pcb->npoints; i++ )
	{
		elem_and &= bytes[i];
		elem_or |= bytes[i];
	}

	while ( elem_and != elem_or )
	{
		elem_and >>= 1;
		elem_or >>= 1;
		commonbits -= 1;
	}
	elem_and <<= nbits - commonbits;
	if ( nsigbits ) *nsigbits = commonbits;
	return elem_and;
}

/**
* How many bits are shared by all elements of this array?
*/
uint32_t
pc_bytes_sigbits_count(const PCBYTES *pcb)
{
    size_t size = pc_interpretation_size(pcb->interpretation);
    uint32_t nbits = -1;
    switch ( size )
    {
        case 1: /* INT8, UINT8 */
        {
            uint8_t commonvalue = pc_bytes_sigbits_count_8(pcb, &nbits);
            break;
        }
        case 2: /* INT16, UINT16 */
        {
            uint16_t commonvalue = pc_bytes_sigbits_count_16(pcb, &nbits);
            break;
        }
        case 4: /* INT32, UINT32 */
        {
            uint32_t commonvalue = pc_bytes_sigbits_count_32(pcb, &nbits);
            break;
        }
        case 8: /* DOUBLE, INT64, UINT64 */
        {
            uint64_t commonvalue = pc_bytes_sigbits_count_64(pcb, &nbits);
            break;
        }
        default:
        {
            pcerror("pc_bytes_sigbits_count cannot handle interpretation %d", pcb->interpretation);
            return -1;
        }
    }
    return nbits;
}


/**
* Encoded array:
* <uint8> number of bits per unique section
* <uint8> common bits for the array
* [n_bits]... unique bits packed in
* Size of encoded array comes out in ebytes_size.
*/
PCBYTES
pc_bytes_sigbits_encode_8(const PCBYTES pcb, uint8_t commonvalue, uint8_t commonbits)
{
    int i;
    int shift;
    uint8_t *bytes = (uint8_t*)(pcb.bytes);
    /* How wide are our words? */
    static int bitwidth = 8;
    /* How wide are our unique values? */
    int nbits = bitwidth - commonbits;
    /* Size of output buffer (#bits/8+1remainder+2metadata) */
    size_t size_out = (nbits * pcb.npoints / 8) + 3;
    uint8_t *bytes_out = pcalloc(size_out);
    /* Use this to zero out the parts that are common */
    uint8_t mask = (0xFF >> commonbits);
    /* Write head */
    uint8_t *byte_ptr = bytes_out;
    /* What bit are we writing to now? */
    int bit = bitwidth;
    /* Write to... */
    PCBYTES pcbout = pcb;

    /* Number of unique bits goes up front */
    *byte_ptr = nbits; byte_ptr++;
    /* The common value we'll add the unique values to */
    *byte_ptr = commonvalue; byte_ptr++;

    /* All the values are the same... */
    if ( bitwidth == commonbits )
    {
        pcbout.size = size_out;
        pcbout.bytes = bytes_out;
        pcbout.compression = PC_DIM_SIGBITS;
        return pcbout;
    }

    for ( i = 0; i < pcb.npoints; i++ )
    {
        uint8_t val = bytes[i];
        /* Clear off common parts */
        val &= mask;
        /* How far to move unique parts to get to write head? */
        shift = bit - nbits;
        /* If positive, we can fit this part into the current word */
        if ( shift >= 0 )
        {
            val <<= shift;
            *byte_ptr |= val;
            bit -= nbits;
            if ( bit <= 0 )
            {
                bit = bitwidth;
                byte_ptr++;
            }
        }
        /* If negative, then we need to split this part across words */
        else
        {
            /* First the bit into the current word */
            uint8_t v = val;
            int s = abs(shift);
            v >>= s;
            *byte_ptr |= v;
            /* The reset to write the next word */
            bit = bitwidth;
            byte_ptr++;
            v = val;
            shift = bit - s;
            /* But only those parts we didn't already write */
            v <<= shift;
            *byte_ptr |= v;
            bit -= s;
        }
    }

    pcbout.size = size_out;
    pcbout.bytes = bytes_out;
    pcbout.compression = PC_DIM_SIGBITS;
    return pcbout;
}

/**
* Encoded array:
* <uint16> number of bits per unique section
* <uint16> common bits for the array
* [n_bits]... unique bits packed in
* Size of encoded array comes out in ebytes_size.
*/
PCBYTES
pc_bytes_sigbits_encode_16(const PCBYTES pcb, uint16_t commonvalue, uint8_t commonbits)
{
    int i;
    int shift;
    uint16_t *bytes = (uint16_t*)(pcb.bytes);

    /* How wide are our words? */
    static int bitwidth = 16;
    /* How wide are our unique values? */
    int nbits = bitwidth - commonbits;
    /* Size of output buffer (#bits/8+1remainder+4metadata)  */
    size_t size_out_raw = (nbits * pcb.npoints / 8) + 5;
    /* Make sure buffer is size to hold all our words */
    size_t size_out = size_out_raw + (size_out_raw % 2);
    uint8_t *bytes_out = pcalloc(size_out);
    /* Use this to zero out the parts that are common */
    uint16_t mask = (0xFFFF >> commonbits);
    /* Write head */
    uint16_t *byte_ptr = (uint16_t*)(bytes_out);
    /* What bit are we writing to now? */
    int bit = bitwidth;
    /* Write to... */
    PCBYTES pcbout = pcb;

    /* Number of unique bits goes up front */
    *byte_ptr = nbits; byte_ptr++;
    /* The common value we'll add the unique values to */
    *byte_ptr = commonvalue; byte_ptr++;

    /* All the values are the same... */
    if ( bitwidth == commonbits )
    {
        pcbout.size = size_out;
        pcbout.bytes = bytes_out;
        pcbout.compression = PC_DIM_SIGBITS;
        return pcbout;
    }

    for ( i = 0; i < pcb.npoints; i++ )
    {
        uint16_t val = bytes[i];
        /* Clear off common parts */
        val &= mask;
        /* How far to move unique parts to get to write head? */
        shift = bit - nbits;
        /* If positive, we can fit this part into the current word */
        if ( shift >= 0 )
        {
            val <<= shift;
            *byte_ptr |= val;
            bit -= nbits;
            if ( bit <= 0 )
            {
                bit = bitwidth;
                byte_ptr++;
            }
        }
        /* If negative, then we need to split this part across words */
        else
        {
            /* First the bit into the current word */
            uint16_t v = val;
            int s = abs(shift);
            v >>= s;
            *byte_ptr |= v;
            /* The reset to write the next word */
            bit = bitwidth;
            byte_ptr++;
            v = val;
            shift = bit - s;
            /* But only those parts we didn't already write */
            v <<= shift;
            *byte_ptr |= v;
            bit -= s;
        }
    }

    pcbout.size = size_out;
    pcbout.bytes = bytes_out;
    pcbout.compression = PC_DIM_SIGBITS;
    return pcbout;
}

/**
* Encoded array:
* <uint32> number of bits per unique section
* <uint32> common bits for the array
* [n_bits]... unique bits packed in
* Size of encoded array comes out in ebytes_size.
*/
PCBYTES
pc_bytes_sigbits_encode_32(const PCBYTES pcb, uint32_t commonvalue, uint8_t commonbits)
{
    int i;
    int shift;
    uint32_t *bytes = (uint32_t*)(pcb.bytes);

    /* How wide are our words? */
    static int bitwidth = 32;
    /* How wide are our unique values? */
    int nbits = bitwidth - commonbits;
    /* Size of output buffer (#bits/8+1remainder+8metadata) */
    size_t size_out_raw = (nbits * pcb.npoints / 8) + 9;
    size_t size_out = size_out_raw + (4 - (size_out_raw % 4));
    uint8_t *bytes_out = pcalloc(size_out);
    /* Use this to zero out the parts that are common */
    uint32_t mask = (0xFFFFFFFF >> commonbits);
    /* Write head */
    uint32_t *byte_ptr = (uint32_t*)bytes_out;
    /* What bit are we writing to now? */
    int bit = bitwidth;
    /* Write to... */
    PCBYTES pcbout = pcb;

    /* Number of unique bits goes up front */
    *byte_ptr = nbits; byte_ptr++;
    /* The common value we'll add the unique values to */
    *byte_ptr = commonvalue; byte_ptr++;

    /* All the values are the same... */
    if ( bitwidth == commonbits )
    {
        pcbout.size = size_out;
        pcbout.bytes = bytes_out;
        pcbout.compression = PC_DIM_SIGBITS;
        return pcbout;
    }

    for ( i = 0; i < pcb.npoints; i++ )
    {
        uint32_t val = bytes[i];
        /* Clear off common parts */
        val &= mask;
        /* How far to move unique parts to get to write head? */
        shift = bit - nbits;
        /* If positive, we can fit this part into the current word */
        if ( shift >= 0 )
        {
            val <<= shift;
            *byte_ptr |= val;
            bit -= nbits;
            if ( bit <= 0 )
            {
                bit = bitwidth;
                byte_ptr++;
            }
        }
        /* If negative, then we need to split this part across words */
        else
        {
            /* First the bit into the current word */
            uint32_t v = val;
            int s = abs(shift);
            v >>= s;
            *byte_ptr |= v;
            /* The reset to write the next word */
            bit = bitwidth;
            byte_ptr++;
            v = val;
            shift = bit - s;
            /* But only those parts we didn't already write */
            v <<= shift;
            *byte_ptr |= v;
            bit -= s;
        }
    }

    pcbout.size = size_out;
    pcbout.bytes = bytes_out;
    pcbout.compression = PC_DIM_SIGBITS;
    return pcbout;
}

/**
* Convert a raw byte array into with common bits stripped and the
* remaining bits packed in.
* <uint8|uint16|uint32> number of bits per unique section
* <uint8|uint16|uint32> common bits for the array
* [n_bits]... unique bits packed in
* Size of encoded array comes out in ebytes_size.
*/
PCBYTES
pc_bytes_sigbits_encode(const PCBYTES pcb)
{
    size_t size = pc_interpretation_size(pcb.interpretation);
    uint32_t nbits;
    switch ( size )
    {
        case 1:
        {
            uint8_t commonvalue = pc_bytes_sigbits_count_8(&pcb, &nbits);
            return pc_bytes_sigbits_encode_8(pcb, commonvalue, nbits);
        }
        case 2:
        {
            uint16_t commonvalue = pc_bytes_sigbits_count_16(&pcb, &nbits);
            return pc_bytes_sigbits_encode_16(pcb, commonvalue, nbits);
        }
        case 4:
        {
            uint32_t commonvalue = pc_bytes_sigbits_count_32(&pcb, &nbits);
            return pc_bytes_sigbits_encode_32(pcb, commonvalue, nbits);
        }
        default:
        {
            pcerror("pc_bytes_sigbits_encode cannot handle interpretation %d", pcb.interpretation);
        }
    }
    pcerror("Uh Oh");
    return pcb;
}

static PCBYTES
pc_bytes_sigbits_flip_endian(const PCBYTES pcb)
{
    int n;
    uint8_t tmp1, tmp2;
    size_t size = pc_interpretation_size(pcb.interpretation);
    uint8_t *b1 = pcb.bytes;
    uint8_t *b2 = pcb.bytes + size;

    /* If it's not multi-byte words, it doesn't need flipping */
    if ( size < 2 )
        return pcb;

    /* We only need to flip the first two words, */
    /* which are the common bit count and common bits word */
    for ( n = 0; n < size / 2; n++ )
    {
        /* Flip bit count */
        tmp1 = b1[n];
        b1[n] = b1[size-n-1];
        b1[size-n-1] = tmp1;
        /* Flip common bits */
        tmp2 = b2[n];
        b2[n] = b2[size-n-1];
        b2[size-n-1] = tmp2;
    }

    return pcb;
}


PCBYTES
pc_bytes_sigbits_decode_8(const PCBYTES pcb)
{
    int i;
    const uint8_t *bytes_ptr = (const uint8_t*)(pcb.bytes);
    uint8_t nbits;
    uint8_t commonvalue;
    uint8_t mask;
    int bit = 8;
    size_t outbytes_size = sizeof(uint8_t) * pcb.npoints;
    uint8_t *outbytes = pcalloc(outbytes_size);
    uint8_t *obytes = (uint8_t*)outbytes;
    PCBYTES pcbout = pcb;

    /* How many unique bits? */
    nbits = *bytes_ptr; bytes_ptr++;
    /* What is the shared bit value? */
    commonvalue = *bytes_ptr; bytes_ptr++;
    /* Mask for just the unique parts */
    mask = (0xFF >> (bit-nbits));

    for ( i = 0; i < pcb.npoints; i++ )
    {
        int shift = bit - nbits;
        uint8_t val = *bytes_ptr;
        /* The unique part is all in this word */
        if ( shift >= 0 )
        {
            /* Push unique part to bottom of word */
            val >>= shift;
            /* Mask out any excess */
            val &= mask;
            /* Add in the common part */
            val |= commonvalue;
            /* Save */
            obytes[i] = val;
            /* Move read head */
            bit -= nbits;
        }
        /* The unique part is split over this word and the next */
        else
        {
            int s = abs(shift);
            val <<= s;
            val &= mask;
            val |= commonvalue;
            obytes[i] = val;
            bytes_ptr++;
            bit = 8;
            val = *bytes_ptr;
            shift = bit - s;
            val >>= shift;
            val &= mask;
            obytes[i] |= val;
            bit -= s;
        }
    }
    pcbout.size = outbytes_size;
    pcbout.compression = PC_DIM_SIGBITS;
    pcbout.bytes = outbytes;
    return pcbout;
}

PCBYTES
pc_bytes_sigbits_decode_16(const PCBYTES pcb)
{
    int i;
    const uint16_t *bytes_ptr = (const uint16_t *)(pcb.bytes);
    uint16_t nbits;
    uint16_t commonvalue;
    uint16_t mask;
    int bit = 16;
    size_t outbytes_size = sizeof(uint16_t) * pcb.npoints;
    uint8_t *outbytes = pcalloc(outbytes_size);
    uint16_t *obytes = (uint16_t*)outbytes;
    PCBYTES pcbout = pcb;

    /* How many unique bits? */
    nbits = *bytes_ptr; bytes_ptr++;
    /* What is the shared bit value? */
    commonvalue = *bytes_ptr; bytes_ptr++;
    /* Calculate mask */
    mask = (0xFFFF >> (bit-nbits));

    for ( i = 0; i < pcb.npoints; i++ )
    {
        int shift = bit - nbits;
        uint16_t val = *bytes_ptr;
        if ( shift >= 0 )
        {
            val >>= shift;
            val &= mask;
            val |= commonvalue;
            obytes[i] = val;
            bit -= nbits;
        }
        else
        {
            int s = abs(shift);
            val <<= s;
            val &= mask;
            val |= commonvalue;
            obytes[i] = val;
            bytes_ptr++;
            bit = 16;
            val = *bytes_ptr;
            shift = bit - s;
            val >>= shift;
            val &= mask;
            obytes[i] |= val;
            bit -= s;
        }
    }
    pcbout.size = outbytes_size;
    pcbout.compression = PC_DIM_SIGBITS;
    pcbout.bytes = outbytes;
    return pcbout;
}

PCBYTES
pc_bytes_sigbits_decode_32(const PCBYTES pcb)
{
    int i;
    const uint32_t *bytes_ptr = (const uint32_t *)(pcb.bytes);
    uint32_t nbits;
    uint32_t commonvalue;
    uint32_t mask;
    int bit = 32;
    size_t outbytes_size = sizeof(uint32_t) * pcb.npoints;
    uint8_t *outbytes = pcalloc(outbytes_size);
    uint32_t *obytes = (uint32_t*)outbytes;
    PCBYTES pcbout = pcb;

    /* How many unique bits? */
    nbits = *bytes_ptr; bytes_ptr++;
    /* What is the shared bit value? */
    commonvalue = *bytes_ptr; bytes_ptr++;
    /* Calculate mask */
    mask = (0xFFFFFFFF >> (bit-nbits));

    for ( i = 0; i < pcb.npoints; i++ )
    {
        int shift = bit - nbits;
        uint32_t val = *bytes_ptr;
        if ( shift >= 0 )
        {
            val >>= shift;
            val &= mask;
            val |= commonvalue;
            obytes[i] = val;
            bit -= nbits;
        }
        else
        {
            int s = abs(shift);
            val <<= s;
            val &= mask;
            val |= commonvalue;
            obytes[i] = val;
            bytes_ptr++;
            bit = 32;
            val = *bytes_ptr;
            shift = bit - s;
            val >>= shift;
            val &= mask;
            bit -= s;
            obytes[i] |= val;
        }
    }

    pcbout.size = outbytes_size;
    pcbout.compression = PC_DIM_SIGBITS;
    pcbout.bytes = outbytes;
    return pcbout;
}


PCBYTES
pc_bytes_sigbits_decode(const PCBYTES pcb)
{
    size_t size = pc_interpretation_size(pcb.interpretation);
    switch ( size )
    {
        case 1:
        {
            return pc_bytes_sigbits_decode_8(pcb);
        }
        case 2:
        {
            return pc_bytes_sigbits_decode_16(pcb);
        }
        case 4:
        {
            return pc_bytes_sigbits_decode_32(pcb);
        }
        default:
        {
            pcerror("pc_bytes_sigbits_decode cannot handle interpretation %d", pcb.interpretation);
        }
    }
    pcerror("pc_bytes_sigbits_decode got an unhandled errror");
    return pcb;
}

static voidpf
pc_zlib_alloc(voidpf opaque, uInt nitems, uInt sz)
{
    return pcalloc(sz*nitems);
}

static void
pc_zlib_free(voidpf opaque, voidpf ptr)
{
    pcfree(ptr);
}


/* TO DO look for Z_STREAM_END on the write */

/**
* Returns compressed byte array with
* <size_t> size of compressed portion
* <size_t> size of original data
* <.....> compresssed bytes
*/
PCBYTES
pc_bytes_zlib_encode(const PCBYTES pcb)
{
    z_stream strm;
    int ret;
    size_t have;
    size_t bufsize = 4*pcb.size;
    uint8_t *buf = pcalloc(bufsize);
    PCBYTES pcbout = pcb;

    /* Use our own allocators */
    strm.zalloc = pc_zlib_alloc;
    strm.zfree = pc_zlib_free;
    strm.opaque = Z_NULL;
    ret = deflateInit(&strm, 9);
    /* Set up input buffer */
    strm.avail_in = pcb.size;
    strm.next_in = pcb.bytes;
    /* Set up output buffer */
    strm.avail_out = bufsize;
    strm.next_out = buf;
    /* Compress */
    ret = deflate(&strm, Z_FINISH);
    assert(ret != Z_STREAM_ERROR);
    have = strm.total_out;
    pcbout.size = have;
    pcbout.bytes = pcalloc(pcbout.size);
    pcbout.compression = PC_DIM_ZLIB;
    memcpy(pcbout.bytes, buf, have);
    pcfree(buf);
    deflateEnd(&strm);
    return pcbout;
}

/**
* Returns uncompressed byte array from input with
* <size_t> size of compressed portion
* <size_t> size of original data
* <.....> compresssed bytes
*/
PCBYTES
pc_bytes_zlib_decode(const PCBYTES pcb)
{
    z_stream strm;
    int ret;
    PCBYTES pcbout = pcb;

    pcbout.size = pc_interpretation_size(pcb.interpretation) * pcb.npoints;

    /* Set up output memory */
    pcbout.bytes = pcalloc(pcbout.size);

    /* Use our own allocators */
    strm.zalloc = pc_zlib_alloc;
    strm.zfree = pc_zlib_free;
    strm.opaque = Z_NULL;
    ret = inflateInit(&strm);
    /* Set up input buffer */
    strm.avail_in = pcb.size;
    strm.next_in = pcb.bytes;

    strm.avail_out = pcbout.size;
    strm.next_out = pcbout.bytes;
    ret = inflate(&strm, Z_FINISH);
    assert(ret != Z_STREAM_ERROR);
    inflateEnd(&strm);

    pcbout.compression = PC_DIM_NONE;
    return pcbout;
}

/**
* This flips bytes in-place, so won't work on readonly bytes
*/
PCBYTES
pc_bytes_flip_endian(PCBYTES pcb)
{
    if ( pcb.readonly )
        pcerror("pc_bytes_flip_endian: cannot flip readonly bytes");

    switch(pcb.compression)
    {
        case PC_DIM_NONE:
            return pcb;
        case PC_DIM_SIGBITS:
            return pc_bytes_sigbits_flip_endian(pcb);
        case PC_DIM_ZLIB:
            return pcb;
        case PC_DIM_RLE:
            return pc_bytes_run_length_flip_endian(pcb);
        default:
            pcerror("pc_bytes_flip_endian: unknown compression");
    }

    return pcb;
}

size_t
pc_bytes_serialized_size(const PCBYTES *pcb)
{
    /* compression type (1) + size of data (4) + data */
    return 1 + 4 + pcb->size;
}

int
pc_bytes_serialize(const PCBYTES *pcb, uint8_t *buf, size_t *size)
{
    static int compression_num_size = 1;
    static int size_num_size = 4;
    int32_t pcbsize = pcb->size;

    /* Compression type number */
    *buf = pcb->compression;
    buf += compression_num_size;
    /* Buffer size */
    memcpy(buf, &pcbsize, size_num_size);
    buf += size_num_size;
    /* Buffer contents */
    memcpy(buf, pcb->bytes, pcb->size);
    /* Return total size */
    *size = compression_num_size + size_num_size + pcbsize;
    return PC_SUCCESS;
}

int
pc_bytes_deserialize(const uint8_t *buf, const PCDIMENSION *dim, PCBYTES *pcb, int readonly, int flip_endian)
{
    pcb->compression = buf[0];
    pcb->size = wkb_get_int32(buf+1, flip_endian);
    pcb->readonly = readonly;
    if ( readonly && flip_endian )
        pcerror("pc_bytes_deserialize: cannot create a read-only buffer on byteswapped input");
    if ( readonly )
    {
        pcb->bytes = (uint8_t*)(buf+5);
    }
    else
    {
        pcb->bytes = pcalloc(pcb->size);
        memcpy(pcb->bytes, buf+5, pcb->size);
        if ( flip_endian )
        {
            *pcb = pc_bytes_flip_endian(*pcb);
        }
    }
    pcb->interpretation = dim->interpretation;
    /* WARNING, still need to set externally */
    /*   pcb.npoints */
    return PC_SUCCESS;
}


static int
pc_bytes_uncompressed_minmax(const PCBYTES *pcb, double *min, double *max)
{
    int i;
    int element_size = pc_interpretation_size(pcb->interpretation);
    double d;
    double mn = MAXFLOAT;
    double mx = -1*MAXFLOAT;
    for ( i = 0; i < pcb->npoints; i++ )
    {
        d = pc_double_from_ptr(pcb->bytes + i*element_size, pcb->interpretation);
        if ( d < mn )
            mn = d;
        if ( d > mx )
            mx = d;
    }
    *min = mn;
    *max = mx;
}

static int
pc_bytes_run_length_minmax(const PCBYTES *pcb, double *min, double *max)
{
    int element_size = pc_interpretation_size(pcb->interpretation);
    double mn = MAXFLOAT;
    double mx = -1*MAXFLOAT;
    double d;
    uint8_t *ptr = pcb->bytes;
    uint8_t *ptr_end = pcb->bytes + pcb->size;

    /* Move past first count */
    ptr++;

    while( ptr < ptr_end )
    {
        d = pc_double_from_ptr(ptr, pcb->interpretation);
        /* Calc min/max */
        if ( d < mn )
            mn = d;
        if ( d > mx )
            mx = d;

        ptr += element_size;
    }

    *min = mn;
    *max = mx;
}


static int
pc_bytes_zlib_minmax(const PCBYTES *pcb, double *min, double *max)
{
    PCBYTES zcb = pc_bytes_zlib_decode(*pcb);
    int rv = pc_bytes_uncompressed_minmax(&zcb, min, max);
    pc_bytes_free(zcb);
    return rv;
}

static int
pc_bytes_sigbits_minmax(const PCBYTES *pcb, double *min, double *max)
{
    PCBYTES zcb = pc_bytes_sigbits_decode(*pcb);
    int rv = pc_bytes_uncompressed_minmax(&zcb, min, max);
    pc_bytes_free(zcb);
    return rv;
}

int pc_bytes_minmax(const PCBYTES *pcb, double *min, double *max)
{
    switch(pcb->compression)
    {
        case PC_DIM_NONE:
            return pc_bytes_uncompressed_minmax(pcb, min, max);
        case PC_DIM_SIGBITS:
            return pc_bytes_sigbits_minmax(pcb, min, max);
        case PC_DIM_ZLIB:
            return pc_bytes_zlib_minmax(pcb, min, max);
        case PC_DIM_RLE:
            return pc_bytes_run_length_minmax(pcb, min, max);
        default:
            pcerror("pc_bytes_minmax: unknown compression");
    }
    return PC_FAILURE;
}
