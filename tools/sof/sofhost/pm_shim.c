/* pm_shim.c - runs Yamagi's Pmove() (Q2 types) on behalf of SoF-typed code. */
#include "common/header/common.h"
#include "pm_shim.h"

static pms_t *cur;

static trace_t pm_trace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end)
{
	static csurface_t surf;
	pms_trace_t t;
	trace_t r;
	memset(&t, 0, sizeof(t));
	cur->trace(start, mins, maxs, end, &t);
	memset(&r, 0, sizeof(r));
	r.allsolid = t.allsolid;
	r.startsolid = t.startsolid;
	r.fraction = t.fraction;
	VectorCopy(t.endpos, r.endpos);
	VectorCopy(t.normal, r.plane.normal);
	r.plane.dist = t.dist;
	r.plane.type = (byte)t.planetype;
	r.plane.signbits = (byte)t.signbits;
	surf.flags = t.surfflags;
	r.surface = &surf;
	r.contents = t.contents;
	r.ent = (struct edict_s *)t.ent;
	return r;
}

static int pm_contents(vec3_t p) { return cur->pointcontents(p); }

void pms_run(pms_t *p)
{
	pmove_t pm;
	int i;
	memset(&pm, 0, sizeof(pm));
	cur = p;
	pm.s.pm_type = (pmtype_t)p->pm_type;
	for (i = 0; i < 3; i++)
	{
		pm.s.origin[i] = p->origin[i];
		pm.s.velocity[i] = p->velocity[i];
		pm.s.delta_angles[i] = p->delta_angles[i];
		pm.cmd.angles[i] = p->angles[i];
	}
	pm.s.pm_flags = (byte)p->pm_flags;
	pm.s.pm_time = (byte)p->pm_time;
	pm.s.gravity = p->gravity;
	pm.cmd.msec = (byte)p->msec;
	pm.cmd.buttons = (byte)p->buttons;
	pm.cmd.forwardmove = p->forwardmove;
	pm.cmd.sidemove = p->sidemove;
	pm.cmd.upmove = p->upmove;
	pm.snapinitial = p->snapinitial;
	pm.trace = pm_trace;
	pm.pointcontents = pm_contents;

	Pmove(&pm);

	for (i = 0; i < 3; i++)
	{
		p->origin[i] = pm.s.origin[i];
		p->velocity[i] = pm.s.velocity[i];
		p->delta_angles[i] = pm.s.delta_angles[i];
		p->viewangles[i] = pm.viewangles[i];
		p->mins[i] = pm.mins[i];
		p->maxs[i] = pm.maxs[i];
	}
	p->pm_flags = pm.s.pm_flags;
	p->pm_time = pm.s.pm_time;
	p->numtouch = pm.numtouch;
	for (i = 0; i < pm.numtouch && i < PMS_MAXTOUCH; i++) p->touchents[i] = pm.touchents[i];
	p->viewheight = pm.viewheight;
	p->groundentity = pm.groundentity;
	p->watertype = pm.watertype;
	p->waterlevel = pm.waterlevel;
	cur = 0;
}
