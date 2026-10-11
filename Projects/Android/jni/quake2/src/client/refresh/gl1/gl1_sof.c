/*
 * gl1_sof.c - draws Soldier of Fortune GHOUL meshes (entity_t.sofdraw).
 *
 * The meshes arrive already animated and posed in the entity's model space; this file
 * places them like an alias model, lights them with the light at the entity origin and
 * binds the skin textures.
 */
#include "header/local.h"
#include "../../../sof/sof_client.h"
#include "../../../sof/sof_hudfont.h"

#define SOF_CLAMP1(x) ((x) > 1.0f ? 1.0f : (x))

image_t *r_sofhudtexture;

/* the VR HUD's dot-matrix font (sof_hudfont.h): one 32 x 32 cell per character, dots of
   3 x 3 texels on a 4 texel pitch with a soft edge, and a solid white cell */
void
R_InitSoFHudTexture(void)
{
	static byte data[SOFHUD_TEX_H][SOFHUD_TEX_W][4];
	int c, row, col, x, y;

	memset(data, 0, sizeof(data));
	for (y = 0; y < SOFHUD_TEX_H; y++)
	{
		for (x = 0; x < SOFHUD_TEX_W; x++)
		{
			data[y][x][0] = data[y][x][1] = data[y][x][2] = 255;
		}
	}
	for (c = 0; c < SOFHUD_COUNT; c++)
	{
		int cx = (c % 16) * SOFHUD_CELL + SOFHUD_GLYPH_X, cy = (c / 16) * SOFHUD_CELL + SOFHUD_GLYPH_Y;
		for (row = 0; row < 7; row++)
		{
			for (col = 0; col < 5; col++)
			{
				if (!(sofhud_font[c][row] & (0x10 >> col)))
				{
					continue;
				}
				for (y = 0; y < 4; y++)
				{
					for (x = 0; x < 4; x++)
					{
						/* lit 3 x 3, the fourth row/column a faint gap */
						data[cy + row * 4 + y][cx + col * 4 + x][3] = (x < 3 && y < 3) ? 255 : 60;
					}
				}
			}
		}
	}
	for (y = 0; y < SOFHUD_CELL; y++)
	{
		for (x = 0; x < SOFHUD_CELL; x++)
		{
			data[(SOFHUD_WHITE / 16) * SOFHUD_CELL + y][(SOFHUD_WHITE % 16) * SOFHUD_CELL + x][3] = 255;
		}
	}
	r_sofhudtexture = R_LoadPic((char *)SOFHUD_TEXTURE, (byte *)data, SOFHUD_TEX_W, 0, SOFHUD_TEX_H, 0, it_sprite, 32);
}

static image_t *
SoF_SkinImage(const char *name)
{
	image_t *img;

	if (!name || !name[0])
	{
		return r_notexture;
	}

	if (!strcmp(name, SOFHUD_TEXTURE))
	{
		return r_sofhudtexture ? r_sofhudtexture : r_notexture;
	}

	img = R_FindImage((char *)name, it_skin);
	return img ? img : r_notexture;
}

