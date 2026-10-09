# Soldier of Fortune build (ndk-build SOF=1 SOF_SDK_DIR=/path/to/ported/sdk/Game)
#
#   yquake2_game : the SoF adapter, loaded by the engine in place of the Quake 2 game module
#   sofgame      : the SoF SDK game module (gamecpp), 64-bit port
#   sofplayer    : the SoF SDK player/weapons module (player), 64-bit port
#
# SOF_SDK_DIR must point at the "Game" directory of the SoF SDK with the 64-bit port patch
# applied (see docs/sof/BUILDING.md). The SDK is not part of this repository.

LOCAL_PATH:= $(call my-dir)

ifeq ($(SOF_SDK_DIR),)
$(error SOF_SDK_DIR is not set: point it at the ported SoF SDK Game directory)
endif

SOF_CXXFLAGS := -std=gnu++11 -fno-strict-aliasing -fexceptions -frtti -w -include $(SOF_SDK_DIR)/qcommon/port_compat.h

# ---------------------------------------------------------------- adapter
include $(CLEAR_VARS)
LOCAL_MODULE := yquake2_game
LOCAL_CFLAGS := -DIOAPI_NO_64 -DYQ2OSTYPE=\"Linux\" -DYQ2ARCH=\"Arm\"
LOCAL_CPPFLAGS := $(SOF_CXXFLAGS)
LOCAL_C_INCLUDES := $(SOF_SDK_DIR)/gamecpp $(SOF_SDK_DIR)/qcommon $(SOF_SDK_DIR)/ghoul \
                    $(LOCAL_PATH)/src/sof/ghoul $(LOCAL_PATH)/src/sof/adapter
LOCAL_SRC_FILES := src/sof/adapter/q2side.c \
                   src/sof/adapter/sofside.cpp \
                   src/sof/ghoul/ghoul_runtime.cpp \
                   src/sof/ghoul/ghoul_gsq.cpp \
                   src/sof/ghoul/ghb_model.cpp \
                   src/sof/ghoul/ghb_dirtable.cpp \
                   $(SOF_SDK_DIR)/ghoul/matrix4.cpp \
                   $(SOF_SDK_DIR)/ghoul/vect3.cpp
LOCAL_LDLIBS := -ldl -llog
LOCAL_SHARED_LIBRARIES := yquake2
include $(BUILD_SHARED_LIBRARY)

# ---------------------------------------------------------------- SoF game module
include $(CLEAR_VARS)
LOCAL_MODULE := sofgame
LOCAL_CPPFLAGS := $(SOF_CXXFLAGS) -fvisibility=hidden -DNDEBUG -D_FINAL_ -D_SOF_ -D__GAME__ -D_RAVEN_
LOCAL_C_INCLUDES := $(SOF_SDK_DIR)/qcommon $(SOF_SDK_DIR)/player $(SOF_SDK_DIR)/gamecpp
SOF_GAME_FILES := ai ai_actions ai_body ai_bodycow ai_bodydekker ai_bodydog ai_bodyhuman ai_bodynoghoul \
  ai_decisions ai_path_pre ai_pathfinding ai_senses CWeaponInfo dm dm_arsenal dm_assassin dm_ctf dm_none \
  dm_real dm_standard ds fx_effects fx_tempents g_bosnia g_castle g_chase g_cmds g_combat g_environ g_func \
  g_generic g_ghoul g_iraq g_items g_lightmodels g_main g_misc g_monster g_newyork g_obj g_phys g_player \
  g_save g_siberia g_skilllevels g_sound g_spawn g_svcmds g_target g_tokyo g_trigger g_uganda g_utils \
  m_ecto m_female m_generic m_heliactions m_heliai m_meso m_snowcatactions m_snowcatai m_tankactions \
  m_tankai mp_ents p_body p_client p_hud p_trail p_view pt_listpointer q_sh_fx q_shared test w_equip w_fire
LOCAL_SRC_FILES := $(addprefix $(SOF_SDK_DIR)/gamecpp/,$(addsuffix .cpp,$(SOF_GAME_FILES))) \
                   $(SOF_SDK_DIR)/ghoul/matrix4.cpp $(SOF_SDK_DIR)/ghoul/vect3.cpp
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)

# ---------------------------------------------------------------- SoF player module
include $(CLEAR_VARS)
LOCAL_MODULE := sofplayer
LOCAL_CPPFLAGS := $(SOF_CXXFLAGS) -fvisibility=hidden -DNDEBUG -D_FINAL_
LOCAL_C_INCLUDES := $(SOF_SDK_DIR)/gamecpp $(SOF_SDK_DIR)/qcommon $(SOF_SDK_DIR)/player
SOF_PLAYER_FILES := player w_equip_attack w_equip_misc w_equipment w_inven w_models w_network w_utils w_weapons
LOCAL_SRC_FILES := $(addprefix $(SOF_SDK_DIR)/player/,$(addsuffix .cpp,$(SOF_PLAYER_FILES)))
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)
