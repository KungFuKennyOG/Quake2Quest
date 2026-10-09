/*
 * SoF BSP v46 -> Quake 2 BSP v38 in-memory converter.
 *
 * Written from the layout in the Raven SoF SDK (qcommon/qfiles.h): 22 lumps,
 * 44 byte faces (extra region / lightmip fields), 32 byte leafs (extra region
 * field). All other lumps share the Q2 layout, so the converter rewrites the
 * header, converts faces and leafs, and copies the rest. The result is a plain
 * v38 file that the stock Yamagi loaders (renderer + collision) read unchanged.
 *
 * Dropped on purpose (not needed to draw / collide yet): region, regionface and
 * light lumps (19..21), per-face region data and the lightmip bytes.
 *
 * Assumes little-endian host (ARM64 / x86).
 */

#ifndef BSP_SOF_H
#define BSP_SOF_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define BSP_SOF_VERSION      46
#define BSP_SOF_LUMPS        22
#define BSP_Q2_LUMPS         19
#define BSP_SOF_FACE_SIZE    44
#define BSP_SOF_LEAF_SIZE    32
#define BSP_Q2_FACE_SIZE     20
#define BSP_Q2_LEAF_SIZE     28

/* lump indices (same as Q2 for 0..18) */
#define BSP_L_FACES          6
#define BSP_L_LEAFS          8

/*
 * Returns a malloc'd v38 buffer (caller frees) and its length in *outlen,
 * or NULL if the input is not a sane v46 BSP (*err describes why).
 */
static inline unsigned char *
BSP_ConvertSoF(const unsigned char *in, int inlen, int *outlen, const char **err)
{
	int32_t lofs[BSP_SOF_LUMPS], llen[BSP_SOF_LUMPS];
	int32_t nofs[BSP_Q2_LUMPS], nlen[BSP_Q2_LUMPS];
	int32_t ident, version, i, total, pos;
	unsigned char *out;
	const char *dummy;

	if (!err) err = &dummy;
	*err = NULL;

	if (inlen < 8 + BSP_SOF_LUMPS * 8)
	{
		*err = "file too small for v46 header";
		return NULL;
	}

	memcpy(&ident, in, 4);
	memcpy(&version, in + 4, 4);

	if (ident != (('P' << 24) + ('S' << 16) + ('B' << 8) + 'I') || version != BSP_SOF_VERSION)
	{
		*err = "not an IBSP v46 file";
		return NULL;
	}

	for (i = 0; i < BSP_SOF_LUMPS; i++)
	{
		memcpy(&lofs[i], in + 8 + i * 8, 4);
		memcpy(&llen[i], in + 8 + i * 8 + 4, 4);

		if (lofs[i] < 0 || llen[i] < 0 || (int64_t)lofs[i] + llen[i] > inlen)
		{
			*err = "lump out of file bounds";
			return NULL;
		}
	}

	if (llen[BSP_L_FACES] % BSP_SOF_FACE_SIZE || llen[BSP_L_LEAFS] % BSP_SOF_LEAF_SIZE)
	{
		*err = "face/leaf lump size is not a multiple of the v46 struct size";
		return NULL;
	}

	/* new lump lengths */
	for (i = 0; i < BSP_Q2_LUMPS; i++)
	{
		nlen[i] = llen[i];
	}
	nlen[BSP_L_FACES] = (llen[BSP_L_FACES] / BSP_SOF_FACE_SIZE) * BSP_Q2_FACE_SIZE;
	nlen[BSP_L_LEAFS] = (llen[BSP_L_LEAFS] / BSP_SOF_LEAF_SIZE) * BSP_Q2_LEAF_SIZE;

	total = 8 + BSP_Q2_LUMPS * 8;
	for (i = 0; i < BSP_Q2_LUMPS; i++)
	{
		total = (total + 3) & ~3;
		nofs[i] = total;
		total += nlen[i];
	}
	total = (total + 3) & ~3;

	out = calloc(1, (size_t)total + 4);
	if (!out)
	{
		*err = "out of memory";
		return NULL;
	}

	ident = (('P' << 24) + ('S' << 16) + ('B' << 8) + 'I');
	version = 38;
	memcpy(out, &ident, 4);
	memcpy(out + 4, &version, 4);

	for (i = 0; i < BSP_Q2_LUMPS; i++)
	{
		memcpy(out + 8 + i * 8, &nofs[i], 4);
		memcpy(out + 8 + i * 8 + 4, &nlen[i], 4);

		if (i != BSP_L_FACES && i != BSP_L_LEAFS)
		{
			memcpy(out + nofs[i], in + lofs[i], (size_t)llen[i]);
		}
	}

	/* faces: v46 (44) -> v38 (20) */
	{
		int n = llen[BSP_L_FACES] / BSP_SOF_FACE_SIZE;
		const unsigned char *s = in + lofs[BSP_L_FACES];
		unsigned char *d = out + nofs[BSP_L_FACES];

		for (i = 0; i < n; i++, s += BSP_SOF_FACE_SIZE, d += BSP_Q2_FACE_SIZE)
		{
			memcpy(d + 0, s + 0, 2);   /* planenum */
			memcpy(d + 2, s + 2, 2);   /* side */
			memcpy(d + 4, s + 4, 4);   /* firstedge */
			memcpy(d + 8, s + 8, 2);   /* numedges */
			memcpy(d + 10, s + 10, 2); /* texinfo */
			memcpy(d + 12, s + 22, 4); /* styles[4] */
			memcpy(d + 16, s + 28, 4); /* lightofs */
		}
	}

	/* leafs: v46 (32) -> v38 (28) */
	{
		int n = llen[BSP_L_LEAFS] / BSP_SOF_LEAF_SIZE;
		const unsigned char *s = in + lofs[BSP_L_LEAFS];
		unsigned char *d = out + nofs[BSP_L_LEAFS];

		for (i = 0; i < n; i++, s += BSP_SOF_LEAF_SIZE, d += BSP_Q2_LEAF_SIZE)
		{
			memcpy(d + 0, s + 0, 4);   /* contents */
			memcpy(d + 4, s + 4, 2);   /* cluster */
			memcpy(d + 6, s + 6, 2);   /* area */
			/* s + 8: region (dropped) */
			memcpy(d + 8, s + 10, 12); /* mins[3], maxs[3] */
			memcpy(d + 20, s + 22, 8); /* firstleafface, numleaffaces, firstleafbrush, numleafbrushes */
		}
	}

	(void)pos;
	*outlen = total;
	return out;
}

#endif /* BSP_SOF_H */
