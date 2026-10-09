/* pm_shim.h - plain-C view of Yamagi's player movement (pmove.c) for SoF engine code. */
#ifndef PM_SHIM_H
#define PM_SHIM_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct
{
	int allsolid, startsolid;
	float fraction;
	float endpos[3];
	float normal[3];
	float dist;
	int planetype, signbits;
	int surfflags;
	int contents;
	void *ent;
} pms_trace_t;

#define PMS_MAXTOUCH 32
typedef struct
{
	/* state, Q2 semantics (pm_type: 0 normal, 1 spectator/noclip, 2 dead, 3 gib, 4 freeze) */
	int pm_type;
	float origin[3], velocity[3];
	int pm_flags, pm_time;
	float gravity;
	float delta_angles[3];
	/* command */
	int msec, buttons;
	short angles[3];
	float forwardmove, sidemove, upmove;
	int snapinitial;
	/* results */
	int numtouch;
	void *touchents[PMS_MAXTOUCH];
	float viewangles[3];
	float viewheight;
	float mins[3], maxs[3];
	void *groundentity;
	int watertype, waterlevel;
	/* callbacks */
	void (*trace)(const float *start, const float *mins, const float *maxs, const float *end, pms_trace_t *out);
	int (*pointcontents)(const float *p);
} pms_t;

void pms_run(pms_t *p);
#ifdef __cplusplus
}
#endif
#endif
