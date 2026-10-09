/* Desktop stand-ins for the Quest-only hooks so the engine can be built and tested on Linux. */
#include <string.h>
typedef float vec3_t[3];
void VR_Init(void) {}
void Android_Vibrate(float duration, int channel, float intensity) { (void)duration; (void)channel; (void)intensity; }
void getVROrigins(vec3_t weaponoffset, vec3_t weaponangles, vec3_t hmdPosition)
{
	memset(weaponoffset, 0, sizeof(vec3_t));
	memset(weaponangles, 0, sizeof(vec3_t));
	memset(hmdPosition, 0, sizeof(vec3_t));
}
float getFOV(void) { return 90.0f; }
