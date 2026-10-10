/*
 * SoF BSP v46 -> Quake 2 BSP v38 in-memory converter.
 *
 * Written from the layout in the Raven SoF SDK (qcommon/qfiles.h): 22 lumps,
 * 44 byte faces (extra region / lightmip fields), 32 byte leafs (extra region
 * field). All other lumps share the Q2 layout, so the converter rewrites the
 * header, converts faces and leafs, and copies the rest. The result is a plain
 * v38 file that the stock Yamagi loaders (renderer + collision) read unchanged.
 *
 * Dropped on purpose: region, regionface and light lumps (19..21) and per-face
 * region data. Lightmaps are upsampled using lightmip (smax,tmax) to full size.
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
#define BSP_L_LIGHTING       7
#define BSP_L_TEXINFO        5
#define BSP_TEXINFO_SIZE     76

/* number of used light styles on a v46 face (styles[4] at +22, 255 = unused) */
static inline int BSP_SoF_FaceStyles(const unsigned char *f)
{
	int n = 0;
	while (n < 4 && f[22 + n] != 255) n++;
	return n;
}

/* full lightmap size in samples from the face extents (as the Q2 renderer computes it) */
static inline void BSP_SoF_FaceLMSize(const unsigned char *f, int *w, int *h)
{
	int16_t e0, e1;
	memcpy(&e0, f + 40, 2);
	memcpy(&e1, f + 42, 2);
	*w = (e0 >> 4) + 1;
	*h = (e1 >> 4) + 1;
}

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
	/* lighting: SoF stores smax*tmax samples per style (lightmip) and the grid is
	 * subsampled by X,Y relative to the face extents. Rebuild a Q2-style lump with
	 * full w*h samples per style, upsampled. */
	{
		int n = llen[BSP_L_FACES] / BSP_SOF_FACE_SIZE;
		const unsigned char *f = in + lofs[BSP_L_FACES];
		int64_t sum = 0;
		for (i = 0; i < n; i++, f += BSP_SOF_FACE_SIZE)
		{
			int32_t lo; int w, h;
			memcpy(&lo, f + 28, 4);
			BSP_SoF_FaceLMSize(f, &w, &h);
			if (lo >= 0 && f[32] && f[33] && w > 0 && h > 0 && w <= 256 && h <= 256)
				sum += (int64_t)w * h * 3 * BSP_SoF_FaceStyles(f);
		}
		if (sum > 0x3fffffff) { *err = "lighting too large"; return NULL; }
		nlen[BSP_L_LIGHTING] = (int32_t)sum;
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

		if (i != BSP_L_FACES && i != BSP_L_LEAFS && i != BSP_L_LIGHTING)
		{
			memcpy(out + nofs[i], in + lofs[i], (size_t)llen[i]);
		}
	}

	/* texinfo: same layout, but SoF surface flags differ from Q2. Keep LIGHT, SLICK,
	 * SKY, WARP, FLOWING, NODRAW, the SoF alpha-texture bit (0x800) and the material
	 * type in the top byte (the game reads flags >> 24 for footsteps, bullet impacts and
	 * damage); drop the deprecated TRANS33/66 bits and other SoF-only bits. */
	{
		int n = llen[BSP_L_TEXINFO] / BSP_TEXINFO_SIZE;
		unsigned char *t = out + nofs[BSP_L_TEXINFO];
		for (i = 0; i < n; i++, t += BSP_TEXINFO_SIZE)
		{
			int32_t f;
			memcpy(&f, t + 32, 4);
			f &= (int32_t)(0x01 | 0x02 | 0x04 | 0x08 | 0x40 | 0x80 | 0x800 | 0xff000000u);
			memcpy(t + 32, &f, 4);
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
		}
	}

	/* second pass: rewrite lightofs and fill the new lighting lump */
	{
		const unsigned char *lsrc = in + lofs[BSP_L_LIGHTING];
		unsigned char *ldst = out + nofs[BSP_L_LIGHTING];
		int32_t cur = 0;
		int n = llen[BSP_L_FACES] / BSP_SOF_FACE_SIZE;
		const unsigned char *s = in + lofs[BSP_L_FACES];
		unsigned char *d = out + nofs[BSP_L_FACES];

		for (i = 0; i < n; i++, s += BSP_SOF_FACE_SIZE, d += BSP_Q2_FACE_SIZE)
		{
			int32_t lo, none = -1;
			int w, h, sm = s[32], tm = s[33], ns = BSP_SoF_FaceStyles(s), st, x, y;

			memcpy(&lo, s + 28, 4);
			BSP_SoF_FaceLMSize(s, &w, &h);

			if (lo < 0 || !sm || !tm || !ns || w <= 0 || h <= 0 || w > 256 || h > 256 ||
			    (int64_t)lo + (int64_t)sm * tm * 3 * ns > llen[BSP_L_LIGHTING])
			{
				memcpy(d + 16, &none, 4);
				continue;
			}

			memcpy(d + 16, &cur, 4);
			for (st = 0; st < ns; st++)
			{
				const unsigned char *src = lsrc + lo + st * sm * tm * 3;
				for (y = 0; y < h; y++)
				{
					float fy = (y + 0.5f) * tm / h - 0.5f;
					int y0 = (int)(fy < 0 ? 0 : fy), y1 = y0 + 1 < tm ? y0 + 1 : y0;
					float ty = fy < 0 ? 0 : fy - y0;
					for (x = 0; x < w; x++)
					{
						float fx = (x + 0.5f) * sm / w - 0.5f;
						int x0 = (int)(fx < 0 ? 0 : fx), x1 = x0 + 1 < sm ? x0 + 1 : x0;
						float tx = fx < 0 ? 0 : fx - x0;
						int c;
						for (c = 0; c < 3; c++)
						{
							float a0 = src[(y0 * sm + x0) * 3 + c] * (1 - tx) + src[(y0 * sm + x1) * 3 + c] * tx;
							float b0 = src[(y1 * sm + x0) * 3 + c] * (1 - tx) + src[(y1 * sm + x1) * 3 + c] * tx;
							ldst[cur++] = (unsigned char)(a0 * (1 - ty) + b0 * ty + 0.5f);
						}
					}
				}
			}
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
