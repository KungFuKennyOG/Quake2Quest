/*
 * gl1_sof.c - draws Soldier of Fortune GHOUL meshes (entity_t.sofdraw).
 *
 * The meshes arrive already animated and posed in the entity's model space; this file
 * places them like an alias model, lights them with the light at the entity origin and
 * binds the skin textures.
 */
#include "header/local.h"
#include "../../../sof/sof_client.h"

static image_t *
SoF_SkinImage(const char *name)
{
	image_t *img;

	if (!name || !name[0])
	{
		return r_notexture;
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

	if (e->flags & RF_WEAPONMODEL)
	{
		float s = vr_weaponscale ? vr_weaponscale->value : 1.0f;

		if (gl_lefthand && gl_lefthand->value == 1.0F)
		{
			glScalef(s, -s, s);
			glCullFace(GL_BACK);
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

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	for (m = 0; m < d->nummeshes; m++)
	{
		const sofmesh_t *mesh = &d->meshes[m];
		int n = mesh->numverts;
		float alpha = (e->flags & RF_TRANSLUCENT) ? e->alpha : 1.0f;

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

			colors[i * 4 + 0] = shadelight[0] * l * mesh->rgba[0];
			colors[i * 4 + 1] = shadelight[1] * l * mesh->rgba[1];
			colors[i * 4 + 2] = shadelight[2] * l * mesh->rgba[2];
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

	if (e->flags & RF_TRANSLUCENT)
	{
		glDisable(GL_BLEND);
	}

	R_TexEnv(GL_REPLACE);
	glShadeModel(GL_FLAT);
	glColor4f(1, 1, 1, 1);
	glPopMatrix();

	if ((e->flags & RF_WEAPONMODEL) && gl_lefthand && gl_lefthand->value == 1.0F)
	{
		glCullFace(GL_FRONT);
	}

	if (e->flags & RF_DEPTHHACK)
	{
		glDepthRangef(gldepthmin, gldepthmax);
	}
}