void
R_DrawSoFEntity(entity_t *e)
{
	const sofdraw_t *d = e->sofdraw;
	vec3_t shadelight;
	int i, m;
	static float colors[4 * 4096];

	if (!d || d->nummeshes <= 0)
	{
		return;
	}

	/* lighting: map light at the origin, with the usual minimum for view models */
	if (e->flags & RF_FULLBRIGHT)
	{
		VectorSet(shadelight, 1, 1, 1);
	}
	else
	{
		R_LightPoint(e->origin, shadelight);

		if (e->flags & RF_MINLIGHT)
		{
			for (i = 0; i < 3; i++)
			{
				if (shadelight[i] > 0.1f)
				{
					break;
				}
			}

			if (i == 3)
			{
				VectorSet(shadelight, 0.1f, 0.1f, 0.1f);
			}
		}
	}

	if (e->flags & RF_DEPTHHACK)
	{
		glDepthRangef(gldepthmin, gldepthmin + 0.3f * (gldepthmax - gldepthmin));
	}

	glPushMatrix();
	/* GHOUL entity space matches the game's EntToWorldMatrix, i.e. the alias model convention */
	e->angles[PITCH] = -e->angles[PITCH];
	R_RotateForEntity(e);
	e->angles[PITCH] = -e->angles[PITCH];

	/* SoF draws GHOUL models with face culling off: a few triangles in the models are
	   wound the other way (they were holes when culled). Only the view weapon is culled
	   (GHOUL triangles are counter-clockwise seen from outside, the opposite of Quake 2's
	   models, so it culls GL_BACK; a mirrored left-handed weapon flips that back). */
	glCullFace(GL_BACK);
	if (!(e->flags & RF_WEAPONMODEL))
	{
		glDisable(GL_CULL_FACE);
	}

	if (e->flags & RF_WEAPONMODEL)
	{
		float s = vr_weaponscale ? vr_weaponscale->value : 1.0f;

		if (gl_lefthand && gl_lefthand->value == 1.0F)
		{
			glScalef(s, -s, s);
			glCullFace(GL_FRONT);
		}
		else
		{
			glScalef(s, s, s);
		}
	}

	glShadeModel(GL_SMOOTH);
	R_TexEnv(GL_MODULATE);

	if (e->flags & RF_TRANSLUCENT)
	{
		glEnable(GL_BLEND);
	}

	/* GHOUL skins use alpha for cut-outs (hair, straps, ...) */
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.5f);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	for (m = 0; m < d->nummeshes; m++)
	{
		const sofmesh_t *mesh = &d->meshes[m];
		int n = mesh->numverts;
		float alpha = (e->flags & RF_TRANSLUCENT) ? e->alpha : 1.0f;

		if (mesh->colors)
		{
			/* effects (sprites, lines): unlit vertex colours, blended, never written to depth */
			if (n <= 0 || mesh->numindices <= 0)
			{
				continue;
			}

			R_Bind(SoF_SkinImage(mesh->skin)->texnum);
			glDisable(GL_ALPHA_TEST);
			glDisable(GL_CULL_FACE);
			glEnable(GL_BLEND);
			glDepthMask(GL_FALSE);

			if (mesh->blend == SOFBLEND_ADD)
			{
				glBlendFunc(GL_ONE, GL_ONE);
			}
			else if (mesh->blend == SOFBLEND_SUBTRACT)
			{
				glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
			}
			else
			{
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}

			if (mesh->nodepth)
			{
				glDisable(GL_DEPTH_TEST);
			}

			glVertexPointer(3, GL_FLOAT, 0, mesh->xyz);
			glTexCoordPointer(2, GL_FLOAT, 0, mesh->st);
			glColorPointer(4, GL_UNSIGNED_BYTE, 0, mesh->colors);
			glDrawElements(GL_TRIANGLES, mesh->numindices, GL_UNSIGNED_SHORT, mesh->indices);

			if (mesh->nodepth)
			{
				glEnable(GL_DEPTH_TEST);
			}

			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glEnable(GL_ALPHA_TEST);
			continue;
		}

		if (n <= 0 || mesh->numindices <= 0 || n > 4096)
		{
			continue;
		}

		R_Bind(SoF_SkinImage(mesh->skin)->texnum);

		/* simple directional term from the normal (light from above) on top of the map light */
		for (i = 0; i < n; i++)
		{
			const float *nr = mesh->normal ? mesh->normal + i * 3 : NULL;
			float l = nr ? 0.75f + 0.25f * nr[2] : 1.0f;

			/* clamped: GLES / gl4es does not clamp vertex colours, so strong map light
			   would otherwise blow the skins out to white */
			colors[i * 4 + 0] = SOF_CLAMP1(shadelight[0] * l) * mesh->rgba[0];
			colors[i * 4 + 1] = SOF_CLAMP1(shadelight[1] * l) * mesh->rgba[1];
			colors[i * 4 + 2] = SOF_CLAMP1(shadelight[2] * l) * mesh->rgba[2];
			colors[i * 4 + 3] = alpha * mesh->rgba[3];
		}

		glVertexPointer(3, GL_FLOAT, 0, mesh->xyz);
		glTexCoordPointer(2, GL_FLOAT, 0, mesh->st);
		glColorPointer(4, GL_FLOAT, 0, colors);
		glDrawElements(GL_TRIANGLES, mesh->numindices, GL_UNSIGNED_SHORT, mesh->indices);
		c_alias_polys += mesh->numindices / 3;
	}

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisable(GL_ALPHA_TEST);

	if (e->flags & RF_TRANSLUCENT)
	{
		glDisable(GL_BLEND);
	}

	R_TexEnv(GL_REPLACE);
	glShadeModel(GL_FLAT);
	glColor4f(1, 1, 1, 1);
	glPopMatrix();

	glCullFace(GL_FRONT);
	if (gl_cull->value)
	{
		glEnable(GL_CULL_FACE);
	}

	if (e->flags & RF_DEPTHHACK)
	{
		glDepthRangef(gldepthmin, gldepthmax);
	}
}
