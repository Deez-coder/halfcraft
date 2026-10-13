#include "bridge.h"
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <cctype>
#include <unordered_set>
#include <vector>
#include "compositor.h"
#include "input.h"
#include "log.h"
#include "ue.h"
#include "ws.h"

// Coordinates: 1 block = 1 m = 100 UU. UE (x, y, z) cm, left-handed with Z up -> Minecraft (x/100, z/100 + yOffset,
// y/100): swapping y and z gives Minecraft's right-handed Y-up frame. Minecraft yaw = UE yaw - 90 (UE yaw 0 looks
// along +X, Minecraft yaw 0 along +Z), pitch = -UE pitch. yOffset puts the ground where a match starts on y = 64.

namespace
{
	constexpr int kPort = 25599;
	constexpr double kGroundLevel = 64.0;    // Minecraft y of the ground where a match starts
	constexpr double kMaxMinecraftPixels = 1920.0 * 1080.0;
	// Damage: a Half Sword fighter has 100 HP, a Minecraft player 20, so 1 Minecraft point = 5 Half Sword HP both ways.
	// Half Sword's Get Damage removes ~0.008 HP per raw point on the torso (less on limbs).
	constexpr double kHpPerMinecraftPoint = 5.0;
	constexpr double kRawPerHp = 1.0 / 0.008;
	constexpr double kHitPushPerPoint = 70.0; // cm/s of velocity change per Minecraft damage point
	constexpr double kStandInHealth = 100.0;  // the stand-in's full health (Half Sword clamps to it every frame)
	// a fighter's hit on Steve, in Minecraft damage (before armour): from the weapon's speed where it hits (cm/s),
	// ~5 at 1000 cm/s, ~8 at 1500, more for heavy weapon heads and for the head and neck
	constexpr double kHitMinSpeed = 200.0;
	constexpr double kHitSpeedPerPoint = 160.0;
	constexpr double kHitMaxPoints = 14.0;
	constexpr double kOutOfReachAbove = 150.0; // Steve's feet this far (cm) over a fighter's: out of his reach
	constexpr double kReachOverPelvis = 220.0; // a blow can land this far (cm) over the striker's pelvis: a sword overhead
	constexpr double kReachFromChest = 300.0;  // and this far from his chest: arm and the longest polearm
	constexpr int kBlocksRadius = 48;         // Minecraft blocks around Steve that are solid in Half Sword (on a sync)

	WsClient g_ws;
	bool g_enabled = true;
	int g_generation = -1;
	int g_viewSent = 0;
	double g_yOffset = 0.0;
	bool g_haveOffset = false;
	int g_tpIn = -1; // ticks until Steve is put where the match starts (the ground streams in first)
	SDK::FVector g_startFeet;
	std::vector<std::pair<int, int>> g_spiral;
	uint64_t g_frame = 0;
	SDK::UWorld *g_world = nullptr;

	LARGE_INTEGER g_freq = {};
	LONGLONG g_nextCheck = 0;
	float g_lastStamp = -1.0f;
	bool g_inTick = false;

	// who gets the input
	bool g_userHalfSword = false; // F7
	bool g_halfSwordMenu = false; // Escape opened Half Sword's menu
	bool g_menuSeen = false;      // and it really came up (cursor shown or game paused): when it goes, so does this
	bool g_mcScreen = false;
	bool g_hadFighter = false; // a living fighter was seen in this match
	bool g_matchOver = false;  // and none is left standing (dead or given up): Half Sword's win screen ("hold G") is up
	int g_overTicks = 0;       // ticks the fight has looked over (or on again): it changes only when it stays so
	bool g_worldSetup = false;

	// Minecraft's camera (from its newest exported frame) and Steve (from "me"), Minecraft coordinates
	struct McPose
	{
		double x = 0, y = 0, z = 0;
		float yaw = 0, pitch = 0, fov = 70;
		bool valid = false;
		int64_t frame = -1; // Minecraft's frame number
	} g_mcCam;
	double g_damageHealthBefore = 0;

	// /hs damage settings from Minecraft (HsConfig): dealt, taken, and per body part
	enum Part { kHead, kNeck, kTorso, kArms, kLegs, kParts };
	struct DamageConfig
	{
		double dealt = 1.0, taken = 1.0, potions = 1.0;
		double part[kParts] = {1, 1, 1, 1, 1};
		int fps = 0; // Half Sword's frame cap (/hs fps, 0 = none): what it leaves of the GPU, Minecraft gets
		bool balance = false; // cap Half Sword a little above Minecraft's rate (Minecraft does the same the other way)
		double props = 1.0;  // how hard Steve's blows push loose things (/hs props)
		double current = 1.0; // how hard water's and lava's currents push the fighters (/hs current)
		double explosions = 1.0; // how hard explosions throw them (/hs explosions)
		double mace = 1.0;   // the mace's damage (/hs mace)
	} g_cfg;
	int g_fpsApplied = 0;
	// Minecraft's frame rate, from the export's publish counter (for the balance)
	int64_t g_balancePublished = -1;
	ULONGLONG g_balanceSince = 0;
	double g_mcFps = 0;
	const char *kPartNames[kParts] = {"head", "neck", "torso", "arms", "legs"};

	Part part_of(const std::string &bone)
	{
		if (bone == "Head" || bone == "visor_bone")
			return kHead;
		if (bone.rfind("neck", 0) == 0)
			return kNeck;
		if (bone.find("arm") != std::string::npos || bone.find("hand") != std::string::npos || bone.rfind("weapon", 0) == 0 ||
			bone.find("_0") != std::string::npos || bone.find("metacarpal") != std::string::npos)
			return kArms;
		if (bone.rfind("thigh", 0) == 0 || bone.rfind("calf", 0) == 0 || bone.rfind("foot", 0) == 0 || bone.rfind("ball", 0) == 0)
			return kLegs;
		return kTorso;
	}
	int64_t g_camFrameSet = -1; // the Minecraft frame our camera was aimed with (it renders next frame)
	struct TickStats
	{
		ULONGLONG since = 0;
		int ticks = 0, newFrames = 0, readFails = 0, matched = 0, between = 0;
		uint64_t presentsAt = 0;
		double matchErr = 0;
		int64_t ageSum = 0;
	} g_tickStats;
	struct Steve
	{
		double x = 0, y = 0, z = 0;
		float yaw = 0;
		bool valid = false;
		bool dead = false;
	} g_steve;
	SDK::FVector g_camLoc;
	SDK::FRotator g_camRot;
	float g_camFov = 90.0f;

	// engine objects
	SDK::UFunction *g_fnGetDamage = nullptr;
	SDK::UClass *g_willieClass = nullptr;

	/// An object pointer plus its GObjects slot, so a destroyed one is never used.
	struct Ref
	{
		SDK::UObject *obj = nullptr;
		int32_t index = -1;
		bool alive() const
		{
			return obj != nullptr && index >= 0 && SDK::UObject::GObjects->GetByIndex(index) == obj;
		}
	};

	// the player's fighter: Steve's invisible stand-in
	struct StandIn
	{
		Ref ref;
		double health = 100, consciousness = 100, limbs[10] = {}, stamina = 100;
		bool hidden = false;
		ULONGLONG since = 0; // when this body became the stand-in
		SDK::USceneComponent *meshParent = nullptr; // what its body hung from before it was pinned (put back as it was)
		bool meshParentKnown = false;
		// how its body met other bodies (collision channel 16) before it was pinned (put back as it was: forced to
		// "block", it was kicked the instant its physics came back - a knee put out)
		SDK::ECollisionResponse bodyResponse = SDK::ECollisionResponse::ECR_Block;
	} g_standIn;
	// the body just given back to Half Sword: its first moments are watched for a burst of speed (it used to be flung
	// out of whatever it was put back into, often to its death)
	Ref g_restored;
	ULONGLONG g_restoredAt = 0;
	bool g_restorePending = false;               // placed, physics still off: it comes on after kRestorePhysicsDelayMs
	constexpr ULONGLONG kRestorePhysicsDelayMs = 120;
	constexpr ULONGLONG kRestoreMaxWaitMs = 800;   // its legs coming down take at most this long
	constexpr double kRestoreLegsDown = 70.0;      // cm of hips over its lowest foot: its legs are down
	constexpr double kRestoreStillCm = 0.5;        // hips and feet moved less than this in a frame...
	constexpr int kRestoreStillFrames = 4;         // ...this many frames running: still
	constexpr ULONGLONG kRestoreZeroMs = 150;      // then, physics on, all its speed cleared every frame this long
	bool g_restorePlaced = false;                  // its feet put on the ground (once, before the stillness wait)
	int g_restoreStill = 0;
	SDK::FVector g_restoreMarks[3] = {};
	constexpr ULONGLONG kRestoreSettleMs = 1500; // then this long watched and kept from harm
	constexpr double kRestoreMaxSpeed = 500.0;   // cm/s: faster than this in that time is the physics shoving it
	constexpr ULONGLONG kStandInSettle = 3000; // ms a new stand-in runs as Half Sword's own before its logic is paused
	struct Placed
	{
		SDK::FVector at{};
		bool valid = false;
	} g_standInPlaced; // where the stand-in was put last frame (it must still be there)
	unsigned g_standInHeld = 0; // times hold_stand_in had to put it back
	ULONGLONG g_standInLogAt = 0;
	// Half Sword's hit events (blueprint delegates, through ProcessEvent): what touches the stand-in never reaches
	// Half Sword's damage. A fighter's weapon or fist hitting it is Steve's damage, sent to Minecraft straight away.
	enum class HitKind { Weapon, WeaponBlunt, Body, Overlap };
	struct HitEvent
	{
		const SDK::UFunction *fn;
		HitKind kind;
	};
	std::vector<HitEvent> g_hitEvents;
	const SDK::UObject *g_standInObj = nullptr; // the stand-in while it is hidden (for the event filter)
	struct HitParms
	{
		SDK::UPrimitiveComponent *hitComponent;
		SDK::AActor *otherActor;
		SDK::UPrimitiveComponent *otherComp;
		SDK::FVector normalImpulse;
		SDK::FHitResult hit;
	};
	struct SteveHit
	{
		Ref by;     // the weapon part, or the fighter's body
		HitKind kind;
		SDK::FVector at;
		SDK::FName bone, byBone;
	};
	std::vector<SteveHit> g_steveHits;
	bool g_steveWasDead = false;
	std::unordered_map<int, ULONGLONG> g_fallUntil; // fighters knocked down (a blast, a golem): back on their feet after this
	// Minecraft's mobs: each gets an invisible fighter body here, on a team of its own, that Half Sword's fighters fight
	// like any other fighter (their AI only ever targets fighters). Its health is Minecraft's: blows on it go there.
	constexpr int kMobTeam = 77;
	constexpr int kMaxMobBodies = 12;
	constexpr double kMobKnockback = 625.0; // cm/s of Half Sword body speed per block/tick of Minecraft knockback
	// upward: only a throw lifts (an iron golem's, a hoglin's; the proxies float, so Minecraft's little hop on any hit
	// never shows): an iron golem's 0.4 throws a fighter some 3 m up, as it throws Steve in Minecraft
	constexpr double kMobKnockbackUp = 2000.0;
	constexpr double kThrownPush = 1.6; // snowballs, eggs: x Minecraft's push (0.4 -> 400 cm/s)
	constexpr double kRodPull = 1.6;    // a fishing rod's yank: x Minecraft's pull
	constexpr double kWindPush = 1.0;      // wind charges, Wind Burst, a mace's shockwave: Minecraft's push as it is
	constexpr double kMaxPush = 1600.0;    // cm/s: past this a ragdoll just comes apart
	constexpr double kChargeShove = 120.0;    // cm/s of whole-body shove per m/s of a spear charge
	constexpr double kMaxChargeShove = 1800.0;
	constexpr double kChargeFloors = 5.0;     // m/s: a charge this fast (a sprint's) takes him off his feet
	constexpr double kPlayerKnockbackPerLevel = 180.0; // cm/s of whole-body shove per level of Knockback / Punch
	struct MobBody
	{
		Ref ref;
		std::string kind;
		double x = 0, y = 0, z = 0, height = 1.8, yaw = 0;
		ULONGLONG seen = 0;
		bool setUp = false;
		float scale = 1.0f;
		double health = 100, consciousness = 100, stamina = 100;
		double limbs[10] = {};
		std::vector<SteveHit> hits;
	};
	std::unordered_map<int, MobBody> g_mobBodies;              // Minecraft entity id ->
	std::unordered_map<const SDK::UObject *, int> g_mobBodyOf; // its body -> Minecraft entity id (the hit filter)
	// Minecraft's solid blocks, as invisible 1 m boxes on one actor of ours
	struct BlockBox
	{
		int x, y, z;
		Ref box; // its collision (the whole block)
		Ref nav; // its hole in the arena's navmesh (smaller: see set_block)
	};
	std::unordered_map<int64_t, BlockBox> g_blocks;
	Ref g_blockActor;
	// the other fighters, by GObjects index (the proxies' handles in Minecraft)
	std::vector<Ref> g_fighters;
	// what the stand-in carries (fists, swords on the belt and back...): hidden and without collision
	std::vector<Ref> g_carried;
	// the camera Half Sword looks through while Minecraft drives
	Ref g_camera;
	int g_fightersRefresh = 0;

	struct Keys
	{
		bool down[256] = {};
	} g_keys;

	int64_t column_key(int x, int z)
	{
		return (int64_t(x) << 32) ^ uint32_t(z);
	}

	float wrap_degrees(double a)
	{
		a = std::fmod(a, 360.0);
		if (a > 180.0)
			a -= 360.0;
		if (a <= -180.0)
			a += 360.0;
		return float(a);
	}

	void sendf(const char *format, ...)
	{
		char buffer[8192];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		g_ws.send(buffer);
	}

	SDK::FVector to_ue(double x, double y, double z)
	{
		return SDK::FVector(x * 100.0, z * 100.0, (y - g_yOffset) * 100.0);
	}

	void to_mc(const SDK::FVector &v, double &x, double &y, double &z)
	{
		x = v.X / 100.0;
		y = v.Z / 100.0 + g_yOffset;
		z = v.Y / 100.0;
	}

	SDK::FName name(const wchar_t *s)
	{
		return SDK::BasicFilesImplUtils::StringToName(s);
	}

	// ---- tiny JSON readers for the mod's flat messages ----
	const char *json_at(const std::string &m, const char *key)
	{
		const std::string k = std::string("\"") + key + "\":";
		const size_t at = m.find(k);
		return at == std::string::npos ? nullptr : m.c_str() + at + k.size();
	}

	double json_num(const std::string &m, const char *key, double fallback = 0.0)
	{
		const char *p = json_at(m, key);
		return p != nullptr ? std::strtod(p, nullptr) : fallback;
	}

	bool json_bool(const std::string &m, const char *key)
	{
		const char *p = json_at(m, key);
		return p != nullptr && std::strncmp(p, "true", 4) == 0;
	}

	std::string json_str(const std::string &m, const char *key)
	{
		const char *p = json_at(m, key);
		if (p == nullptr || *p != '"')
			return {};
		const char *end = std::strchr(p + 1, '"');
		return end != nullptr ? std::string(p + 1, end) : std::string();
	}

	int json_nums(const std::string &m, const char *key, double *out, int max)
	{
		const char *p = json_at(m, key);
		if (p == nullptr || *p != '[')
			return 0;
		int n = 0;
		++p;
		while (n < max && *p && *p != ']')
		{
			char *end = nullptr;
			out[n++] = std::strtod(p, &end);
			p = end;
			while (*p == ',' || *p == ' ')
				++p;
		}
		return n;
	}

	// ---- Minecraft's exported frames: the camera they were rendered with ----
	HANDLE g_mapping = nullptr;
	const uint8_t *g_shm = nullptr;
	int64_t g_lastPublish = -1;
	ULONGLONG g_lastPublishAt = 0;

	template <typename T>
	T rd(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	void close_frames()
	{
		if (g_shm != nullptr)
			UnmapViewOfFile(g_shm);
		if (g_mapping != nullptr)
			CloseHandle(g_mapping);
		g_shm = nullptr;
		g_mapping = nullptr;
	}

	/// The camera of Minecraft's newest exported frame (header + slot descriptors: the first 4096 bytes).
	bool newest_frame_pose(McPose &out)
	{
		if (g_shm == nullptr)
		{
			g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\MCPassthroughFrame");
			if (g_mapping == nullptr)
				return false;
			g_shm = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, 4096));
			if (g_shm == nullptr || rd<uint32_t>(g_shm) != 0x5450434D)
			{
				close_frames();
				return false;
			}
			g_lastPublishAt = GetTickCount64();
		}
		const int64_t publish = rd<int64_t>(g_shm + 32);
		if (publish != g_lastPublish)
		{
			g_lastPublish = publish;
			g_lastPublishAt = GetTickCount64();
		}
		else if (GetTickCount64() - g_lastPublishAt > 2000)
		{
			close_frames(); // Minecraft restarted (a new mapping) or stopped: open it again
			return false;
		}
		const int32_t slot = rd<int32_t>(g_shm + 40);
		if (slot < 0 || slot >= rd<int32_t>(g_shm + 12))
			return false;
		const uint8_t *d = g_shm + 256 + 128 * slot;
		const int64_t seq = rd<int64_t>(d);
		if (seq & 1)
			return false;
		McPose p;
		p.fov = rd<float>(d + 40);
		p.x = rd<double>(d + 48);
		p.y = rd<double>(d + 56);
		p.z = rd<double>(d + 64);
		p.yaw = rd<float>(d + 72);
		p.pitch = rd<float>(d + 76);
		p.frame = rd<int64_t>(d + 8);
		if (rd<int64_t>(d) != seq)
			return false;
		p.valid = true;
		out = p;
		return true;
	}

	// ---- ground ----
	/// The highest static surface (WorldStatic: floors, walls, props; not bodies or loose weapons) below `from`.
	bool ground_z(SDK::UWorld *world, double x, double y, double fromZ, double &outZ, bool withBlocks = false)
	{
		SDK::EObjectTypeQuery types[] = {SDK::EObjectTypeQuery::ObjectTypeQuery1};
		SDK::TArray<SDK::EObjectTypeQuery> typeArray(types, 1, 1);
		// not Minecraft's own blocks: the ground is Half Sword's (unless asked for: what a body stands on)
		SDK::AActor *blocks[1] = {g_blockActor.alive() && !withBlocks ? static_cast<SDK::AActor *>(g_blockActor.obj) : nullptr};
		SDK::TArray<SDK::AActor *> ignore(blocks, blocks[0] != nullptr ? 1 : 0, 1);
		SDK::FHitResult hit{};
		const bool ok = SDK::UKismetSystemLibrary::LineTraceSingleForObjects(world, SDK::FVector(x, y, fromZ), SDK::FVector(x, y, fromZ - 20000.0),
			typeArray, false, ignore, SDK::EDrawDebugTrace::None, &hit, true, SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f);
		if (!ok || !hit.bBlockingHit)
			return false;
		outZ = hit.ImpactPoint.Z;
		return true;
	}

	/// The first static thing (WorldStatic, not Minecraft's own blocks) on a line. `inside`: the line started inside a
	/// simple collision shape (box, convex: those report a hit right where a trace starts), whose bounds are then given.
	constexpr double kSubHalf = 6.25; // half a 1/8-block sub-column (cm)
	constexpr double kClearCm = 3.0;  // a follow-up trace starts this far from the surface it found (the box is 2 cm tall)

	std::string *g_groundWhy = nullptr; // /hs ground: what the traces hit, for the log

	struct TraceHit
	{
		double z = 0.0;
		bool inside = false;
		double insideBottom = 0.0, insideTop = 0.0;
	};

	/// `half`: a flat box this wide (cm, half the side) swept along the line instead of a ray: it catches anything in its
	/// footprint, so thin rails and posts can't slip between the lines.
	bool trace_static(SDK::UWorld *world, const SDK::FVector &from, const SDK::FVector &to, TraceHit &out, double half = kSubHalf)
	{
		SDK::EObjectTypeQuery types[] = {SDK::EObjectTypeQuery::ObjectTypeQuery1};
		SDK::TArray<SDK::EObjectTypeQuery> typeArray(types, 1, 1);
		SDK::AActor *blocks[1] = {g_blockActor.alive() ? static_cast<SDK::AActor *>(g_blockActor.obj) : nullptr};
		SDK::TArray<SDK::AActor *> ignore(blocks, blocks[0] != nullptr ? 1 : 0, 1);
		SDK::FHitResult hit{};
		const bool ok = half > 0.0
			? SDK::UKismetSystemLibrary::BoxTraceSingleForObjects(world, from, to, SDK::FVector(half, half, 1.0), SDK::FRotator{}, typeArray, false, ignore,
				  SDK::EDrawDebugTrace::None, &hit, true, SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f)
			: SDK::UKismetSystemLibrary::LineTraceSingleForObjects(world, from, to, typeArray, false, ignore, SDK::EDrawDebugTrace::None, &hit, true,
				  SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f);
		if (!ok || !hit.bBlockingHit)
		{
			if (g_groundWhy != nullptr)
				*g_groundWhy += "    trace " + std::to_string(int(from.Z)) + (to.Z < from.Z ? " down" : " up") + ": nothing\n";
			return false;
		}
		out.z = hit.ImpactPoint.Z;
		out.inside = hit.bStartPenetrating || hit.Distance < 0.5f;
		if (g_groundWhy != nullptr)
		{
			SDK::UPrimitiveComponent *c = hit.Component.Get();
			SDK::AActor *a = c != nullptr ? c->GetOwner() : nullptr;
			char line[512];
			snprintf(line, sizeof(line), "    trace %d %s: hit z %.0f%s %s / %s (blocks bodies %d, pawns %d)\n", int(from.Z), to.Z < from.Z ? "down" : "up", out.z,
				out.inside ? " INSIDE" : "", a != nullptr ? a->GetName().c_str() : "?", c != nullptr ? c->GetName().c_str() : "?",
				c != nullptr ? int(c->GetCollisionResponseToChannel(SDK::ECollisionChannel::ECC_PhysicsBody)) : -1,
				c != nullptr ? int(c->GetCollisionResponseToChannel(SDK::ECollisionChannel::ECC_Pawn)) : -1);
			*g_groundWhy += line;
		}
		if (out.inside)
		{
			out.insideBottom = from.Z - 50.0;
			out.insideTop = from.Z;
			if (SDK::UPrimitiveComponent *c = hit.Component.Get())
			{
				SDK::FVector origin, extent;
				float radius = 0.0f;
				SDK::UKismetSystemLibrary::GetComponentBounds(c, &origin, &extent, &radius);
				out.insideBottom = origin.Z - extent.Z;
				out.insideTop = origin.Z + extent.Z;
			}
		}
		return true;
	}

	// ---- ground: Half Sword's floors and walls for Minecraft, at 1/8 of a block ----
	// Each Minecraft block column near Steve is traced in 8x8 sub-columns (12.5 cm): straight down from high above,
	// again from just under every hit (each surface top: ground, ledge, wall top, bridge), and, where there is more
	// than one top, straight up as well (each surface's underside: the air under a bridge or an arch). That gives the
	// solid stretches of each sub-column; each Minecraft cell then is full (a barrier in Minecraft), partial (a mask
	// of 1/8-block cubes Minecraft merges into its collision: HostGround) or empty. A column whose 9 samples agree
	// (flat floor, mostly) takes them for all 64 sub-columns. Only changes are sent; full cells only where exposed.
	// (The idea - shapes merged into Minecraft's collision - is SkyCraft's, github.com/chasmlol/SkyCraft.)
	constexpr int kVoxRadius = 32;            // block columns around Steve that get collision
	constexpr double kScanFromAbove = 4000.0; // cm over Steve the down traces start (tall walls' tops, roofs)
	constexpr int kCellsBelow = 6;            // Minecraft cells sent below Steve's
	constexpr int kCellsAbove = 12;           // and above
	constexpr int kRescanCells = 3;           // Steve this many cells up or down from a column's scan: scan it again
	constexpr double kScanBudgetMs = 2.0;     // tracing per tick
	constexpr double kScanRushMs = 8.0;       // while a column this close to Steve isn't in (he'd fall through it)
	constexpr int kNearCells = 3;
	constexpr ULONGLONG kEmptyRetryMs = 1000; // a column with nothing in it (its part of the map not loaded yet?): again after,
	constexpr int kNearRetryCells = 8;        // this close to Steve (outside the map's edge, nothing ever comes: not so often)
	constexpr ULONGLONG kFarEmptyRetryMs = 10000;
	constexpr int kMaxTraces = 24;            // per sub-column
	constexpr double kMergeGapCm = 60.0;      // a gap under a floor thinner than this is closed (nothing fits in it)
	constexpr int kNearlyWholeLow = 426;      // of a cell's 448 1/8 cubes in its bottom 7 layers: counts as a whole cell
	constexpr int kSupportLayers = 4;         // a ground surface cell this many 1/8 layers up (median) gets a support barrier
	constexpr double kOpenSolidCm = 150.0;    // a one-sided surface this close over the next: solid between them
	constexpr double kOpenThinCm = 25.0;      // else it's taken to be this thick

	// the map just loaded or dropped a piece (a big one like Abyss swaps them as the player goes): for a moment a piece
	// can be gone before the one replacing it is in. Meanwhile the ground right under Steve isn't taken away (he fell
	// through it and was lifted back up); it's looked at again after
	constexpr ULONGLONG kStreamGuardMs = 3000;
	ULONGLONG g_mapChangedAt = 0;
	int g_guardX = 0, g_guardY = 0, g_guardZ = 0; // Steve's cell

	struct ScannedColumn
	{
		int refY = 0;          // Steve's cell when it was scanned
		ULONGLONG at = 0;      // when
		bool empty = false;    // nothing there (not streamed in yet?): tried again later
		int baseY = 0;         // its full cells, as bits from baseY up (neighbours' exposure)
		uint32_t fullBits = 0;
	};
	std::unordered_map<int64_t, ScannedColumn> g_scanned;
	std::unordered_map<int64_t, uint64_t> g_sentCells; // cell -> what Minecraft has (1: full, else a mask's hash)
	std::string g_voxOut;
	int g_voxOutCount = 0;
	int g_floorBacked = 0, g_floorUnbacked = 0; // scanned columns whose top floor has a whole cell (a barrier) right under it
	int g_unbackedLogged = 0;

	int64_t cell_key(int x, int y, int z)
	{
		return (int64_t(x & 0x1FFFFF) << 42) | (int64_t(y & 0x1FFFFF) << 21) | int64_t(z & 0x1FFFFF);
	}

	void reset_ground()
	{
		g_scanned.clear();
		g_sentCells.clear();
		g_voxOut.clear();
		g_voxOutCount = 0;
	}

	void flush_vox()
	{
		if (g_voxOutCount == 0)
			return;
		g_ws.send("{\"t\":\"vox\",\"c\":[" + g_voxOut + "]}");
		g_voxOut.clear();
		g_voxOutCount = 0;
	}

	std::string base64(const uint8_t *data, size_t n)
	{
		static const char *abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		out.reserve((n + 2) / 3 * 4);
		for (size_t i = 0; i < n; i += 3)
		{
			const uint32_t v = (uint32_t(data[i]) << 16) | (i + 1 < n ? uint32_t(data[i + 1]) << 8 : 0) | (i + 2 < n ? uint32_t(data[i + 2]) : 0);
			out += abc[(v >> 18) & 63];
			out += abc[(v >> 12) & 63];
			out += i + 1 < n ? abc[(v >> 6) & 63] : '=';
			out += i + 2 < n ? abc[v & 63] : '=';
		}
		return out;
	}

	/// One cell's new state for Minecraft (mask: nullptr = empty, all 0xFF = full), if it changed. `support`: a partial
	/// cell at the ground's surface that gets a barrier in Minecraft too (a real block for torches, redstone, water).
	void send_cell(int x, int y, int z, const uint8_t *mask, bool full, bool support = false)
	{
		uint64_t want = 0;
		if (full)
			want = 1;
		else if (mask != nullptr)
		{
			want = 1469598103934665603ull;
			for (int i = 0; i < 64; ++i)
				want = (want ^ mask[i]) * 1099511628211ull;
			want = (want ^ (support ? 0x5Au : 0u)) * 1099511628211ull;
			want |= 2; // (never 0 or 1)
		}
		const int64_t key = cell_key(x, y, z);
		auto it = g_sentCells.find(key);
		const uint64_t had = it != g_sentCells.end() ? it->second : 0;
		if (had == want)
			return;
		if (want == 0)
			g_sentCells.erase(key);
		else
			g_sentCells[key] = want;
		char head[64];
		snprintf(head, sizeof(head), "%s[%d,%d,%d,\"", g_voxOutCount > 0 ? "," : "", x, y, z);
		g_voxOut += head;
		if (full)
			g_voxOut += "F";
		else if (mask != nullptr)
			g_voxOut += base64(mask, 64);
		g_voxOut += support && mask != nullptr ? "\",1]" : "\"]";
		if (++g_voxOutCount >= 160)
			flush_vox();
	}

	/// The solid stretches [bottom, top] (UE z, cm) along the vertical line at (ux, uy) between zTop and zBot, top-down.
	/// Nobody gets under the lowest floor of a column: it is solid all the way down (a thin floor piece over a gap
	/// would otherwise leave no whole cell under it, so no barrier - Minecraft's water, torches and redstone need one).
	/// Gaps too thin for anything to get into (a floor piece lying just over the ground under it) are closed: they'd
	/// leave the cell under the floor short of whole.
	void finish_stretches(std::vector<std::pair<double, double>> &out, double zBot, bool solidDown = true)
	{
		for (size_t i = 0; i + 1 < out.size();)
			if (out[i].first - out[i + 1].second < kMergeGapCm)
			{
				out[i].first = out[i + 1].first;
				out.erase(out.begin() + long(i) + 1);
			}
			else
				++i;
		if (!out.empty() && solidDown)
			out.back().first = zBot;
	}

	/// Whether (x, y, z) is inside something static: a tiny box swept 1 cm down from there starts in it.
	bool inside_static(SDK::UWorld *world, double x, double y, double z)
	{
		std::string *why = g_groundWhy;
		g_groundWhy = nullptr; // (the probes aren't worth a log line each)
		TraceHit t;
		const bool in = trace_static(world, SDK::FVector(x, y, z), SDK::FVector(x, y, z - 1.0), t, 1.0) && t.inside;
		g_groundWhy = why;
		return in;
	}

	/// The bottom of the solid that (x, y, from) is in, no lower than `limit` (the shape's bounds). Probed: one mesh's
	/// bounds can reach far below it at this spot (a vaulted roof's go down to where its arches meet the walls: taking
	/// them made the air under the Cellar's roof solid). Steps doubling down to the first point outside, then halved
	/// to within 1/16 block.
	double inside_bottom(SDK::UWorld *world, double x, double y, double from, double limit)
	{
		double in = from, out = limit;
		bool found = false;
		for (double step = 12.5; from - step > limit; step *= 2.0)
		{
			if (!inside_static(world, x, y, from - step))
			{
				out = from - step;
				found = true;
				break;
			}
			in = from - step;
		}
		if (!found)
		{
			if (inside_static(world, x, y, limit + 0.5))
				return limit;
			out = limit;
		}
		while (in - out > 6.25)
		{
			const double mid = (in + out) / 2.0;
			if (inside_static(world, x, y, mid))
				in = mid;
			else
				out = mid;
		}
		if (g_groundWhy != nullptr)
			*g_groundWhy += "    inside from " + std::to_string(int(from)) + ": solid down to " + std::to_string(int((in + out) / 2.0)) + " (bounds say " +
				std::to_string(int(limit)) + ")\n";
		return (in + out) / 2.0;
	}

	void solid_stretches(SDK::UWorld *world, double ux, double uy, double zTop, double zBot, std::vector<std::pair<double, double>> &out)
	{
		out.clear();
		double from = zTop;
		double open = 0.0; // a surface top whose underside isn't known yet
		bool haveOpen = false;
		TraceHit h;
		for (int n = 0; n < kMaxTraces && from > zBot; ++n)
		{
			if (!trace_static(world, SDK::FVector(ux, uy, from), SDK::FVector(ux, uy, zBot), h))
			{
				if (haveOpen)
					out.emplace_back(zBot, open); // nothing more under it: ground (a landscape has no underside)
				finish_stretches(out, zBot);
				return;
			}
			if (h.inside)
			{
				// in a box or convex shape: solid down to its bottom here (probed: not its bounds' bottom)
				const double bottom = std::max(std::min(inside_bottom(world, ux, uy, from, std::max(h.insideBottom, zBot)), from - 12.5), zBot);
				if (haveOpen)
					out.emplace_back(bottom, open), haveOpen = false;
				else if (!out.empty() && out.back().first <= from + 2.0)
					out.back().first = std::min(out.back().first, bottom);
				else
					out.emplace_back(bottom, from);
				from = bottom - kClearCm;
				continue;
			}
			if (haveOpen)
			{
				// a mesh surface above this one: where is its underside? (a closed mesh shows it from below)
				TraceHit u;
				if (trace_static(world, SDK::FVector(ux, uy, h.z + kClearCm), SDK::FVector(ux, uy, open - kClearCm), u) && !u.inside)
					out.emplace_back(u.z, open);
				else if (open - h.z <= kOpenSolidCm)
				{
					// no underside to see (one-sided): solid down to this surface, which is part of it
					from = h.z - kClearCm;
					continue;
				}
				else
					out.emplace_back(open - kOpenThinCm, open);
			}
			open = h.z;
			haveOpen = true;
			from = h.z - kClearCm;
		}
		if (haveOpen)
			out.emplace_back(std::max(zBot, open - kOpenThinCm), open);
		// out of traces before the bottom (many floors over this one: a cellar under a building): what's under the last
		// surface found isn't known, so it isn't filled in (that filled whole rooms)
		finish_stretches(out, zBot, from <= zBot);
		if (from > zBot)
		{
			static int logged = 0;
			if (logged < 5)
			{
				++logged;
				hslog::info("ground: out of traces at %.0f, %.0f (stopped at z %.0f of %.0f)", ux, uy, from, zBot);
			}
		}
	}

	/// One Minecraft block column: its cells from Steve's - kCellsBelow to + kCellsAbove, sent where they changed.
	/// False if there was nothing to hit at all (not streamed in yet, or outside the map).
	bool scan_column(SDK::UWorld *world, int x, int z, int refY, ScannedColumn &col)
	{
		const double zRef = (refY - g_yOffset) * 100.0;
		const double zTop = zRef + kScanFromAbove, zBot = (refY - kCellsBelow - 2 - g_yOffset) * 100.0;
		std::vector<std::pair<double, double>> sub[64];
		// the 9 samples first: if they agree (in 1/8 blocks), the column is uniform
		static const int samples[3] = {0, 3, 7};
		auto eighths = [](const std::vector<std::pair<double, double>> &s) {
			std::vector<int> q;
			for (const auto &[lo, hi] : s)
			{
				q.push_back(int(std::floor((lo / 100.0 + g_yOffset) * 8.0 + 0.5)));
				q.push_back(int(std::floor((hi / 100.0 + g_yOffset) * 8.0 + 0.5)));
			}
			return q;
		};
		bool uniform = true, any = false;
		std::vector<int> first;
		for (int si = 0; si < 3; ++si)
			for (int sj = 0; sj < 3; ++sj)
			{
				const int i = samples[si], j = samples[sj];
				solid_stretches(world, (x + (i + 0.5) / 8.0) * 100.0, (z + (j + 0.5) / 8.0) * 100.0, zTop, zBot, sub[i * 8 + j]);
				any = any || !sub[i * 8 + j].empty();
				const std::vector<int> q = eighths(sub[i * 8 + j]);
				if (si == 0 && sj == 0)
					first = q;
				else if (q != first)
					uniform = false;
			}
		if (!any)
			return false;
		// and one box the size of the whole column: anything sticking up higher than the samples found (a fence's rail
		// or post between them) means the column isn't uniform after all
		if (uniform)
		{
			TraceHit whole;
			if (!trace_static(world, SDK::FVector((x + 0.5) * 100.0, (z + 0.5) * 100.0, zTop), SDK::FVector((x + 0.5) * 100.0, (z + 0.5) * 100.0, zBot),
					whole, 50.0))
				uniform = sub[0].empty();
			else if (whole.inside || sub[0].empty() || whole.z > sub[0].front().second + 100.0 / 16.0)
				uniform = false;
		}
		for (int i = 0; i < 8; ++i)
			for (int j = 0; j < 8; ++j)
			{
				const bool sampled = (i == 0 || i == 3 || i == 7) && (j == 0 || j == 3 || j == 7);
				if (sampled)
					continue;
				if (uniform)
					sub[i * 8 + j] = sub[0];
				else
					solid_stretches(world, (x + (i + 0.5) / 8.0) * 100.0, (z + (j + 0.5) / 8.0) * 100.0, zTop, zBot, sub[i * 8 + j]);
			}
		// the cells
		const int y0 = refY - kCellsBelow, y1 = refY + kCellsAbove;
		uint8_t masks[kCellsBelow + kCellsAbove + 1][64];
		int counts[kCellsBelow + kCellsAbove + 1] = {};
		for (int y = y0; y <= y1; ++y)
		{
			uint8_t *m = masks[y - y0];
			std::memset(m, 0, 64);
			for (int k = 0; k < 8; ++k)
			{
				const double cz = (y + (k + 0.5) / 8.0 - g_yOffset) * 100.0; // the 1/8 layer's middle
				for (int j = 0; j < 8; ++j)
					for (int i = 0; i < 8; ++i)
						for (const auto &[lo, hi] : sub[i * 8 + j])
							if (cz >= lo && cz <= hi)
							{
								m[k * 8 + j] |= uint8_t(1 << i);
								++counts[y - y0];
								break;
							}
			}
		}
		// A floor a little under a cell's top leaves that cell 7/8 full - not a whole cell, so no barrier right under the
		// floor, and Minecraft's torches, redstone and water there would have nothing real to stand on. Solid through
		// its bottom 7 layers (all but a few of those 448 1/8 cubes) counts as whole: Minecraft's floor then is up to
		// 1/8 block over Half Sword's there, about the size of its own bumps. (Walls and rails don't fill 7 whole layers.)
		for (int y = y0; y <= y1; ++y)
		{
			const int c = counts[y - y0];
			if (c == 0 || c == 512)
				continue;
			int low = 0;
			for (int b = 0; b < 56; ++b)
				low += std::popcount(unsigned(masks[y - y0][b]));
			if (low >= kNearlyWholeLow)
			{
				std::memset(masks[y - y0], 0xFF, 64);
				counts[y - y0] = 512;
			}
		}
		col.baseY = y0;
		col.fullBits = 0;
		for (int y = y0; y <= y1; ++y)
			if (counts[y - y0] == 512)
				col.fullBits |= 1u << (y - y0);
		// does the ground (the lowest floor: whole cells from the bottom up, then the first that isn't) have a whole cell
		// (a barrier: support for torches, redstone, water) right under its surface? (Things standing on it don't count.)
		{
			int top = -1;
			for (int y = y0; y <= y1 && top < 0; ++y)
				if (counts[y - y0] < 512)
					top = counts[y - y0] == 0 ? y - 1 : y;
			if (top >= y0)
			{
				auto layers = [&](int y) { // the highest 1/8 layer with anything, 1..8
					int h = 0;
					for (int k = 0; k < 8; ++k)
						for (int j = 0; j < 8; ++j)
							if (masks[y - y0][k * 8 + j] != 0)
								h = k + 1;
					return h;
				};
				const bool backed = counts[top - y0] == 512 || (layers(top) <= 4 && top > y0 && counts[top - 1 - y0] == 512);
				if (backed)
					++g_floorBacked;
				else
				{
					++g_floorUnbacked;
					if (g_unbackedLogged < 12)
					{
						++g_unbackedLogged;
						std::string col;
						for (int y = y1; y >= y0; --y)
						{
							const int c = counts[y - y0];
							char cell[16];
							snprintf(cell, sizeof(cell), "%s%d:%s", col.empty() ? "" : " ", y, c == 0 ? "." : c == 512 ? "#" : "");
							col += cell;
							if (c > 0 && c < 512)
								col += std::to_string(layers(y)) + "/8(" + std::to_string(c) + ")";
						}
						hslog::info("ground: floor without a whole cell under it at %d,%d (%s): %s", x, z, uniform ? "uniform" : "detailed", col.c_str());
					}
				}
			}
		}
		auto fullAt = [&](int cx, int cz, int y) {
			if (cx == x && cz == z)
				return y >= y0 && y <= y1 && counts[y - y0] == 512;
			auto it = g_scanned.find(column_key(cx, cz));
			if (it == g_scanned.end() || it->second.empty)
				return false; // not known: count as open (the cell is sent)
			const int k = y - it->second.baseY;
			return k >= 0 && k < 32 && (it->second.fullBits & (1u << k)) != 0;
		};
		// the ground's surface cell: the first one over the whole cells from the bottom up. If the ground's typical height
		// in it (the median of its 64 sub-columns, so a tree trunk or a wheel on part of it doesn't count) is at least
		// half way up, it gets a barrier as support: things placed go on top of it; lower, they go into it and rest on
		// the whole cell under it. Either way within half a block of the ground. Props on the ground get none.
		int surface = -1;
		for (int y = y0; y <= y1 && surface < 0; ++y)
			if (counts[y - y0] < 512)
				surface = y;
		bool surfaceSupport = false;
		if (surface >= y0 && counts[surface - y0] > 0)
		{
			const uint8_t *m = masks[surface - y0];
			int heights[64];
			for (int j = 0; j < 8; ++j)
				for (int i = 0; i < 8; ++i)
				{
					int h = 0;
					while (h < 8 && (m[h * 8 + j] & (1 << i)) != 0)
						++h;
					heights[j * 8 + i] = h;
				}
			std::nth_element(heights, heights + 32, heights + 64);
			surfaceSupport = heights[32] >= kSupportLayers;
		}
		const bool guard = GetTickCount64() - g_mapChangedAt < kStreamGuardMs && std::abs(x - g_guardX) <= 2 && std::abs(z - g_guardZ) <= 2;
		bool kept = false;
		for (int y = y0; y <= y1; ++y)
		{
			const int c = counts[y - y0];
			if (c == 0 && guard && y <= g_guardY && y >= g_guardY - 3 && g_sentCells.count(cell_key(x, y, z)))
				kept = true; // (under Steve while the map swaps pieces: kept for now)
			else if (c == 0)
				send_cell(x, y, z, nullptr, false);
			else if (c < 512)
				send_cell(x, y, z, masks[y - y0], false, y == surface && surfaceSupport);
			else
			{
				// full: only where something can touch it (deep inside the ground nothing does)
				const bool exposed = !fullAt(x, z, y + 1) || !fullAt(x, z, y - 1) || !fullAt(x + 1, z, y) || !fullAt(x - 1, z, y) ||
					!fullAt(x, z + 1, y) || !fullAt(x, z - 1, y);
				if (exposed || g_sentCells.count(cell_key(x, y, z)))
					send_cell(x, y, z, nullptr, true);
			}
		}
		if (kept)
		{
			col.empty = true; // (looked at again in a second: then it goes, if it's still not there)
			col.at = GetTickCount64();
		}
		return true;
	}

	/// Scan the columns around `feet` (UE) that need it, nearest first, for kScanBudgetMs (more while the ones right
	/// around him aren't in: he'd fall through).
	void sample_ground(SDK::UWorld *world, const SDK::FVector &feet)
	{
		// a big map (Abyss) loads its ground in pieces around the player as he goes: a piece coming (or going) is looked
		// at again, nearest first (only what changed is sent). Columns scanned before their piece was in found nothing,
		// or less than is there now: the ground had stopped further out until /hs fixground
		static int levelsSeen = -1;
		const int levels = world->Levels.Num();
		if (levels != levelsSeen)
		{
			if (levelsSeen >= 0)
			{
				g_scanned.clear();
				g_mapChangedAt = GetTickCount64();
				hslog::info("ground: the map has %d pieces loaded now (was %d): scanning again", levels, levelsSeen);
			}
			levelsSeen = levels;
		}
		if (g_spiral.empty())
		{
			for (int dx = -kVoxRadius; dx <= kVoxRadius; ++dx)
				for (int dz = -kVoxRadius; dz <= kVoxRadius; ++dz)
					if (dx * dx + dz * dz <= kVoxRadius * kVoxRadius)
						g_spiral.emplace_back(dx, dz);
			std::sort(g_spiral.begin(), g_spiral.end(), [](auto &a, auto &b) {
				return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
			});
		}
		LARGE_INTEGER freq, t0, now;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);
		// Minecraft column (x, z) covers UE x in [x, x+1) m and UE y in [z, z+1) m
		const int px = int(std::floor(feet.X / 100.0)), pz = int(std::floor(feet.Y / 100.0));
		const int refY = int(std::floor(feet.Z / 100.0 + g_yOffset));
		const ULONGLONG tick = GetTickCount64();
		g_guardX = px;
		g_guardY = refY;
		g_guardZ = pz;
		auto needs = [&](int x, int z) {
			auto it = g_scanned.find(column_key(x, z));
			if (it == g_scanned.end())
				return true;
			if (!it->second.empty)
				return std::abs(it->second.refY - refY) >= kRescanCells;
			const bool close = std::abs(x - px) <= kNearRetryCells && std::abs(z - pz) <= kNearRetryCells;
			return tick - it->second.at >= (close ? kEmptyRetryMs : kFarEmptyRetryMs);
		};
		bool nearMissing = false;
		for (int dx = -kNearCells; dx <= kNearCells && !nearMissing; ++dx)
			for (int dz = -kNearCells; dz <= kNearCells && !nearMissing; ++dz)
				nearMissing = needs(px + dx, pz + dz);
		const long long budget = static_cast<long long>(freq.QuadPart * (nearMissing ? kScanRushMs : kScanBudgetMs) / 1000.0);
		static int scannedThisSecond = 0, slowColumns = 0;
		static ULONGLONG statsAt = 0;
		for (const auto &[dx, dz] : g_spiral)
		{
			const int x = px + dx, z = pz + dz;
			const int64_t key = column_key(x, z);
			if (!needs(x, z))
				continue;
			QueryPerformanceCounter(&now);
			if (now.QuadPart - t0.QuadPart > budget)
				break;
			ScannedColumn col;
			col.refY = refY;
			col.at = tick;
			if (!scan_column(world, x, z, refY, col))
				col.empty = true;
			g_scanned[key] = col;
			++scannedThisSecond;
		}
		flush_vox();
		// nothing at all under Steve, as far down as 200 m (his part of the map isn't loaded yet; ground just further down
		// than the scan reaches is a fall): Minecraft holds him up until there is
		static bool hovering = false;
		auto own = g_scanned.find(column_key(px, pz));
		double below = 0.0;
		const bool hover = own != g_scanned.end() && own->second.empty && !ground_z(world, feet.X, feet.Y, feet.Z + 50.0, below);
		if (hover != hovering)
		{
			hovering = hover;
			g_ws.send(hover ? "{\"t\":\"hover\",\"on\":true}" : "{\"t\":\"hover\",\"on\":false}");
			hslog::info("ground: nothing under Steve at %d, %d: %s", px, pz, hover ? "held up" : "there is now");
		}
		if (tick - statsAt >= 5000)
		{
			if (scannedThisSecond > 0)
				hslog::info("ground: %d columns scanned in 5 s, %zu cells in Minecraft; floors on a whole cell %d, not %d", scannedThisSecond,
					g_sentCells.size(), g_floorBacked, g_floorUnbacked);
			statsAt = tick;
			scannedThisSecond = 0;
			(void)slowColumns;
		}
	}

	// ---- fighters ----
	SDK::AWillie_BP_C *as_willie(SDK::UObject *o)
	{
		return o != nullptr && g_willieClass != nullptr && o->IsA(g_willieClass) ? static_cast<SDK::AWillie_BP_C *>(o) : nullptr;
	}

	SDK::FVector feet_of(SDK::AWillie_BP_C *w)
	{
		SDK::FVector at = w->K2_GetActorLocation();
		at.Z -= w->CapsuleComponent != nullptr ? w->CapsuleComponent->CapsuleHalfHeight : 90.0;
		return at;
	}

	void hide_carried(SDK::AActor *a, bool hidden)
	{
		a->SetActorHiddenInGame(hidden);
		a->SetActorEnableCollision(!hidden); // invisible fists or swords must not hit anyone
	}

	// the scan for fighters and what the stand-in carries goes through all of GObjects (100k+): a slice per tick, so it
	// never costs a frame
	constexpr int kScanPerTick = 12000;
	int g_scanAt = 0;
	std::vector<Ref> g_scanFighters;

	// Half Sword's give-up widget (UI_GiveUp_C: hold G to give up): its meter, found by the same scan
	SDK::UClass *g_giveUpClass = nullptr;
	std::vector<Ref> g_giveUps, g_scanGiveUps;

	/// While Steve fights (the input is Minecraft's, his stand-in alive, the fight on), Half Sword's give-up meter stays
	/// empty: G is Minecraft's then, so nothing should fill it - yet something did (a G Half Sword still thought held
	/// after a menu, or the new stand-in's own logic): fight after fight was given up a few seconds in.
	void hold_give_up()
	{
		if (g_giveUps.empty() || g_matchOver || !input::routed_to_minecraft() || !g_standIn.ref.alive())
			return;
		auto *w = as_willie(g_standIn.ref.obj);
		if (w == nullptr || w->DED)
			return;
		for (const Ref &r : g_giveUps)
		{
			if (!r.alive())
				continue;
			auto *base = reinterpret_cast<uint8_t *>(r.obj);
			// only Steve's own (the fighters give up by Half Sword's own fear and wounds: never touched)
			if (SDK::UObject *master = *reinterpret_cast<SDK::UObject **>(base + 0x0318); master != nullptr && master != w) // My_Master
				continue;
			double &meter = *reinterpret_cast<double *>(base + 0x0328); // UI_GiveUp_C::Hold_Meter
			const bool gave = *reinterpret_cast<bool *>(base + 0x0348); // UI_GiveUp_C::Gave_up
			if (meter > 0.0 || gave)
			{
				static ULONGLONG said = 0;
				if (GetTickCount64() - said > 2000)
				{
					said = GetTickCount64();
					hslog::info("Half Sword's give-up meter was filling mid-fight (%.2f%s): emptied", meter, gave ? ", already given up" : "");
				}
				meter = 0.0;
			}
		}
	}


	void refresh_fighters(SDK::UWorld *world, SDK::UObject *standIn)
	{
		const int total = SDK::UObject::GObjects->Num();
		if (g_scanAt == 0)
		{
			g_scanFighters.clear();
			g_scanGiveUps.clear();
			if (g_giveUpClass == nullptr)
				g_giveUpClass = SDK::UObject::FindClassFast("UI_GiveUp_C");
		}
		const int end = std::min(total, g_scanAt + kScanPerTick);
		for (int i = g_scanAt; i < end; ++i)
		{
			SDK::UObject *o = SDK::UObject::GObjects->GetByIndex(i);
			if (o != nullptr && g_giveUpClass != nullptr && o->IsA(g_giveUpClass) && !o->IsDefaultObject())
				g_scanGiveUps.push_back({o, i});
			if (o != nullptr && standIn != nullptr && o != standIn && o->IsA(SDK::EClassCastFlags::Actor) && !o->IsDefaultObject())
			{
				auto *a = static_cast<SDK::AActor *>(o);
				SDK::USceneComponent *parent = a->RootComponent != nullptr ? a->RootComponent->AttachParent : nullptr;
				if (!a->bHidden && ((a->Owner != nullptr && g_mobBodyOf.count(a->Owner)) || (parent != nullptr && g_mobBodyOf.count(parent->Outer))))
					hide_carried(a, true); // a mob's body carries nothing anyone could see or be hit by
				if ((a->Owner == standIn || (parent != nullptr && parent->Outer == standIn)) && !a->bHidden)
				{
					hide_carried(a, true);
					g_carried.push_back({o, i});
					hslog::info("stand-in carries %s: hidden", a->GetName().c_str());
				}
			}
			auto *w = as_willie(o);
			if (w == nullptr || w == standIn || w->IsDefaultObject() || w->Outer == nullptr || w->Outer->Outer != world || w->Team_Int == kMobTeam)
				continue;
			// a living body nobody drives isn't a fighter: the last fight's bodies wait at the map's centre (no player,
			// no AI) until Half Sword cleans them up, and mobs went for them first (the dead keep their AI controller)
			if (w->Controller == nullptr && w->Health > 0.0 && !w->DED)
				continue;
			g_scanFighters.push_back({o, i});
		}
		g_scanAt = end;
		if (g_scanAt >= total)
		{
			g_fighters = g_scanFighters; // a whole pass: the new list
			g_giveUps = g_scanGiveUps;
			g_scanAt = 0;
		}
	}

	SDK::AWillie_BP_C *fighter_by_handle(int handle)
	{
		for (const Ref &r : g_fighters)
			if (r.index == handle && r.alive())
				return as_willie(r.obj);
		return nullptr;
	}

	/// Half Sword's game mode says the match is won (BP_HalfSwordGameMode_C::Match_Won, at 0x0394).
	bool match_won(SDK::UWorld *world)
	{
		static SDK::UClass *gmClass = nullptr;
		SDK::AGameModeBase *gm = world != nullptr ? world->AuthorityGameMode : nullptr;
		if (gmClass == nullptr && gm != nullptr && gm->Class != nullptr && gm->Class->GetName() == "BP_HalfSwordGameMode_C")
			gmClass = gm->Class;
		if (gm == nullptr || gmClass == nullptr || !gm->IsA(gmClass))
			return false;
		return *(reinterpret_cast<const bool *>(reinterpret_cast<const uint8_t *>(gm) + 0x0394));
	}

	// a fighter's body for Minecraft's crosshair: points along the skeleton (the client tests its ray against capsules
	// between them, so a fighter on the ground can be hit where he lies)
	const wchar_t *kBodyBones[] = {L"Head", L"neck_01", L"spine_03", L"pelvis", L"upperarm_l", L"lowerarm_l", L"hand_l", L"upperarm_r", L"lowerarm_r",
		L"hand_r", L"thigh_l", L"calf_l", L"foot_l", L"thigh_r", L"calf_r", L"foot_r"};
	constexpr int kBodyPoints = sizeof(kBodyBones) / sizeof(kBodyBones[0]);

	/// The other fighters for Minecraft's proxies: {"t":"peds","p":[[handle, feet x,y,z, then the body points],...]}.
	void send_fighters(SDK::UWorld *world)
	{
		static SDK::FName bones[kBodyPoints];
		static bool named = false;
		if (!named)
		{
			named = true;
			for (int i = 0; i < kBodyPoints; ++i)
				bones[i] = name(kBodyBones[i]);
		}
		std::string list;
		int standing = 0;
		for (const Ref &r : g_fighters)
		{
			auto *w = r.alive() ? as_willie(r.obj) : nullptr;
			if (w == nullptr)
				continue;
			const bool dead = w->Health <= 0.0 || w->DED;
			if (!dead && !w->Give_Up)
				++standing;
			// where the proxy stands: under the body's hips, not the movement capsule (a dead fighter's capsule drops
			// away through the arena, and Minecraft checks Steve's reach against the proxy)
			SDK::FVector stand = feet_of(w);
			if (w->Mesh != nullptr)
			{
				const SDK::FVector hips = w->Mesh->GetSocketLocation(bones[3]);
				stand = SDK::FVector(hips.X, hips.Y, hips.Z - 90.0);
			}
			double x, y, z;
			to_mc(stand, x, y, z);
			char e[96];
			snprintf(e, sizeof(e), "%s[%d,%.3f,%.3f,%.3f", list.empty() ? "" : ",", r.index, x, y, z);
			list += e;
			if (w->Mesh != nullptr)
				for (int i = 0; i < kBodyPoints; ++i)
				{
					to_mc(w->Mesh->GetSocketLocation(bones[i]), x, y, z);
					snprintf(e, sizeof(e), ",%.3f,%.3f,%.3f", x, y, z);
					list += e;
				}
			list += dead ? ",1]" : ",0]"; // dead: still a body to cut, but nobody's foe
		}
		g_ws.send("{\"t\":\"peds\",\"p\":[" + list + "]}");
		if (standing > 0)
			g_hadFighter = true;
		// over: nobody left standing (dead or given up) after a fight, or Half Sword says it's won. It has to look so
		// for a while before it counts (a fighter between ragdoll and getting up), both ways.
		// Half Sword's own word that the match is won counts too (someone knocked out or surrendering still stands),
		// but only for the fighters it was won against: a new one standing is a new fight, whatever the flag says
		static bool wasWon = false;
		static std::unordered_set<int> wonAgainst;
		const bool won = match_won(world);
		if (won != wasWon)
		{
			wasWon = won;
			wonAgainst.clear();
			if (won)
				for (const Ref &r : g_fighters)
					wonAgainst.insert(r.index);
			hslog::info("Half Sword's Match_Won: %d (%d standing)", won, standing);
		}
		bool newcomer = false;
		for (const Ref &r : g_fighters)
		{
			auto *f = r.alive() ? as_willie(r.obj) : nullptr;
			if (f != nullptr && f->Health > 0.0 && !f->Give_Up && wonAgainst.count(r.index) == 0)
				newcomer = true;
		}
		const bool over = (won && !newcomer) || (standing == 0 && g_hadFighter);
		if (over == g_matchOver)
			g_overTicks = 0;
		else if (++g_overTicks >= (over ? 30 : 60))
		{
			g_overTicks = 0;
			g_matchOver = over;
			input::share_continue_key(over);
			hslog::info(over ? "match over: G goes to Half Sword too (hold it to continue)" : "fight on");
		}
	}

	/// The blade (and its hit box) of a sword the stand-in carries: Steve's strikes land in Half Sword as that blade.
	struct Blade
	{
		Ref weapon;
		SDK::UPrimitiveComponent *head = nullptr, *pommel = nullptr;
		SDK::UBoxComponent *headBox = nullptr;
	} g_blade;

	void find_blade()
	{
		if (g_blade.weapon.alive())
			return;
		g_blade = {};
		static SDK::UClass *weaponClass = SDK::UObject::FindClassFast("ModularWeaponBP_C");
		static SDK::UClass *boxClass = SDK::UBoxComponent::StaticClass();
		for (const Ref &r : g_carried)
		{
			if (!r.alive() || weaponClass == nullptr || !r.obj->IsA(weaponClass))
				continue;
			auto *wpn = static_cast<SDK::AModularWeaponBP_C *>(r.obj);
			if (wpn->Head == nullptr)
				continue;
			g_blade.weapon = r;
			g_blade.head = wpn->Head;
			g_blade.pommel = wpn->Pommel;
			for (SDK::UActorComponent *c : wpn->BlueprintCreatedComponents)
				if (c != nullptr && c->IsA(boxClass) && static_cast<SDK::USceneComponent *>(c)->AttachParent == wpn->Head)
					g_blade.headBox = static_cast<SDK::UBoxComponent *>(c);
			hslog::info("Steve's blade: %s (box %s)", wpn->GetName().c_str(), g_blade.headBox ? g_blade.headBox->GetName().c_str() : "-");
			if (wpn->GetName().find("Dagger") == std::string::npos)
				break; // a sword beats a dagger
		}
	}

	/// Where a strike from `from` along `ray` (UE, unit) meets the fighter's body: the bone and point it hits, or the
	/// bone nearest the line when the line misses (Minecraft's box was hit, the body leans out of it).
	void find_strike(SDK::AWillie_BP_C *w, const SDK::FVector &from, const SDK::FVector &ray, SDK::FName &bone, SDK::FVector &at)
	{
		auto *mesh = w->Mesh;
		const SDK::FVector end(from.X + ray.X * 800.0, from.Y + ray.Y * 800.0, from.Z + ray.Z * 800.0);
		SDK::FVector loc, nrm;
		SDK::FName hitBone;
		SDK::FHitResult hit{};
		if (mesh->K2_LineTraceComponent(from, end, false, false, false, &loc, &nrm, &hitBone, &hit) && !hitBone.IsNone())
		{
			bone = hitBone;
			at = loc;
			return;
		}
		static const wchar_t *candidates[] = {L"Head", L"neck_01", L"spine_05", L"spine_03", L"pelvis", L"upperarm_l", L"upperarm_r", L"lowerarm_l",
			L"lowerarm_r", L"thigh_l", L"thigh_r", L"calf_l", L"calf_r"};
		double best = 1e30;
		for (const wchar_t *b : candidates)
		{
			const SDK::FName n = name(b);
			const SDK::FVector p = mesh->GetSocketLocation(n);
			const SDK::FVector d(p.X - from.X, p.Y - from.Y, p.Z - from.Z);
			const double along = std::max(0.0, d.X * ray.X + d.Y * ray.Y + d.Z * ray.Z);
			const SDK::FVector c(from.X + ray.X * along - p.X, from.Y + ray.Y * along - p.Y, from.Z + ray.Z * along - p.Z);
			const double dist = c.X * c.X + c.Y * c.Y + c.Z * c.Z;
			if (dist < best)
			{
				best = dist;
				bone = n;
				at = p;
			}
		}
	}

	/// A Minecraft strike on a fighter, through Half Sword's own hit pipeline (Deal Complex Damage: armour, damage,
	/// wounds, blood, broken bones, dismemberment), as if `striker` hit `bone` at `speed` cm/s. Then the /hs multipliers.
	double *stand_in_limbs(SDK::AWillie_BP_C *w, int i); // a fighter's limb pools (0 head .. 8 back)

	void strike_fighter(SDK::AWillie_BP_C *w, SDK::FName bone, const SDK::FVector &at, SDK::FVector dir, double speed, double cutting, double stab,
		double rigidity, int blunt, bool extraHigh, SDK::UPrimitiveComponent *striker, SDK::UBoxComponent *box, const char *what, double draw = 0.0,
		bool lower = false)
	{
		auto *mesh = w->Mesh;
		if (mesh == nullptr)
			return; // (the dead too: a body can still be hit)
		const double len = std::sqrt(dir.X * dir.X + dir.Y * dir.Y + dir.Z * dir.Z);
		dir = len > 1e-6 ? SDK::FVector(dir.X / len, dir.Y / len, dir.Z / len) : SDK::FVector(0, 0, 1);
		const SDK::FVector vel(dir.X * speed, dir.Y * speed, dir.Z * speed);
		const double before = w->Health;
		SDK::EPhysicalSurface surface{};
		double damage = 0, cutOut = 0, rigOut = 0, density = 0;
		bool lowerOut = false;
		w->Deal_Complex_Damage(mesh, striker, bone, at, SDK::FVector(-dir.X, -dir.Y, -dir.Z), vel, vel, cutting, stab, rigidity, blunt, lower, false, 1.0,
			box, extraHigh, draw, &surface, &damage, &cutOut, &rigOut, &density, &lowerOut);
		const double lost = before - w->Health;
		const std::string boneName = bone.ToString();
		const double mult = g_cfg.dealt * g_cfg.part[part_of(boneName)];
		if (lost > 0.0 && mult != 1.0 && w->Health > 0.0)
		{
			w->Health = before - lost * mult;
			if (w->Health <= 0.0)
				w->Death();
		}
		hslog::info("%s -> %s %s: %.0f cm/s: health %.1f -> %.1f (x%.2f)", what, w->GetName().c_str(), boneName.c_str(), speed, before, w->Health,
			mult);
	}

	void on_hit(const std::string &m)
	{
		auto *w = fighter_by_handle(int(json_num(m, "h", -1)));
		if (w == nullptr || w->Mesh == nullptr)
			return;
		double from[3] = {0, 0, 0}, ray[3] = {0, 0, 0};
		const bool traced = json_nums(m, "from", from, 3) == 3 && json_nums(m, "ray", ray, 3) == 3;
		const std::string kind = json_str(m, "k"), item = json_str(m, "item");
		const double points = json_num(m, "d");
		const bool crit = json_bool(m, "crit");
		const SDK::FVector start = traced ? to_ue(from[0], from[1], from[2]) : w->K2_GetActorLocation();
		const SDK::FVector dir = traced ? SDK::FVector(ray[0], ray[2], ray[1]) : SDK::FVector(0, 0, -1);
		SDK::FName bone;
		SDK::FVector at;
		find_strike(w, start, dir, bone, at);
		find_blade();
		const bool arrow = kind == "arrow" || item == "trident";
		const bool thrown = kind == "snowball" || kind == "egg"; // only a push (below), no blow
		// Minecraft's damage sets how hard: ~400 cm/s for a fist, ~1500 for a diamond sword, more for crits
		double speed = std::clamp(350.0 + 160.0 * points, 350.0, 2400.0) * (crit ? 1.3 : 1.0);
		const bool mace = item == "mace";
		// a mace smash: Minecraft's bonus for the fall (12 for 3 blocks, 22 for 8, +1 a block after that) as the speed
		// of the blow, past the usual cap: from high enough it's a killing blow, as in Minecraft
		const double smash = json_num(m, "smash", 0.0);
		if (smash > 0.0)
			speed = std::clamp(1800.0 + 110.0 * smash, 1800.0, 6000.0);
		// a spear's charge: how fast Steve came on (m/s: ~5.6 sprinting, ~14 on a horse) past the cap too
		const double charge = json_num(m, "charge", 0.0);
		if (charge > 0.0)
			speed = std::clamp(2400.0 + 450.0 * charge, 2400.0, 8000.0);
		const bool spear = item.size() > 6 && item.compare(item.size() - 6, 6, "_spear") == 0;
		if (thrown)
		{
		}
		else if (arrow)
		{
			speed = std::clamp(900.0 + 80.0 * points, 900.0, 3000.0);
			strike_fighter(w, bone, at, dir, speed, 15.0, 0.6, 0.4, 0, true, g_blade.head != nullptr ? g_blade.head : w->Mesh, nullptr, "arrow");
		}
		else if (kind == "player")
		{
			// every Minecraft weapon, fists, blocks, tools: blunt, like a pommel or a club; a spear stabs, as an arrow
			SDK::UPrimitiveComponent *by = spear && g_blade.head != nullptr ? g_blade.head : g_blade.pommel;
			if (by == nullptr && g_standIn.ref.alive())
				by = as_willie(g_standIn.ref.obj)->Mesh;
			if (spear)
				strike_fighter(w, bone, at, dir, speed, 15.0, 0.6, 0.4, 0, crit || charge > 0.0, by != nullptr ? by : w->Mesh, nullptr, item.c_str());
			else if (mace)
			{
				// (/hs mace: on what the blow took)
				const double before = w->Health;
				strike_fighter(w, bone, at, dir, speed, 0.0, 0.0, 1.1, 0, crit, by != nullptr ? by : w->Mesh, nullptr, item.c_str());
				if (g_cfg.mace != 1.0 && w->Health < before && w->Health > 0.0)
				{
					w->Health = before - (before - w->Health) * g_cfg.mace;
					if (w->Health <= 0.0)
						w->Death();
				}
			}
			else
				strike_fighter(w, bone, at, dir, speed, 0.0, 0.0, 1.1, 0, crit, by != nullptr ? by : w->Mesh, nullptr, item.empty() ? "fist" : item.c_str());
		}
		else
		{
			// a mob: a bite or a blow, blunt
			SDK::UPrimitiveComponent *by = g_standIn.ref.alive() ? as_willie(g_standIn.ref.obj)->Mesh : w->Mesh;
			strike_fighter(w, bone, at, dir, speed, kind == "zombie" ? 5.0 : 0.0, 0.0, 1.0, 1, false, by, nullptr, kind.c_str());
		}
		double kb[3];
		if (json_nums(m, "kb", kb, 3) == 3)
		{
			// a mob's blow: Minecraft's own knockback for it (measured on the fighter's proxy, blocks per tick): the
			// usual push, an iron golem's fling up, a ravager's shove; as a change of speed of his whole body
			// (a snowball or an egg pushes as a blow would in Minecraft; stronger here, with a little lift, or a fighter's
			// balance just takes it: one snowball should move him back)
			const double scale = thrown ? kThrownPush : 1.0;
			const SDK::FVector v(kb[0] * kMobKnockback * scale, kb[2] * kMobKnockback * scale,
				thrown ? 100.0 : kb[1] * (kb[1] > 0.0 ? kMobKnockbackUp : kMobKnockback));
			w->Mesh->AddImpulseToAllBodiesBelow(v, name(L"pelvis"), true, true);
			const double horizontal = std::sqrt(kb[0] * kb[0] + kb[2] * kb[2]);
			if (kb[1] > 0.3 || horizontal > 0.9)
			{
				w->Force_Fall = true; // thrown: down for a moment, as from a blast
				g_fallUntil[int(json_num(m, "h", -1))] = GetTickCount64() + 1200;
			}
			hslog::info("%s knocks fighter %d back: %.2f, %.2f up (blocks/tick)", kind.c_str(), int(json_num(m, "h", -1)), horizontal, kb[1]);
			return;
		}
		// Minecraft's knockback, on top of Half Sword's own reaction
		const double push = std::min(points * 40.0 * (crit ? 1.5 : 1.0), kMaxPush);
		w->Mesh->AddImpulse(SDK::FVector(dir.X * push, dir.Y * push, std::max(dir.Z, 0.15) * push), bone, true);
		// a spear's charge throws him back the way Steve came, his whole body; at a gallop it takes him off his feet
		if (charge > 0.0)
		{
			const int handle = int(json_num(m, "h", -1));
			const double h = std::sqrt(dir.X * dir.X + dir.Y * dir.Y), shove = std::min(kChargeShove * charge, kMaxChargeShove);
			if (h > 1e-3)
				w->Mesh->AddImpulseToAllBodiesBelow(SDK::FVector(dir.X / h * shove, dir.Y / h * shove, shove * 0.15), name(L"pelvis"), true, true);
			if (charge >= kChargeFloors)
			{
				w->Force_Fall = true;
				g_fallUntil[handle] = GetTickCount64() + 1500;
			}
			hslog::info("spear charge on fighter %d: %.1f m/s, %.1f Minecraft damage", handle, charge, points);
		}
		// a real smash (3 blocks and more) floors him
		if (smash >= 12.0)
		{
			const int handle = int(json_num(m, "h", -1));
			w->Force_Fall = true;
			g_fallUntil[handle] = GetTickCount64() + 1500;
			hslog::info("mace smash on fighter %d: bonus %.1f (fell %.1f blocks)", handle, smash, json_num(m, "fall", 0.0));
		}
		// Knockback (sword) or Punch (bow): an extra shove of the whole body per level, well short of Minecraft's (which
		// throws a player several blocks): it staggers him back, it doesn't put him down
		if (const double kbl = json_num(m, "kbl", 0.0); kbl > 0.0)
		{
			const double h = std::sqrt(dir.X * dir.X + dir.Y * dir.Y);
			if (h > 1e-3)
			{
				const double shove = kPlayerKnockbackPerLevel * std::min(kbl, 5.0);
				w->Mesh->AddImpulseToAllBodiesBelow(SDK::FVector(dir.X / h * shove, dir.Y / h * shove, 60.0 * std::min(kbl, 5.0)), name(L"pelvis"), true, true);
				hslog::info("%s: knockback %.0f on fighter %d", item.empty() ? kind.c_str() : item.c_str(), kbl, int(json_num(m, "h", -1)));
			}
		}
	}

	/// {"t":"push","h":fighter,"v":[x,y,z],"k":"fishing_rod"}: Minecraft pulls (or pushes) the fighter's proxy, in blocks
	/// per tick: the same change of speed for his whole body. A rod's hard yank (from far off) takes his feet away.
	void on_push(const std::string &m)
	{
		const int handle = int(json_num(m, "h", -1));
		auto *w = fighter_by_handle(handle);
		double v[3];
		if (w == nullptr || w->Mesh == nullptr || json_nums(m, "v", v, 3) != 3)
			return;
		const double speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		const std::string kind = json_str(m, "k");
		const bool rod = kind == "fishing_rod";
		// (wind and a mace's shockwave as Minecraft pushes; a rod's pull stronger: a fighter's balance takes a tug)
		const double scale = (rod ? kRodPull : kWindPush) * kMobKnockback;
		SDK::FVector dv(v[0] * scale, v[2] * scale, std::max(v[1] * scale, rod ? 220.0 : 150.0)); // a little lift at least, or his feet just drag
		if (const double len = std::sqrt(dv.X * dv.X + dv.Y * dv.Y + dv.Z * dv.Z); len > kMaxPush)
			dv = SDK::FVector(dv.X / len * kMaxPush, dv.Y / len * kMaxPush, dv.Z / len * kMaxPush);
		w->Mesh->AddImpulseToAllBodiesBelow(dv, name(L"pelvis"), true, true);
		if (speed > 0.4)
		{
			w->Force_Fall = true;
			g_fallUntil[handle] = GetTickCount64() + 1200;
		}
		hslog::info("%s pushes fighter %d: %.2f blocks/tick", kind.c_str(), handle, speed);
	}

	// ---- fire: a burning fighter panics like one in Half Sword's wasp nest ----
	struct Burning
	{
		ULONGLONG until = 0;    // burning while Minecraft keeps saying so
		ULONGLONG nextSting = 0;
		ULONGLONG since = 0;    // when it caught fire
	};
	std::unordered_map<int, Burning> g_burning; // fighter handle -> its fire
	Ref g_fireActor;
	SDK::UPrimitiveComponent *g_fireSource = nullptr; // what the "stings" come from (the nest's mesh, in Half Sword)

	SDK::UPrimitiveComponent *fire_source(SDK::UWorld *world)
	{
		if (g_fireActor.alive() && g_fireSource != nullptr)
			return g_fireSource;
		g_fireSource = nullptr;
		SDK::FTransform t{};
		t.Rotation.W = 1.0;
		t.Scale3D = SDK::FVector(1, 1, 1);
		t.Translation = SDK::FVector(0.0, 0.0, 50000.0);
		SDK::AActor *a = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world, SDK::AActor::StaticClass(), t,
			SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		if (a == nullptr)
			return nullptr;
		SDK::UGameplayStatics::FinishSpawningActor(a, t, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		auto *sphere = static_cast<SDK::UPrimitiveComponent *>(a->AddComponentByClass(SDK::USphereComponent::StaticClass(), false, t, false));
		if (sphere == nullptr)
			return nullptr;
		sphere->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
		sphere->SetHiddenInGame(true, false);
		g_fireActor = {a, a->Index};
		g_fireSource = sphere;
		return sphere;
	}

	/// {"t":"burn","h":[handles]}: the fighters whose proxies are on fire right now (sent while any are).
	void on_burn(const std::string &m)
	{
		double handles[64];
		const int n = json_nums(m, "h", handles, 64);
		const ULONGLONG now = GetTickCount64();
		for (int i = 0; i < n; ++i)
		{
			Burning &b = g_burning[int(handles[i])];
			if (b.until < now)
			{
				hslog::info("fighter %d is on fire", int(handles[i]));
				b.since = now;
			}
			b.until = now + 600;
		}
	}

	/// Every tick: each burning fighter is "stung" all over, as the wasp nest does to whoever is near it at full
	/// anger: Half Sword's own pain reaction (grabbing at himself, screaming, flailing) follows.
	void burn_fighters(SDK::UWorld *world)
	{
		const ULONGLONG now = GetTickCount64();
		for (auto it = g_burning.begin(); it != g_burning.end();)
		{
			auto *w = fighter_by_handle(it->first);
			if (w == nullptr || now > it->second.until)
			{
				if (w != nullptr)
				{
					hslog::info("fighter %d is no longer on fire", it->first);
					w->Force_Fall = false; // he can get up again
				}
				it = g_burning.erase(it);
				continue;
			}
			Burning &b = it->second;
			++it;
			if (now < b.nextSting || w->Mesh == nullptr || w->Health <= 0.0 || w->DED)
				continue;
			b.nextSting = now + 30 + std::rand() % 50; // (the nest: every 0.01 - 0.1 s)
			SDK::UPrimitiveComponent *source = fire_source(world);
			if (source == nullptr)
				continue;
			static SDK::FName bones[kBodyPoints];
			static bool named = false;
			if (!named)
			{
				named = true;
				for (int i = 0; i < kBodyPoints; ++i)
					bones[i] = name(kBodyBones[i]);
			}
			const SDK::FName bone = bones[std::rand() % kBodyPoints];
			const SDK::FVector at = w->Mesh->GetSocketLocation(bone);
			double damageOut = 0.0;
			w->Get_Damage(SDK::FVector(0, 0, 0), SDK::FVector(0, 0, 0), at, SDK::FVector(0, 0, 0), bone, 250.0, 10.0, true, w->Mesh, 0, true, true, source,
				true, nullptr, 100.0, false, 0.0, &damageOut);
			// and it takes him down: his legs and his senses go while he burns (the panic itself is Half Sword's)
			if (now - b.since > 1500)
			{
				w->Consciousness = std::max(0.0, w->Consciousness - 4.0);
				w->Stamina = std::max(0.0, w->Stamina - 6.0);
				w->Force_Fall = true;
			}
		}
	}

	// ---- potions on the fighters (splash, lingering, tipped arrows: Minecraft applies them to the proxies) ----
	constexpr double kHealthPerPoint = 5.0; // Minecraft's 20 health is a fighter's 100
	constexpr double kPoisonFloor = 30.0;   // poison never kills in Minecraft: it stops here (and doesn't make him give up alone)
	constexpr double kMagicPerPoint = 2.5;  // magic damage: a harming potion bites but doesn't finish him
	constexpr double kMaxHarm = 25.0;       // the most one harming splash takes
	constexpr double kMaxTick = 3.0;        // the most one tick of poison or wither takes

	/// A fighter's health, limbs included, as Half Sword gives it at the start (the class defaults).
	double full_health(int i)
	{
		auto *cdo = g_willieClass != nullptr ? static_cast<SDK::AWillie_BP_C *>(g_willieClass->ClassDefaultObject) : nullptr;
		if (cdo == nullptr)
			return 100.0;
		const double v = i < 0 ? cdo->Health : *stand_in_limbs(cdo, i);
		return v > 0.0 ? v : 100.0;
	}

	/// {"t":"magic","h":fighter,"d":points,"k":"harming|poison|wither","nokill":bool}: Minecraft's magic damage on his
	/// proxy (an instant harming potion, poison and wither ticking), straight off his health.
	void on_magic(const std::string &m)
	{
		const int handle = int(json_num(m, "h", -1));
		auto *w = fighter_by_handle(handle);
		if (w == nullptr || w->DED || w->Health <= 0.0)
			return;
		const double before = w->Health;
		const std::string kind = json_str(m, "k");
		const double points = json_num(m, "d");
		// potions: their own multiplier (/hs potions: /hs dealt is for blows); drowning and freezing aren't potions
		const bool potion = kind == "harming" || kind == "poison" || kind == "wither";
		const double lost = potion ? std::min(points * kMagicPerPoint, kind == "harming" ? kMaxHarm : kMaxTick) * g_cfg.potions : points * kHealthPerPoint;
		const bool nokill = json_bool(m, "nokill");
		w->Health = nokill ? std::max(std::min(before, kPoisonFloor), before - lost) : before - lost;
		// sick: it saps him a little too
		w->Stamina = std::max(0.0, w->Stamina - lost * 0.3);
		hslog::info("%s on fighter %d: %.2f Minecraft damage, health %.1f -> %.1f", kind.c_str(), handle, points, before, w->Health);
		if (w->Health <= 0.0)
			w->Death();
	}

	/// {"t":"heal","h":fighter,"d":points}: Minecraft heals his proxy (instant health, regeneration): his health and
	/// his limbs come back, up to what he started with.
	void on_heal(const std::string &m)
	{
		const int handle = int(json_num(m, "h", -1));
		auto *w = fighter_by_handle(handle);
		if (w == nullptr || w->DED || w->Health <= 0.0)
			return;
		const double add = json_num(m, "d") * kHealthPerPoint;
		const double before = w->Health;
		w->Health = std::min(full_health(-1), w->Health + add);
		for (int i = 0; i < 10; ++i)
			*stand_in_limbs(w, i) = std::min(full_health(i), *stand_in_limbs(w, i) + add);
		w->Bleeding = std::max(0.0, w->Bleeding - add * 0.02);
		w->Consciousness = std::min(100.0, w->Consciousness + add);
		static ULONGLONG said = 0;
		if (GetTickCount64() - said > 1000)
		{
			said = GetTickCount64();
			hslog::info("fighter %d healed: health %.1f -> %.1f", handle, before, w->Health);
		}
	}

	constexpr double kWaterLift = 490.0;    // cm/s2 up on a fighter wholly in water (half of gravity: he still sinks, slowly)
	constexpr double kLiquidCurrent = 650.0; // cm/s2 along the current, in it to the hips (/hs current scales it)
	constexpr double kFrozenStaminaDrain = 25.0; // stamina a second, frozen through
	constexpr double kWaterDrag = 2.5;      // of his body's speed, the share lost per second (all under)
	constexpr double kSnowDrag = 9.0;       // powder snow: nearly held
	constexpr double kLavaDrag = 5.0;

	/// What Minecraft's effects do to a fighter while they last (levels from 1; 0 = none).
	struct Effects
	{
		int slowness = 0, speed = 0, strength = 0, weakness = 0;
		bool invisible = false;
		ULONGLONG until = 0;                         // they last while Minecraft keeps saying so
		std::vector<Ref> hidden;                      // the parts of his body made invisible
		float dilation = 1.0f;                        // what was set on him
	};
	std::unordered_map<int, Effects> g_effects; // fighter handle -> his effects

	/// How much of a fighter's body is in Minecraft's water, powder snow and lava (0..1), and the water's current there.
	struct Surroundings
	{
		double water = 0.0, snow = 0.0, lava = 0.0, flowX = 0.0, flowZ = 0.0, frozen = 0.0;
		bool drowning = false;
		ULONGLONG until = 0;
		float walkSpeed = 0.0f; // his own walking speed (what's put back when he's out)
		bool choking = false;   // his throat set held (drowning): let go when he's out
	};

	/// Drowning is Half Sword's own choking: his throat "held" (Willie_BP eases Strangle_Rate up from it, which lowers
	/// his consciousness's cap and builds Choke_To_Death_Rate: he blacks out, then dies of it), or let go.
	void set_choking(SDK::AWillie_BP_C *w, Surroundings &s, bool on)
	{
		if (on == s.choking)
			return;
		w->My_Throat_Is_Held_R = on;
		w->My_Throat_Is_Held_L = on;
		s.choking = on;
	}
	std::unordered_map<int, Surroundings> g_surroundings; // fighter handle -> what he's in

	/// {"t":"env","f":[handle, water, snow, lava, flow x, flow z, drowning, frozen, ...]} (Minecraft's blocks, every 2
	/// ticks while any is in some; an empty list once after).
	void on_env(const std::string &m)
	{
		const ULONGLONG now = GetTickCount64();
		double v[1024];
		const int n = json_nums(m, "f", v, 1024);
		for (int i = 0; i + 7 < n; i += 8)
		{
			Surroundings &s = g_surroundings[int(v[i])];
			if (s.until < now)
				hslog::info("fighter %d in Minecraft's %s", int(v[i]), v[i + 3] > 0.0 ? "lava" : v[i + 1] > 0.0 ? "water" : "powder snow");
			s.water = v[i + 1];
			s.snow = v[i + 2];
			s.lava = v[i + 3];
			s.flowX = v[i + 4];
			s.flowZ = v[i + 5];
			if ((v[i + 6] != 0.0) != s.drowning)
				hslog::info("fighter %d %s", int(v[i]), v[i + 6] != 0.0 ? "is out of air: choking" : "breathes again");
			s.drowning = v[i + 6] != 0.0;
			s.frozen = v[i + 7];
			s.until = now + 400;
		}
	}

	/// Every tick: what Minecraft's water, powder snow and lava do to the fighters in them. He walks slower (water a
	/// half, lava more, powder snow nearly holds him), his body drags in it (its speed bleeds away), water carries him a
	/// little and its current pushes him. (Burning and drowning are Minecraft's: "burn", "magic".)
	void apply_surroundings()
	{
		const ULONGLONG now = GetTickCount64();
		static ULONGLONG last = now;
		const double dt = std::min(0.1, (now - last) / 1000.0);
		last = now;
		for (auto it = g_surroundings.begin(); it != g_surroundings.end();)
		{
			Surroundings &s = it->second;
			auto *w = fighter_by_handle(it->first);
			if (w == nullptr || now > s.until || w->DED)
			{
				if (w != nullptr && s.walkSpeed > 0.0f && w->CharacterMovement != nullptr)
					w->CharacterMovement->MaxWalkSpeed = s.walkSpeed;
				if (w != nullptr)
					set_choking(w, s, false);
				it = g_surroundings.erase(it);
				continue;
			}
			++it;
			set_choking(w, s, s.drowning);
			// frozen through: the cold saps him (slow, heavy, out of breath)
			if (s.frozen >= 1.0)
				w->Stamina = std::max(0.0, w->Stamina - kFrozenStaminaDrain * dt);
			const double snow = std::min(1.0, s.snow * 2.0); // (sunk to the knees is held fast already)
			const double slow = (1.0 - 0.5 * s.water) * (1.0 - 0.8 * snow) * (1.0 - 0.7 * std::min(1.0, s.lava * 2.0));
			if (w->CharacterMovement != nullptr)
			{
				if (s.walkSpeed <= 0.0f)
					s.walkSpeed = w->CharacterMovement->MaxWalkSpeed;
				w->CharacterMovement->MaxWalkSpeed = float(s.walkSpeed * slow);
			}
			if (w->Mesh == nullptr || dt <= 0.0)
				continue;
			// drag: this share of his body's speed goes each second (per second, not per frame)
			const double drag = kWaterDrag * s.water + kSnowDrag * snow + kLavaDrag * s.lava;
			const SDK::FVector v = w->Mesh->GetPhysicsLinearVelocity(name(L"pelvis"));
			const double keep = std::exp(-drag * dt);
			SDK::FVector change(-v.X * (1.0 - keep), -v.Y * (1.0 - keep), -v.Z * (1.0 - keep) * 0.5);
			// water carries him a little (half his weight, all under); a current (water's or lava's) pushes him along
			const double current = kLiquidCurrent * g_cfg.current * std::min(1.0, (s.water + s.lava) * 2.0);
			change.X += s.flowX * current * dt;
			change.Y += s.flowZ * current * dt;
			change.Z += kWaterLift * s.water * dt;
			w->Mesh->AddImpulseToAllBodiesBelow(change, name(L"pelvis"), true, true);
		}
	}

	/// Minecraft stops driving: the fighters walk at their own speed again.
	void clear_surroundings()
	{
		for (auto &[handle, s] : g_surroundings)
			if (auto *w = fighter_by_handle(handle); w != nullptr)
			{
				if (s.walkSpeed > 0.0f && w->CharacterMovement != nullptr)
					w->CharacterMovement->MaxWalkSpeed = s.walkSpeed;
				set_choking(w, s, false);
			}
		g_surroundings.clear();
	}

	/// How hard a fighter hits, by his effects: Minecraft's Strength (+3 a level on a 1-point fist, here a share) and
	/// Weakness (-4 a level).
	double effect_damage(const SDK::AWillie_BP_C *w)
	{
		if (w == nullptr)
			return 1.0;
		auto it = g_effects.find(w->Index);
		if (it == g_effects.end())
			return 1.0;
		return std::clamp(1.0 + 0.3 * it->second.strength - 0.4 * it->second.weakness, 0.15, 3.0);
	}

	/// {"t":"fx","slow":[h,level,...],"speed":[...],"str":[...],"weak":[...],"invis":[h,...]}: the fighters whose
	/// proxies have these effects now (sent every half second while any do; an empty one when the last ends).
	void on_fx(const std::string &m)
	{
		const ULONGLONG now = GetTickCount64();
		std::unordered_map<int, Effects> next;
		double v[128];
		auto levels = [&](const char *key, int Effects::*field) {
			const int n = json_nums(m, key, v, 128);
			for (int i = 0; i + 1 < n; i += 2)
				next[int(v[i])].*field = int(v[i + 1]);
		};
		levels("slow", &Effects::slowness);
		levels("speed", &Effects::speed);
		levels("str", &Effects::strength);
		levels("weak", &Effects::weakness);
		const int n = json_nums(m, "invis", v, 128);
		for (int i = 0; i < n; ++i)
			next[int(v[i])].invisible = true;
		for (auto &[handle, e] : next)
		{
			Effects &cur = g_effects[handle];
			if (cur.until == 0)
				hslog::info("fighter %d: slowness %d, speed %d, strength %d, weakness %d%s", handle, e.slowness, e.speed, e.strength, e.weakness,
					e.invisible ? ", invisible" : "");
			cur.slowness = e.slowness;
			cur.speed = e.speed;
			cur.strength = e.strength;
			cur.weakness = e.weakness;
			cur.invisible = e.invisible;
			cur.until = now + 1500;
		}
		// the ones not in the list are over (expiry does the rest)
		for (auto &[handle, e] : g_effects)
			if (!next.count(handle))
				e.until = std::min(e.until, now);
	}

	void show_body(Effects &e)
	{
		for (const Ref &r : e.hidden)
			if (r.alive())
				static_cast<SDK::USceneComponent *>(r.obj)->SetVisibility(true, false);
		e.hidden.clear();
	}

	/// Every tick: the effects on the fighters. Slowness and Speed: Half Sword's own time for him (his steps and his
	/// swings, slower or quicker); Invisibility: his body unseen (what he wears and carries still shows, as in
	/// Minecraft); Strength and Weakness: his blows (effect_damage).
	void apply_effects()
	{
		const ULONGLONG now = GetTickCount64();
		static SDK::UClass *skinned = SDK::USkeletalMeshComponent::StaticClass();
		for (auto it = g_effects.begin(); it != g_effects.end();)
		{
			Effects &e = it->second;
			auto *w = fighter_by_handle(it->first);
			if (w == nullptr || now > e.until)
			{
				if (w != nullptr)
				{
					w->CustomTimeDilation = 1.0f;
					hslog::info("fighter %d: effects over", it->first);
				}
				show_body(e);
				it = g_effects.erase(it);
				continue;
			}
			// Minecraft: Speed +20% a level, Slowness -15% a level
			const float dilation = float(std::clamp(1.0 + 0.2 * e.speed - 0.15 * e.slowness, 0.3, 1.8));
			if (std::fabs(w->CustomTimeDilation - dilation) > 0.001f)
				w->CustomTimeDilation = dilation;
			if (e.invisible && e.hidden.empty() && !w->DED)
			{
				SDK::TArray<SDK::UActorComponent *> parts = w->K2_GetComponentsByClass(skinned);
				for (int i = 0; i < parts.Num(); ++i)
				{
					auto *c = static_cast<SDK::USceneComponent *>(parts[i]);
					if (c != nullptr && c->IsVisible())
					{
						c->SetVisibility(false, false);
						e.hidden.push_back({c, c->Index});
					}
				}
			}
			else if (!e.invisible && !e.hidden.empty())
				show_body(e);
			++it;
		}
	}

	/// Minecraft stops driving: the fighters as Half Sword has them (their own time, seen).
	void clear_effects()
	{
		for (auto &[handle, e] : g_effects)
		{
			if (auto *w = fighter_by_handle(handle); w != nullptr)
				w->CustomTimeDilation = 1.0f;
			show_body(e);
		}
		g_effects.clear();
		clear_surroundings();
	}


	constexpr double kLightningHealth = 30.0; // a bolt's damage off his health

	/// A Minecraft explosion: a blunt blast through Half Sword's hit pipeline for every fighter in reach, and a shove
	/// (/hs explosions: how hard).
	void on_explosion(const std::string &m)
	{
		double c[3];
		if (json_nums(m, "pos", c, 3) != 3)
			return;
		const double r = json_num(m, "r", 4.0), reach = r * 2.5;
		const SDK::FVector centre = to_ue(c[0], c[1], c[2]);
		for (const Ref &ref : g_fighters)
		{
			auto *w = ref.alive() ? as_willie(ref.obj) : nullptr;
			if (w == nullptr || w->Mesh == nullptr)
				continue;
			const SDK::FVector at = w->Mesh->GetSocketLocation(name(L"spine_03"));
			SDK::FVector d(at.X - centre.X, at.Y - centre.Y, at.Z - centre.Z);
			const double dist = std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z) / 100.0;
			if (dist >= reach)
				continue;
			const double impact = 1.0 - dist / reach;
			d.Z += 60.0;
			SDK::UPrimitiveComponent *by = g_standIn.ref.alive() ? as_willie(g_standIn.ref.obj)->Mesh : w->Mesh;
			// the blast on the body, and again on the head and a leg (a blast hits all of him)
			strike_fighter(w, name(L"spine_03"), at, d, 1200.0 + 4200.0 * impact, 0.0, 0.0, 1.0, 2, true, by, nullptr, "explosion");
			if (impact > 0.35)
			{
				strike_fighter(w, name(L"Head"), w->Mesh->GetSocketLocation(name(L"Head")), d, 900.0 + 3000.0 * impact, 0.0, 0.0, 1.0, 1, true, by, nullptr,
					"explosion");
				strike_fighter(w, name(L"thigh_l"), w->Mesh->GetSocketLocation(name(L"thigh_l")), d, 900.0 + 3000.0 * impact, 0.0, 0.0, 1.0, 1, true, by,
					nullptr, "explosion");
			}
			// and it throws him: off his feet, up and away, every part of him
			const double push = 1500.0 * impact * g_cfg.explosions;
			const double len = std::max(1e-3, std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z));
			w->Mesh->AddImpulseToAllBodiesBelow(SDK::FVector(d.X / len * push, d.Y / len * push, d.Z / len * push + 800.0 * impact * g_cfg.explosions), name(L"pelvis"), true,
				true);
			w->Force_Fall = true;
			g_fallUntil[ref.index] = GetTickCount64() + 1500;
		}
	}

	/// {"t":"lightning","h":fighter}: a bolt strikes him (Minecraft sets him on fire too: the fire's panic follows). It
	/// goes through him from the head down, takes a good part of his health, his legs and nearly his senses.
	void on_lightning(const std::string &m)
	{
		const int handle = int(json_num(m, "h", -1));
		auto *w = fighter_by_handle(handle);
		if (w == nullptr || w->Mesh == nullptr || w->DED || w->Health <= 0.0)
			return;
		SDK::UPrimitiveComponent *by = g_standIn.ref.alive() ? as_willie(g_standIn.ref.obj)->Mesh : w->Mesh;
		const SDK::FName head = name(L"Head");
		strike_fighter(w, head, w->Mesh->GetSocketLocation(head), SDK::FVector(0, 0, -1), 2600.0, 0.0, 0.0, 1.0, 2, true, by, nullptr, "lightning");
		const double before = w->Health;
		w->Health -= kLightningHealth * g_cfg.dealt;
		w->Stamina = 0.0;
		w->Consciousness = std::max(0.0, w->Consciousness - 60.0);
		w->Force_Fall = true;
		g_fallUntil[handle] = GetTickCount64() + 2500;
		hslog::info("lightning on fighter %d: health %.1f -> %.1f", handle, before, w->Health);
		if (w->Health <= 0.0)
			w->Death();
	}

	void on_config(const std::string &m)
	{
		g_cfg.dealt = json_num(m, "dealt", 1.0);
		g_cfg.taken = json_num(m, "taken", 1.0);
		g_cfg.potions = json_num(m, "potions", 1.0);
		for (int p = 0; p < kParts; ++p)
			g_cfg.part[p] = json_num(m, kPartNames[p], 1.0);
		g_cfg.fps = int(json_num(m, "fps", 0.0));
		g_cfg.balance = json_num(m, "balance", 0.0) != 0.0;
		g_cfg.props = json_num(m, "props", 1.0);
		g_cfg.current = json_num(m, "current", 1.0);
		g_cfg.explosions = json_num(m, "explosions", 1.0);
		g_cfg.mace = json_num(m, "mace", 1.0);
		hslog::info("damage settings: dealt x%.2f taken x%.2f potions x%.2f head %.2f neck %.2f torso %.2f arms %.2f legs %.2f", g_cfg.dealt, g_cfg.taken, g_cfg.potions,
			g_cfg.part[kHead], g_cfg.part[kNeck], g_cfg.part[kTorso], g_cfg.part[kArms], g_cfg.part[kLegs]);
	}

	// ---- the stand-in (the player's own fighter) ----
	double *stand_in_limbs(SDK::AWillie_BP_C *w, int i)
	{
		double *limbs[10] = {&w->Head_Health, &w->Body_Upper_Health, &w->Body_Lower_Health, &w->Arm_R_Health, &w->Arm_L_Health,
			&w->Leg_R_Health, &w->Leg_L_Health, &w->Neck_Health, &w->Back_Health, &w->Consciousness_Cap};
		return limbs[i];
	}

	/// The hit events of the fighters' weapons and bodies (once their classes are loaded): a weapon's head and blade
	/// parts strike, its pommel, guard and grip only knock.
	void find_hit_events()
	{
		if (!g_hitEvents.empty())
			return;
		static SDK::UClass *weaponClass = nullptr;
		if (weaponClass == nullptr)
			weaponClass = SDK::UObject::FindClassFast("ModularWeaponBP_C");
		if (weaponClass == nullptr || g_willieClass == nullptr)
			return;
		for (SDK::UClass *c : {weaponClass, g_willieClass})
			for (SDK::UField *f = c->Children; f != nullptr; f = f->Next)
			{
				if (!f->IsA(SDK::EClassCastFlags::Function))
					continue;
				const std::string n = f->GetName();
				HitKind kind;
				if (n.find("ComponentBeginOverlapSignature") != std::string::npos)
					kind = HitKind::Overlap;
				else if (n.find("ComponentHitSignature") == std::string::npos)
					continue;
				else if (c == g_willieClass)
					kind = HitKind::Body;
				else if (n.find("Pommel") != std::string::npos || n.find("Guard") != std::string::npos || n.find("Grip") != std::string::npos)
					kind = HitKind::WeaponBlunt;
				else
					kind = HitKind::Weapon;
				g_hitEvents.push_back({static_cast<const SDK::UFunction *>(f), kind});
			}
		hslog::info("hit events: %d", int(g_hitEvents.size()));
	}

	/// The hardest of the fighters' blows on a body this frame, in Minecraft damage (points), and who struck it.
	struct BestHit
	{
		const SteveHit *hit = nullptr;
		double points = 0, speed = 0, mass = 0;
		SDK::AWillie_BP_C *attacker = nullptr;
	};

	BestHit score_hits(const std::vector<SteveHit> &hits, SDK::AWillie_BP_C *w, bool steve)
	{
		BestHit best;
		// Steve up high (on blocks, mid-air) while his stand-in is down on the ground (Half Sword puts it there between
		// frames): a blow on it then lands where he isn't
		if (false && steve && g_steve.valid) // (off: the 17:43 build had no such check)
		{
			const double above = to_ue(g_steve.x, g_steve.y, g_steve.z).Z - feet_of(w).Z;
			if (above > kOutOfReachAbove)
			{
				static ULONGLONG said = 0;
				if (GetTickCount64() - said > 2000)
				{
					said = GetTickCount64();
					hslog::info("Steve is %.0f cm above his stand-in (it's on the ground below him): its blows don't count", above);
				}
				return best;
			}
		}
		for (const SteveHit &h : hits)
		{
			if (!h.by.alive())
				continue;
			auto *c = static_cast<SDK::UPrimitiveComponent *>(h.by.obj);
			const bool body = h.kind == HitKind::Body;
			if (body)
			{
				// a fighter's own body: only fists and feet strike (a shoulder bumping into it doesn't)
				const std::string b = h.byBone.ToString();
				if (b.find("hand") == std::string::npos && b.find("lowerarm") == std::string::npos && b.find("foot") == std::string::npos &&
					b.find("calf") == std::string::npos)
					continue;
			}
			// a fighter well below (Steve up on blocks, them at the foot) can't reach, as in Minecraft: only what the
			// fighter's arms could really put there counts
			SDK::AActor *owner = c->GetOwner();
			auto *attacker = body ? as_willie(owner) : nullptr;
			static SDK::UClass *weaponClass = SDK::UObject::FindClassFast("ModularWeaponBP_C");
			if (!body && owner != nullptr && weaponClass != nullptr && owner->IsA(weaponClass))
				attacker = static_cast<SDK::AModularWeaponBP_C *>(owner)->Parent_Actor;
			// only a fighter that's still there (a weapon's owner from a fight before may be gone: no raw pointers)
			if (attacker != nullptr)
			{
				bool known = false;
				for (const Ref &r : g_fighters)
					known = known || (r.obj == attacker && r.alive());
				if (!known)
					attacker = nullptr;
			}
			// whose it is, when the weapon doesn't say (dropped, loose, another kind): the fighter nearest the blow
			if (attacker == nullptr)
			{
				double nearest = 1e30;
				for (const Ref &r : g_fighters)
				{
					auto *f = r.alive() ? as_willie(r.obj) : nullptr;
					if (f == nullptr || f == w)
						continue;
					const SDK::FVector p = f->K2_GetActorLocation();
					const double d = (p.X - h.at.X) * (p.X - h.at.X) + (p.Y - h.at.Y) * (p.Y - h.at.Y) + (p.Z - h.at.Z) * (p.Z - h.at.Z);
					if (d < nearest)
					{
						nearest = d;
						attacker = f;
					}
				}
			}
			// the blow has to land on the body where it really is (Steve's, a mob's: the capsule): one somewhere else is
			// a stale or stray contact (a hidden body moving fast, a weapon flying loose), not a hit on him
			{
				const SDK::FVector c = w->K2_GetActorLocation();
				// (a mob's body is scaled to the mob)
				const double half = w->CapsuleComponent != nullptr ? w->CapsuleComponent->GetScaledCapsuleHalfHeight() : 90.0;
				const double reach = w->CapsuleComponent != nullptr ? w->CapsuleComponent->GetScaledCapsuleRadius() + 100.0 : 135.0;
				const double up = std::abs(h.at.Z - c.Z), side = std::sqrt((h.at.X - c.X) * (h.at.X - c.X) + (h.at.Y - c.Y) * (h.at.Y - c.Y));
				if (up > half + 60.0 || side > reach)
				{
					static ULONGLONG said = 0;
					if (GetTickCount64() - said > 1000)
					{
						said = GetTickCount64();
						hslog::info("%s: a blow %.0f cm off the body (%.0f up/down, %.0f aside): not a hit", steve ? "Steve" : "a mob", std::sqrt(up * up + side * side),
							up, side);
					}
					continue;
				}
			}
			// and his real body (pelvis, chest: not the capsule, which Minecraft's blocks under Steve can shove up beside
			// him while the fighter stays below) has to be able to put it there: not far over his hips, not past his
			// arm and weapon
			if (attacker != nullptr && attacker->Mesh != nullptr)
			{
				const SDK::FVector pelvis = attacker->Mesh->GetSocketLocation(name(L"pelvis"));
				const SDK::FVector chest = attacker->Mesh->GetSocketLocation(name(L"spine_05"));
				const double over = h.at.Z - pelvis.Z;
				const double away = std::sqrt((h.at.X - chest.X) * (h.at.X - chest.X) + (h.at.Y - chest.Y) * (h.at.Y - chest.Y) +
					(h.at.Z - chest.Z) * (h.at.Z - chest.Z));
				if (over > kReachOverPelvis || away > kReachFromChest)
				{
					static ULONGLONG said = 0;
					if (GetTickCount64() - said > 1000)
					{
						said = GetTickCount64();
						hslog::info("%s out of %s's reach: the blow %.0f cm over his pelvis, %.0f cm from his chest (capsule feet %.0f cm, pelvis %.0f cm "
							"below the body's feet): no damage", steve ? "Steve" : "a mob", attacker->GetName().c_str(), over, away,
							feet_of(w).Z - feet_of(attacker).Z, feet_of(w).Z - pelvis.Z);
					}
					continue;
				}
			}
			if (attacker != nullptr && feet_of(w).Z - feet_of(attacker).Z > kOutOfReachAbove)
			{
				static ULONGLONG said = 0;
				if (GetTickCount64() - said > 1000)
				{
					said = GetTickCount64();
					hslog::info("%s out of reach above %s (%.0f cm up): no damage", steve ? "Steve" : "a mob", attacker->GetName().c_str(),
						feet_of(w).Z - feet_of(attacker).Z);
				}
				continue;
			}
			const SDK::FVector v = c->GetPhysicsLinearVelocityAtPoint(h.at, body ? h.byBone : SDK::FName{});
			const double speed = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
			const double mass = body ? 1.0 : c->GetMass();
			double points = std::clamp((speed - kHitMinSpeed) / kHitSpeedPerPoint, 0.0, kHitMaxPoints);
			points *= body ? 0.35 : h.kind == HitKind::WeaponBlunt ? 0.5 : std::clamp(std::sqrt(std::max(mass, 0.1) / 1.0), 0.6, 1.8);
			const Part part = part_of(h.bone.ToString());
			points *= part == kHead || part == kNeck ? 1.5 : part == kTorso ? 1.0 : 0.7;
			if (steve)
				points *= g_cfg.taken * g_cfg.part[part];
			points *= effect_damage(attacker); // his Strength or Weakness
			if (points > best.points)
			{
				best.points = points;
				best.hit = &h;
				best.speed = speed;
				best.mass = mass;
				best.attacker = attacker;
			}
		}
		return best;
	}

	/// The fighters' hits on Steve since the last frame -> one Minecraft hit ("hurt"), the hardest of them, sent now.
	/// Minecraft's own hurt cooldown does the rest: a blow touching for several frames counts once (more only if a
	/// later contact is harder), with one knockback.
	void send_steve_hits(SDK::AWillie_BP_C *w)
	{
		if (g_steveHits.empty())
			return;
		const BestHit best = score_hits(g_steveHits, w, true);
		if (best.hit != nullptr && best.points >= 0.3)
		{
			// knockback from the nearest fighter
			double fx = g_steve.x, fy = g_steve.y, fz = g_steve.z, nearest = 1e30;
			const SDK::FVector me = w->K2_GetActorLocation();
			for (const Ref &r : g_fighters)
			{
				auto *f = r.alive() ? as_willie(r.obj) : nullptr;
				if (f == nullptr)
					continue;
				const SDK::FVector p = f->K2_GetActorLocation();
				const double d = (p.X - me.X) * (p.X - me.X) + (p.Y - me.Y) * (p.Y - me.Y);
				if (d < nearest)
				{
					nearest = d;
					to_mc(p, fx, fy, fz);
				}
			}
			sendf("{\"t\":\"hurt\",\"d\":%.2f,\"from\":[%.3f,%.3f,%.3f]}", best.points, fx, fy, fz);
			const bool hasBody = best.attacker != nullptr && best.attacker->Mesh != nullptr;
			{
				// where everything was, in Minecraft's y: Steve (Minecraft's word), the stand-in, the blow, the striker
				double x, z, standInY, blowY, capY = 0.0, pelvisY = 0.0;
				to_mc(feet_of(w), x, standInY, z);
				to_mc(best.hit->at, x, blowY, z);
				if (best.attacker != nullptr)
					to_mc(feet_of(best.attacker), x, capY, z);
				if (hasBody)
					to_mc(best.attacker->Mesh->GetSocketLocation(name(L"pelvis")), x, pelvisY, z);
				hslog::info("  heights (y): Steve %.2f, stand-in feet %.2f, blow %.2f, his capsule feet %.2f, his pelvis %.2f", g_steve.y, standInY, blowY,
					capY, pelvisY);
			}
			hslog::info("Steve hit in the %s (%s) by %s of %s: %.0f cm/s, %.1f kg -> %.2f Minecraft damage (his capsule %.0f cm below, the blow %.0f cm "
				"over his pelvis)",
				kPartNames[part_of(best.hit->bone.ToString())], best.hit->bone.ToString().c_str(), static_cast<SDK::UObject *>(best.hit->by.obj)->GetName().c_str(),
				best.attacker != nullptr ? best.attacker->GetName().c_str() : "nobody", best.speed, best.mass, best.points,
				best.attacker != nullptr ? feet_of(w).Z - feet_of(best.attacker).Z : 0.0,
				hasBody ? best.hit->at.Z - best.attacker->Mesh->GetSocketLocation(name(L"pelvis")).Z : 0.0);
		}
		g_steveHits.clear();
	}

	void hide_actor(SDK::AActor *a, bool hidden)
	{
		if (a == nullptr)
			return;
		a->SetActorHiddenInGame(hidden);
	}

	void stand_in_weapons(SDK::AWillie_BP_C *w, bool hidden)
	{
		if (!hidden)
		{
			for (const Ref &r : g_carried)
				if (r.alive())
					hide_carried(static_cast<SDK::AActor *>(r.obj), false);
			g_carried.clear();
		}
		g_fightersRefresh = 0; // hidden: found by the next scan
	}

	// ---- night vision: Minecraft's, on Steve, lights Half Sword's own picture through its own exposure (on our
	// camera), the shadows lifted and the highlights held back (local exposure), so the dark shows its detail. A map
	// with a fixed exposure (East Tower's night) gets more of it; one with automatic exposure (Ambush's night: kept dark
	// by a floor under how much it may brighten) gets that floor lowered instead - more exposure on top of an automatic
	// one turned the whole picture white ----
	float g_nightVision = 0.0f; // 0..1 ("me": Minecraft's own strength, flickering as it runs out)
	bool g_nightVisionOn = false;
	struct MapExposure
	{
		bool known = false;
		bool manual = false;                                    // fixed exposure (else automatic)
		float bias = 1.0f, minBright = 0.03f, shadows = 1.0f, highlights = 1.0f; // the map's own (its strongest unbound volume's)
	} g_mapExposure;

	MapExposure map_exposure(SDK::UWorld *world)
	{
		MapExposure e;
		e.known = true;
		SDK::TArray<SDK::AActor *> found;
		SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::APostProcessVolume::StaticClass(), &found);
		const SDK::APostProcessVolume *best = nullptr;
		for (int i = 0; i < found.Num(); ++i)
		{
			const auto *v = static_cast<const SDK::APostProcessVolume *>(found[i]);
			if (v != nullptr && v->bEnabled && v->bUnbound && (best == nullptr || v->Priority > best->Priority))
				best = v;
		}
		if (best != nullptr)
		{
			const SDK::FPostProcessSettings &s = best->Settings;
			if (s.bOverride_AutoExposureMethod)
				e.manual = s.AutoExposureMethod == SDK::EAutoExposureMethod::AEM_Manual;
			if (s.bOverride_AutoExposureBias)
				e.bias = s.AutoExposureBias;
			if (s.bOverride_AutoExposureMinBrightness)
				e.minBright = s.AutoExposureMinBrightness;
			if (s.bOverride_LocalExposureShadowContrastScale)
				e.shadows = s.LocalExposureShadowContrastScale;
			if (s.bOverride_LocalExposureHighlightContrastScale)
				e.highlights = s.LocalExposureHighlightContrastScale;
		}
		hslog::info("night vision: the map's exposure %s, bias %.2f, floor %.3f, shadows %.2f, highlights %.2f (%d volumes)", e.manual ? "fixed" : "automatic",
			e.bias, e.minBright, e.shadows, e.highlights, found.Num());
		return e;
	}

	void apply_night_vision(SDK::UWorld *world, SDK::ACameraActor *cam)
	{
		if (cam->CameraComponent == nullptr)
			return;
		SDK::FPostProcessSettings &s = cam->CameraComponent->PostProcessSettings;
		const float nv = std::clamp(g_nightVision, 0.0f, 1.0f);
		if (nv <= 0.0f)
		{
			if (g_nightVisionOn)
			{
				s.bOverride_AutoExposureBias = 0;
				s.bOverride_AutoExposureMinBrightness = 0;
				s.bOverride_LocalExposureShadowContrastScale = 0;
				s.bOverride_LocalExposureHighlightContrastScale = 0;
				g_nightVisionOn = false;
			}
			return;
		}
		if (!g_mapExposure.known)
			g_mapExposure = map_exposure(world);
		const MapExposure &e = g_mapExposure;
		if (e.manual)
		{
			s.bOverride_AutoExposureBias = 1;
			s.AutoExposureBias = e.bias + 2.0f * nv;
		}
		else
		{
			// the floor down (in steps of light: 0.5 -> 0.03 is 4 stops), so a dark scene may be brightened to normal;
			// a bright one isn't touched
			s.bOverride_AutoExposureMinBrightness = 1;
			const float floor = std::min(e.minBright, 0.03f);
			s.AutoExposureMinBrightness = std::exp(std::log(e.minBright) + (std::log(floor) - std::log(e.minBright)) * nv);
		}
		const float shadows = e.manual ? 0.3f : 0.5f, highlights = e.manual ? 0.45f : 0.6f;
		s.bOverride_LocalExposureShadowContrastScale = 1;
		s.LocalExposureShadowContrastScale = std::min(e.shadows, e.shadows + (shadows - e.shadows) * nv);
		s.bOverride_LocalExposureHighlightContrastScale = 1;
		s.LocalExposureHighlightContrastScale = std::min(e.highlights, e.highlights + (highlights - e.highlights) * nv);
		g_nightVisionOn = true;
	}

	/// Half Sword looks through a camera of ours, placed where Minecraft's camera is: as the view target (for the
	/// next camera update) and written straight into this frame's camera cache, which the renderer reads after us.
	void apply_camera(SDK::UWorld *world, SDK::APlayerController *pc, SDK::APlayerCameraManager *cm)
	{
		if (!g_camera.alive())
		{
			SDK::FTransform t{};
			t.Rotation.W = 1.0;
			t.Translation = g_camLoc;
			t.Scale3D = SDK::FVector(1, 1, 1);
			SDK::AActor *a = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world, SDK::ACameraActor::StaticClass(), t,
				SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
			if (a == nullptr)
				return;
			SDK::UGameplayStatics::FinishSpawningActor(a, t, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
			g_camera = {a, a->Index};
			g_mapExposure = {}; // (a new camera: a new map, maybe)
			g_nightVisionOn = false;
			hslog::info("camera actor %s", a->GetName().c_str());
		}
		auto *cam = static_cast<SDK::ACameraActor *>(g_camera.obj);
		SDK::FHitResult hit{};
		cam->K2_SetActorLocationAndRotation(g_camLoc, g_camRot, false, &hit, true);
		if (cam->CameraComponent != nullptr)
			cam->CameraComponent->SetFieldOfView(g_camFov);
		apply_night_vision(world, cam);
		if (cm->ViewTarget.Target != cam)
			pc->SetViewTargetWithBlend(cam, 0.0f, SDK::EViewTargetBlendFunction::VTBlend_Linear, 0.0f, false);
	}

	void release_camera(SDK::APlayerController *pc, SDK::APawn *pawn)
	{
		if (pc != nullptr && pawn != nullptr && pc->PlayerCameraManager != nullptr && g_camera.alive() &&
			pc->PlayerCameraManager->ViewTarget.Target == g_camera.obj)
			pc->SetViewTargetWithBlend(pawn, 0.0f, SDK::EViewTargetBlendFunction::VTBlend_Linear, 0.0f, false);
	}

	void restore_stand_in()
	{
		if (!g_standIn.ref.alive() || !g_standIn.hidden)
			return;
		auto *w = as_willie(g_standIn.ref.obj);
		w->Health = g_standIn.health;
		for (int i = 0; i < 10; ++i)
			*stand_in_limbs(w, i) = g_standIn.limbs[i];
		hide_actor(w, false);
		// on something solid first: it stood wherever Steve was, and Steve can be a little in Half Sword's ground or in
		// a block's box - physics coming back on in there flung the body out, often to its death. The first solid
		// thing under the hips (a block's box too: Steve may stand on one)
		const SDK::FVector at = w->K2_GetActorLocation();
		const double half = w->CapsuleComponent != nullptr ? w->CapsuleComponent->CapsuleHalfHeight : 90.0;
		double groundZ = 0.0;
		if (g_world != nullptr && ground_z(g_world, at.X, at.Y, at.Z + 50.0, groundZ, true) && groundZ > at.Z - half - 300.0)
		{
			SDK::FHitResult hit{};
			w->K2_SetActorLocation(SDK::FVector(at.X, at.Y, groundZ + half + 5.0), false, &hit, true);
			// and its feet out of the ground: the pinned pose's feet can hang below the capsule, and legs in the ground
			// when physics came back were shoved out (a broken leg)
			double low = 1e30;
			if (w->Mesh != nullptr)
				for (const wchar_t *b : {L"foot_l", L"foot_r", L"ball_l", L"ball_r", L"calf_l", L"calf_r"})
					low = std::min(low, w->Mesh->GetSocketLocation(name(b)).Z);
			// (not lowered when they hang over it: frozen in the air, its legs are drawn up; they come down while it
			// walks again before its physics (settle_restored), and lowered first they came down into the ground)
			double lift = 0.0;
			const double footBefore = low;
			if (low < 1e29 && low < groundZ + 2.0)
			{
				lift = groundZ + 4.0 - low;
				const SDK::FVector now = w->K2_GetActorLocation();
				w->K2_SetActorLocation(SDK::FVector(now.X, now.Y, now.Z + lift), false, &hit, true);
			}
			const double pelvisZ = w->Mesh != nullptr ? w->Mesh->GetSocketLocation(name(L"pelvis")).Z : 0.0;
			auto *cdo = static_cast<SDK::AWillie_BP_C *>(g_willieClass->ClassDefaultObject);
			hslog::info("stand-in back on the ground: %.0f cm %s; its lowest foot was %.0f cm %s the ground, moved %+.0f; hips %.0f cm over the ground; "
				"fallen %d, crouched %d, kneeling %d/%d; body on its capsule at %.0f (a fresh one's %.0f), hanging from %s",
				std::fabs(groundZ + half + 5.0 - at.Z), groundZ + half + 5.0 > at.Z ? "up" : "down", std::fabs(footBefore - groundZ),
				footBefore >= groundZ ? "over" : "in", lift, pelvisZ - groundZ, int(w->Fallen), int(w->Is_Crouched), int(w->L_Kneel), int(w->R_Kneel),
				w->Mesh != nullptr ? w->Mesh->RelativeLocation.Z : 0.0, cdo != nullptr && cdo->Mesh != nullptr ? cdo->Mesh->RelativeLocation.Z : 0.0,
				w->Mesh != nullptr && w->Mesh->AttachParent == w->CapsuleComponent ? "the capsule" : "something else");
		}
		if (auto *cmc = w->CharacterMovement; cmc != nullptr)
		{
			// walking again at once, physics still off: its animation sees ground under it and puts its legs down (frozen
			// with Steve in the air, they were drawn up to the hips - and put out when it was dropped onto them)
			cmc->StopMovementImmediately();
			cmc->SetComponentTickEnabled(true);
			cmc->SetMovementMode(SDK::EMovementMode::MOVE_Walking, 0);
		}
		// its physics comes back a moment later, held still till then: its parts, moved along with Steve every frame,
		// kept that speed when physics came back on at once (flung, every bone broken)
		g_restored = g_standIn.ref;
		g_restoredAt = GetTickCount64();
		g_restorePending = true;
		g_restorePlaced = false;
		g_restoreStill = 0;
		w->Blood_Bleed_Ok = true;
		w->Blood_Splash_Ok = true;
		stand_in_weapons(w, false);
		g_standInPlaced = {};
		w->SetActorTickEnabled(true);
		g_standIn.hidden = false;
		g_standInObj = nullptr;
		g_steveHits.clear();
		hslog::info("stand-in restored");
	}

	// the given-back body's joints, unbreakable for its first moments: physics coming back on put a leg out of joint
	// 31 ms in (the engine tears a joint past its break limit; Half Sword then calls the leg dislocated). Each joint's
	// own limits, to put back
	struct SavedJoint
	{
		SDK::FConstraintInstanceAccessor joint;
		bool linear = false, angular = false;
		float linearAt = 0.0f, angularAt = 0.0f;
	};
	std::vector<SavedJoint> g_savedJoints;
	Ref g_savedJointsMesh;

	void restore_joints()
	{
		if (g_savedJointsMesh.alive())
			for (SavedJoint &s : g_savedJoints)
			{
				SDK::UConstraintInstanceBlueprintLibrary::SetLinearBreakable(s.joint, s.linear, s.linearAt);
				SDK::UConstraintInstanceBlueprintLibrary::SetAngularBreakable(s.joint, s.angular, s.angularAt);
			}
		if (!g_savedJoints.empty())
			hslog::info("the body given back: its %zu breakable joints breakable again", g_savedJoints.size());
		g_savedJoints.clear();
		g_savedJointsMesh = {};
	}

	void hold_joints(SDK::USkeletalMeshComponent *mesh)
	{
		restore_joints();
		SDK::TArray<SDK::FConstraintInstanceAccessor> all;
		mesh->GetConstraints(false, &all);
		for (int i = 0; i < all.Num(); ++i)
		{
			SavedJoint s;
			s.joint = all[i];
			SDK::UConstraintInstanceBlueprintLibrary::GetLinearBreakable(s.joint, &s.linear, &s.linearAt);
			SDK::UConstraintInstanceBlueprintLibrary::GetAngularBreakable(s.joint, &s.angular, &s.angularAt);
			if (!s.linear && !s.angular)
				continue;
			SDK::UConstraintInstanceBlueprintLibrary::SetLinearBreakable(s.joint, false, s.linearAt);
			SDK::UConstraintInstanceBlueprintLibrary::SetAngularBreakable(s.joint, false, s.angularAt);
			g_savedJoints.push_back(s);
		}
		g_savedJointsMesh = {mesh, mesh->Index};
		hslog::info("the body given back: %zu of its %d joints unbreakable for now", g_savedJoints.size(), all.Num());
	}

	/// The body just given back, its first moments: a burst of speed is the physics shoving it out of something, not
	/// a move of its own - stopped.
	void settle_restored()
	{
		if (!g_restored.alive())
			return;
		auto *w = as_willie(g_restored.obj);
		if (w == nullptr || w->Mesh == nullptr || w->DED)
		{
			g_restorePending = false;
			return;
		}
		const ULONGLONG now = GetTickCount64();
		auto *mesh = w->Mesh;
		if (g_restorePending)
		{
			// its legs down: hips this far over its lowest foot (a standing body's are ~90 cm)
			static const SDK::FName pelvisName = name(L"pelvis");
			double low = 1e30;
			for (const wchar_t *b : {L"foot_l", L"foot_r", L"ball_l", L"ball_r", L"calf_l", L"calf_r"})
				low = std::min(low, mesh->GetSocketLocation(name(b)).Z);
			const double legs = mesh->GetSocketLocation(pelvisName).Z - low;
			const ULONGLONG waited = now - g_restoredAt;
			if (w->CharacterMovement != nullptr)
				w->CharacterMovement->StopMovementImmediately(); // (still: its parts' speed goes to nothing)
			const bool legsDone = legs >= kRestoreLegsDown || waited >= kRestoreMaxWaitMs;
			if (waited >= kRestorePhysicsDelayMs && legsDone && !g_restorePlaced)
			{
				// its feet on the ground once more, as they are now (then it has to be still again: see below)
				g_restorePlaced = true;
				double groundZ = 0.0;
				const SDK::FVector at = w->K2_GetActorLocation();
				if (g_world != nullptr && low < 1e29 && ground_z(g_world, at.X, at.Y, at.Z + 50.0, groundZ, true) && std::fabs(low - groundZ - 4.0) > 2.0 &&
					std::fabs(low - groundZ) < 150.0)
				{
					SDK::FHitResult hit{};
					w->K2_SetActorLocation(SDK::FVector(at.X, at.Y, at.Z + groundZ + 4.0 - low), false, &hit, true);
					hslog::info("the body given back: feet put on the ground, %+.0f cm", groundZ + 4.0 - low);
				}
				hslog::info("the body given back: legs %s (hips %.0f cm over its lowest foot) after %llu ms", legs >= kRestoreLegsDown ? "down" : "STILL UP",
					legs, waited);
			}
			// still: its frozen parts' last move becomes their speed when physics takes them (placed 8 cm the frame before:
			// flung at 5.8 m/s; legs swinging down as it stood: a knee put out). Hips and feet unmoved for a few frames
			const SDK::FVector marks[3] = {mesh->GetSocketLocation(pelvisName), mesh->GetSocketLocation(name(L"foot_l")), mesh->GetSocketLocation(name(L"foot_r"))};
			double moved = 0.0;
			for (int i = 0; i < 3; ++i)
				moved = std::max(moved, std::sqrt((marks[i].X - g_restoreMarks[i].X) * (marks[i].X - g_restoreMarks[i].X) +
					(marks[i].Y - g_restoreMarks[i].Y) * (marks[i].Y - g_restoreMarks[i].Y) + (marks[i].Z - g_restoreMarks[i].Z) * (marks[i].Z - g_restoreMarks[i].Z)));
			for (int i = 0; i < 3; ++i)
				g_restoreMarks[i] = marks[i];
			g_restoreStill = moved < kRestoreStillCm ? g_restoreStill + 1 : 0;
			if (g_restorePlaced && (g_restoreStill >= kRestoreStillFrames || waited >= kRestoreMaxWaitMs + 600))
			{
				hslog::info("the body given back: still for %d frames (last move %.1f cm) after %llu ms", g_restoreStill, moved, waited);
				// a free ragdoll, hanging from nothing, as every live fighter's body is (lab/lua/simbones.lua: an AI
				// fighter's mesh has no parent). A new fight's body starts on its capsule with physics off, and Half Sword
				// lets go of it as its physics starts; given back on the capsule, the walking capsule dragged the ragdoll
				// along - a jolt forward every time, a knee put out every second or third
				if (mesh->AttachParent != nullptr)
					mesh->K2_DetachFromComponent(SDK::EDetachmentRule::KeepWorld, SDK::EDetachmentRule::KeepWorld, SDK::EDetachmentRule::KeepWorld, false);
				mesh->SetCollisionResponseToChannel(static_cast<SDK::ECollisionChannel>(16), g_standIn.bodyResponse);
				hold_joints(mesh);
				mesh->SetAllBodiesSimulatePhysics(true);
				mesh->SetAllPhysicsLinearVelocity(SDK::FVector(0, 0, 0), false);
				mesh->SetAllPhysicsAngularVelocityInDegrees(SDK::FVector(0, 0, 0), false);
				if (auto *cmc = w->CharacterMovement; cmc != nullptr)
				{
					cmc->StopMovementImmediately();
					cmc->SetComponentTickEnabled(true);
					cmc->SetMovementMode(SDK::EMovementMode::MOVE_Walking, 0);
				}
				g_restorePending = false;
				g_restoredAt = now;
				hslog::info("the body given back: its physics on again");
			}
		}
		else if (now - g_restoredAt > kRestoreSettleMs)
		{
			restore_joints(); // (its own again: blows can put joints out)
			return;
		}
		// its first moments: a burst of speed is the physics shoving it, not a move of its own - stopped, every frame;
		// and whatever that did to it doesn't count (it is as Steve left it: the stand-in's health, limbs...)
		if (!g_restorePending)
		{
			static const SDK::FName pelvis = name(L"pelvis");
			const SDK::FVector v = mesh->GetPhysicsLinearVelocity(pelvis);
			const double speed = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
			// (its very first moments: any speed at all is left over from before - none of its own yet)
			if (now - g_restoredAt < kRestoreZeroMs)
			{
				mesh->SetAllPhysicsLinearVelocity(SDK::FVector(0, 0, 0), false);
				mesh->SetAllPhysicsAngularVelocityInDegrees(SDK::FVector(0, 0, 0), false);
			}
			else if (speed > kRestoreMaxSpeed)
			{
				mesh->SetAllPhysicsLinearVelocity(SDK::FVector(0, 0, 0), false);
				mesh->SetAllPhysicsAngularVelocityInDegrees(SDK::FVector(0, 0, 0), false);
				hslog::info("the body given back was flung at %.0f cm/s, %llu ms in: stopped", speed, now - g_restoredAt);
			}
		}
		// a bone broken or a joint put out anyway (not through the Break_ functions, block_restore_break): say which
		{
			static uint32_t last = 0;
			const bool flags[] = {w->Leg_L_Broken, w->Leg_R_Broken, w->Arm_L_Broken, w->Arm_R_Broken, w->Back_Broken, w->Head_Broken,
				w->Leg_L_Dislocated, w->Leg_R_Dislocated, w->Arm_L_Dislocated, w->Arm_R_Dislocated, w->Neck_Dislocated, w->Spine_Dislocated};
			static const char *kNames[] = {"left leg broken", "right leg broken", "left arm broken", "right arm broken", "back broken", "head broken",
				"left leg dislocated", "right leg dislocated", "left arm dislocated", "right arm dislocated", "neck dislocated", "spine dislocated"};
			uint32_t bits = 0;
			for (int i = 0; i < 12; ++i)
				bits |= flags[i] ? (1u << i) : 0u;
			if (bits != last)
			{
				for (int i = 0; i < 12; ++i)
					if ((bits & (1u << i)) && !(last & (1u << i)))
						hslog::info("the body given back: %s, %llu ms in%s", kNames[i], now - g_restoredAt, g_restorePending ? " (physics still off)" : "");
				last = bits;
			}
		}
		if (g_restored.obj == g_standIn.ref.obj)
		{
			w->Health = std::max(w->Health, g_standIn.health);
			w->Consciousness = std::max(w->Consciousness, g_standIn.consciousness);
			w->Bleeding = 0.0;
			for (int i = 0; i < 10; ++i)
				*stand_in_limbs(w, i) = std::max(*stand_in_limbs(w, i), g_standIn.limbs[i]);
		}
	}

	/// A body that must stand exactly where its owner is (the fighters aim at its bones): Steve's stand-in, a mob's.
	/// No physics, the animated body pinned to the capsule as the class defaults have it. Half Sword turns physics
	/// back on for parts of the body (falling, limbs going limp...): look at all of them, every frame.
	void pin_body(SDK::AWillie_BP_C *w)
	{
		static const wchar_t *kPhysicsBones[] = {L"pelvis", L"spine_03", L"Head", L"thigh_l", L"calf_r", L"upperarm_l", L"hand_r"};
		bool loose = false;
		if (auto *mesh = w->Mesh; mesh != nullptr && w->CapsuleComponent != nullptr)
		{
			loose = mesh->AttachParent != w->CapsuleComponent;
			for (const wchar_t *b : kPhysicsBones)
				loose = loose || mesh->IsSimulatingPhysics(name(b));
		}
		if (auto *mesh = w->Mesh; loose)
		{
			auto *cdo = static_cast<SDK::AWillie_BP_C *>(g_willieClass->ClassDefaultObject);
			mesh->SetAllBodiesSimulatePhysics(false);
			mesh->SetSimulatePhysics(false);
			// (its size as it is in the world: given back, it hangs from nothing and its own scale is its size in the
			// world; kept "relative", the capsule's scale - a fighter's random size, 0.939 - went on top of that, and the
			// body shrank with every switch)
			mesh->K2_AttachToComponent(w->CapsuleComponent, SDK::FName{}, SDK::EAttachmentRule::KeepRelative, SDK::EAttachmentRule::KeepRelative,
				SDK::EAttachmentRule::KeepWorld, false);
			SDK::FHitResult hit{};
			if (cdo != nullptr && cdo->Mesh != nullptr)
				mesh->K2_SetRelativeLocationAndRotation(cdo->Mesh->RelativeLocation, cdo->Mesh->RelativeRotation, false, &hit, true);
			// bodies (collision type 16) pass through it, so running into a fighter can't knock him over; weapons
			// (type 14) still hit it
			mesh->SetCollisionResponseToChannel(static_cast<SDK::ECollisionChannel>(16), SDK::ECollisionResponse::ECR_Ignore);
			static int pins = 0;
			if (++pins <= 20 || pins % 100 == 0)
				hslog::info("%s body pinned to the capsule (%d)", w->Team_Int == kMobTeam ? "a mob's" : "the stand-in's", pins);
		}
	}

	void keep_stand_in(SDK::AWillie_BP_C *w)
	{
		if (g_standIn.ref.obj != w || !g_standIn.ref.alive())
		{
			g_standIn = {};
			g_standInPlaced = {};
			g_standIn.ref = {w, w->Index};
			g_standIn.since = GetTickCount64();
			g_standIn.meshParent = w->Mesh != nullptr ? w->Mesh->AttachParent : nullptr;
			g_standIn.meshParentKnown = w->Mesh != nullptr;
			if (w->Mesh != nullptr)
				g_standIn.bodyResponse = w->Mesh->GetCollisionResponseToChannel(static_cast<SDK::ECollisionChannel>(16));
			hslog::info("stand-in's body as Half Sword had it: hanging from %s, other bodies %s, simulating %d",
				w->Mesh == nullptr ? "-" : w->Mesh->AttachParent == nullptr ? "nothing" : w->Mesh->AttachParent == w->CapsuleComponent ? "the capsule" : "something else",
				g_standIn.bodyResponse == SDK::ECollisionResponse::ECR_Block ? "block" : g_standIn.bodyResponse == SDK::ECollisionResponse::ECR_Overlap ? "overlap" : "ignored",
				w->Mesh != nullptr ? int(w->Mesh->IsSimulatingPhysics(name(L"pelvis"))) : -1);
			g_standIn.health = w->Health;
			g_standIn.consciousness = w->Consciousness;
			g_standIn.stamina = w->Stamina;
			for (int i = 0; i < 10; ++i)
				g_standIn.limbs[i] = std::min(*stand_in_limbs(w, i), kStandInHealth);
			g_standIn.health = std::min(g_standIn.health, kStandInHealth);
			if (g_standIn.health <= 0.0)
				g_standIn.health = kStandInHealth;
			hslog::info("stand-in: %s (health %.0f)", w->GetName().c_str(), w->Health);
			// a new fight: on at once, not after the match-over check notices (until then G still went to Half Sword
			// and the stand-in's giving up wasn't undone: G held from the win screen gave up the new fight)
			if (g_matchOver)
				hslog::info("new fight: G is Minecraft's again");
			g_matchOver = false;
			g_overTicks = 0;
			g_hadFighter = false;
			input::share_continue_key(false);
			w->Give_Up = false;
			w->Give_Up_2__Temp_ = false;
			sendf("{\"t\":\"match\"}"); // a new body: a new fight (Minecraft gives the kit again)
		}
		pin_body(w);
		if (w->CharacterMovement != nullptr && w->CharacterMovement->MovementMode != SDK::EMovementMode::MOVE_None)
			w->CharacterMovement->DisableMovement(); // no gravity, no walking off: it is wherever Steve is
		if (!g_standIn.hidden)
		{
			hide_actor(w, true);
			stand_in_weapons(w, true);
			if (w->CharacterMovement != nullptr)
				w->CharacterMovement->DisableMovement();
			g_standIn.hidden = true;
			g_restorePending = false; // (Steve again before its physics came back: it stays pinned)
			restore_joints();
			g_restored = {};
		}
		// Steve's health is Minecraft's: the stand-in's hit events never reach Half Sword (the filter), the fighters'
		// weapons and fists on it go to Minecraft as they land
		g_standInObj = w;
		find_hit_events();
		send_steve_hits(w);
		// Steve died in Minecraft: so does the stand-in, and Half Sword's match is lost (its menu comes up)
		if (g_steve.dead && !g_steveWasDead && !w->DED && g_hadFighter && !g_matchOver)
		{
			hslog::info("Steve died: the stand-in dies too");
			w->SetActorTickEnabled(true); // its death is Half Sword's own, all of it
			g_standInPlaced = {};
			w->Death();
		}
		g_steveWasDead = g_steve.dead;
		if (w->DED)
			return;
		// and in case anything else gets through: it never dies, bleeds out, faints or falls
		w->Health = g_standIn.health;
		w->Consciousness = g_standIn.consciousness;
		w->Stamina = g_standIn.stamina;
		w->Bleeding = 0.0;
		for (int i = 0; i < 10; ++i)
			*stand_in_limbs(w, i) = g_standIn.limbs[i];
		w->Blood_Bleed_Ok = false;
		w->Blood_Splash_Ok = false;
		w->Spawn_On_Death_Blood_Emitter = false;
		// unarmed and unseen, Half Sword's fear would make it yield and end the match: Steve doesn't give up
		if (!g_matchOver)
		{
			w->Fear = 0.0;
			w->Fearless = true;
			w->Give_Up = false; // (holding G is giving up / going on: Half Sword's own once the match is won)
			w->Give_Up_2__Temp_ = false;
		}
		// where Steve is
		if (g_steve.valid && g_tpIn < 0)
		{
			SDK::FVector at = to_ue(g_steve.x, g_steve.y, g_steve.z);
			at.Z += w->CapsuleComponent != nullptr ? w->CapsuleComponent->CapsuleHalfHeight : 90.0;
			// where Half Sword left it since the last frame: it must not have moved (it used to fall back to the ground
			// between frames whenever Steve was up high, and took the blows down there)
			if (g_standInPlaced.valid)
			{
				const SDK::FVector now = w->K2_GetActorLocation();
				const double drift = std::sqrt((now.X - g_standInPlaced.at.X) * (now.X - g_standInPlaced.at.X) +
					(now.Y - g_standInPlaced.at.Y) * (now.Y - g_standInPlaced.at.Y) + (now.Z - g_standInPlaced.at.Z) * (now.Z - g_standInPlaced.at.Z));
				static ULONGLONG said = 0;
				if (drift > 30.0 && GetTickCount64() - said > 2000)
				{
					said = GetTickCount64();
					hslog::info("stand-in moved %.0f cm (%.0f down) since it was placed (put back %u times between frames): movement mode %d, falling at "
						"%.0f cm/s, movement ticking %d", drift, g_standInPlaced.at.Z - now.Z, g_standInHeld, w->CharacterMovement != nullptr ? int(w->CharacterMovement->MovementMode.EnumValue) : -1,
						w->CharacterMovement != nullptr ? -w->CharacterMovement->Velocity.Z : 0.0,
						w->CharacterMovement != nullptr ? int(w->CharacterMovement->IsComponentTickEnabled()) : -1);
				}
			}
			SDK::FHitResult hit{};
			w->K2_SetActorLocation(at, false, &hit, true);
			w->K2_SetActorRotation(SDK::FRotator(0.0, g_steve.yaw + 90.0, 0.0), true);
			// and Half Sword leaves it alone: no movement (no falling, no walking, no speed left over from a fall), and
			// none of the body's own per-frame logic, which put it back on the ground at the start of every frame (with
			// Steve up on blocks, the fighters hit it down there); hold_stand_in puts it back if anything still moves it
			if (auto *cmc = w->CharacterMovement; cmc != nullptr)
			{
				cmc->StopMovementImmediately();
				if (cmc->IsComponentTickEnabled())
					cmc->SetComponentTickEnabled(false);
			}
			// (not at once: a body just spawned for a new fight is still being put together by its own logic, and pausing
			// that half way left parts that crashed Half Sword when pinned; until then hold_stand_in keeps it in place)
			if (w->IsActorTickEnabled() && GetTickCount64() - g_standIn.since > kStandInSettle)
			{
				w->SetActorTickEnabled(false);
				hslog::info("stand-in: its own per-frame logic paused");
			}
			g_standInPlaced = {w->K2_GetActorLocation(), true};
			if (GetTickCount64() - g_standInLogAt >= 3000 && w->Mesh != nullptr)
			{
				// where its body really is against Steve (Minecraft y): they must agree, also up on blocks
				g_standInLogAt = GetTickCount64();
				double x, actorY, pelvisY, headY, z;
				to_mc(w->K2_GetActorLocation(), x, actorY, z);
				to_mc(w->Mesh->GetSocketLocation(name(L"pelvis")), x, pelvisY, z);
				to_mc(w->Mesh->GetSocketLocation(name(L"Head")), x, headY, z);
				hslog::info("stand-in: Steve feet y %.2f, capsule centre y %.2f, pelvis y %.2f, head y %.2f", g_steve.y, actorY, pelvisY, headY);
			}
		}
	}

	// ---- Minecraft's mobs' bodies ----
	int32_t *enemy_count(SDK::UWorld *world)
	{
		SDK::AGameModeBase *gm = world != nullptr ? world->AuthorityGameMode : nullptr;
		if (gm == nullptr || gm->Class == nullptr || gm->Class->GetName() != "BP_HalfSwordGameMode_C")
			return nullptr;
		return reinterpret_cast<int32_t *>(reinterpret_cast<uint8_t *>(gm) + 0x0390); // BP_HalfSwordGameMode_C::Enemy_Count
	}

	/// A fighter with no AI of its own (it goes where its mob goes) on the mobs' team. Not one of the match's enemies.
	SDK::AWillie_BP_C *spawn_mob_body(SDK::UWorld *world, const SDK::FVector &at)
	{
		int32_t *enemies = enemy_count(world);
		const int32_t before = enemies != nullptr ? *enemies : 0;
		SDK::FTransform t{};
		t.Rotation.W = 1.0;
		t.Translation = SDK::FVector(at.X, at.Y, at.Z + 100.0);
		t.Scale3D = SDK::FVector(1, 1, 1);
		SDK::AActor *a = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world, g_willieClass, t,
			SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		auto *w = as_willie(a);
		if (w == nullptr)
			return nullptr;
		w->AutoPossessAI = SDK::EAutoPossessAI::Disabled;
		w->Team_Int = kMobTeam;
		SDK::UGameplayStatics::FinishSpawningActor(a, t, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		if (enemies != nullptr && *enemies != before)
			*enemies = before;
		hide_actor(w, true);
		return w;
	}

	void remove_mob_body(MobBody &b)
	{
		if (b.ref.obj != nullptr)
			g_mobBodyOf.erase(b.ref.obj);
		if (b.ref.alive())
			static_cast<SDK::AActor *>(b.ref.obj)->K2_DestroyActor(); // (never Death(): that counts an enemy down)
		b.ref = {};
	}

	void clear_mob_bodies()
	{
		for (auto &[id, b] : g_mobBodies)
			remove_mob_body(b);
		g_mobBodies.clear();
		g_mobBodyOf.clear();
	}

	/// {"t":"mobs","m":[[id,"kind",x,y,z,height,yaw],...]}: Minecraft's mobs near Steve, nearest first, every tick.
	void on_mobs(SDK::UWorld *world, const std::string &m)
	{
		const ULONGLONG now = GetTickCount64();
		size_t p = m.find("\"m\":[");
		if (p == std::string::npos)
			return;
		p += 5;
		while ((p = m.find('[', p)) != std::string::npos)
		{
			int id = 0;
			char kind[48] = {};
			double x = 0, y = 0, z = 0, h = 1.8, yaw = 0;
			const int n = sscanf_s(m.c_str() + p, "[%d,\"%47[^\"]\",%lf,%lf,%lf,%lf,%lf", &id, kind, unsigned(sizeof(kind)), &x, &y, &z, &h, &yaw);
			p = m.find(']', p);
			if (p == std::string::npos)
				break;
			++p;
			if (n < 5)
				continue;
			auto it = g_mobBodies.find(id);
			if (it == g_mobBodies.end() || !it->second.ref.alive())
			{
				if (it == g_mobBodies.end() && int(g_mobBodies.size()) >= kMaxMobBodies)
					continue;
				auto *w = spawn_mob_body(world, to_ue(x, y, z));
				if (w == nullptr)
					continue;
				MobBody &b = g_mobBodies[id];
				remove_mob_body(b);
				b = MobBody{};
				b.ref = {w, w->Index};
				b.kind = kind;
				g_mobBodyOf[w] = id;
				hslog::info("mob body for %s %d: %s", kind, id, w->GetName().c_str());
				it = g_mobBodies.find(id);
			}
			MobBody &b = it->second;
			b.x = x;
			b.y = y;
			b.z = z;
			b.height = h;
			b.yaw = yaw;
			b.seen = now;
		}
	}

	/// A mob's body each frame: where its mob is, its size, never hurt here (its health is Minecraft's), and the
	/// fighters' blows on it to Minecraft.
	void keep_mob_body(int id, MobBody &b)
	{
		auto *w = as_willie(b.ref.obj);
		if (w == nullptr)
			return;
		if (!b.setUp)
		{
			b.setUp = true;
			b.health = w->Health > 0.0 ? w->Health : 100.0;
			b.consciousness = w->Consciousness;
			b.stamina = w->Stamina;
			for (int i = 0; i < 10; ++i)
				b.limbs[i] = *stand_in_limbs(w, i);
			hide_actor(w, true);
		}
		pin_body(w);
		if (w->CharacterMovement != nullptr && w->CharacterMovement->MovementMode != SDK::EMovementMode::MOVE_None)
			w->CharacterMovement->DisableMovement();
		w->Health = b.health;
		w->Consciousness = b.consciousness;
		w->Stamina = b.stamina;
		w->Bleeding = 0.0;
		for (int i = 0; i < 10; ++i)
			*stand_in_limbs(w, i) = b.limbs[i];
		w->Blood_Bleed_Ok = false;
		w->Blood_Splash_Ok = false;
		w->Spawn_On_Death_Blood_Emitter = false;
		w->Fear = 0.0;
		w->Fearless = true;
		w->Give_Up = false;
		w->Give_Up_2__Temp_ = false;
		// its mob's size: a fighter's body scaled to the mob's height (a spider's is low, a golem's tall)
		const float scale = float(std::clamp(b.height / 1.85, 0.3, 1.7));
		if (std::fabs(scale - b.scale) > 0.01f)
		{
			w->SetActorScale3D(SDK::FVector(scale, scale, scale));
			b.scale = scale;
		}
		SDK::FVector at = to_ue(b.x, b.y, b.z);
		at.Z += (w->CapsuleComponent != nullptr ? w->CapsuleComponent->CapsuleHalfHeight : 90.0) * scale;
		SDK::FHitResult hit{};
		w->K2_SetActorLocation(at, false, &hit, true);
		w->K2_SetActorRotation(SDK::FRotator(0.0, b.yaw + 90.0, 0.0), true);
		if (!b.hits.empty())
		{
			const BestHit best = score_hits(b.hits, w, false);
			if (best.hit != nullptr && best.points >= 0.3)
			{
				sendf("{\"t\":\"mobhurt\",\"id\":%d,\"d\":%.2f,\"by\":%d}", id, best.points, best.attacker != nullptr ? best.attacker->Index : -1);
				hslog::info("%s %d hit by %s: %.0f cm/s -> %.2f Minecraft damage", b.kind.c_str(), id,
					best.attacker != nullptr ? best.attacker->GetName().c_str() : "?", best.speed, best.points);
			}
			b.hits.clear();
		}
	}

	void keep_mob_bodies()
	{
		const ULONGLONG now = GetTickCount64();
		for (auto it = g_mobBodies.begin(); it != g_mobBodies.end();)
		{
			MobBody &b = it->second;
			if (now - b.seen > 1000 || !b.ref.alive() || as_willie(b.ref.obj)->DED)
			{
				hslog::info("mob body for %s %d gone", b.kind.c_str(), it->first);
				remove_mob_body(b);
				it = g_mobBodies.erase(it);
				continue;
			}
			keep_mob_body(it->first, b);
			++it;
		}
	}

	// ---- Minecraft's blocks, solid in Half Sword: the fighters, their weapons and Steve's stand-in meet them ----
	int64_t block_key(int x, int y, int z)
	{
		return (int64_t(x & 0xFFFFF) << 40) | (int64_t(z & 0xFFFFF) << 20) | int64_t(y & 0xFFFFF);
	}

	SDK::FVector block_centre(int x, int y, int z)
	{
		return to_ue(x + 0.5, y + 0.5, z + 0.5);
	}

	constexpr double kNavHoleHalf = 30.0; // half the width of a block's hole in the navmesh (cm; the block is 50)

	/// The actor carrying the boxes (one per world; its root sits at the origin, so a box's relative place is its place).
	SDK::AActor *block_actor(SDK::UWorld *world)
	{
		if (g_blockActor.alive())
			return static_cast<SDK::AActor *>(g_blockActor.obj);
		g_blocks.clear();
		SDK::FTransform t{};
		t.Rotation.W = 1.0;
		t.Scale3D = SDK::FVector(1, 1, 1);
		SDK::AActor *a = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world, SDK::AActor::StaticClass(), t,
			SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		if (a == nullptr)
			return nullptr;
		SDK::UGameplayStatics::FinishSpawningActor(a, t, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		a->AddComponentByClass(SDK::USceneComponent::StaticClass(), false, t, false); // the root
		g_blockActor = {a, a->Index};
		hslog::info("block actor %s", a->GetName().c_str());
		return a;
	}

	void set_block(SDK::UWorld *world, int x, int y, int z, bool solid)
	{
		const int64_t key = block_key(x, y, z);
		auto it = g_blocks.find(key);
		if (!solid)
		{
			if (it != g_blocks.end())
			{
				for (Ref *r : {&it->second.box, &it->second.nav})
					if (r->alive())
					{
						auto *c = static_cast<SDK::UActorComponent *>(r->obj);
						c->K2_DestroyComponent(c);
					}
				g_blocks.erase(it);
			}
			return;
		}
		if (it != g_blocks.end() && it->second.box.alive())
			return;
		SDK::AActor *a = block_actor(world);
		if (a == nullptr)
			return;
		SDK::FTransform t{};
		t.Rotation.W = 1.0;
		t.Scale3D = SDK::FVector(1, 1, 1);
		t.Translation = block_centre(x, y, z);
		// (deferred: the navigation settings have to be in place before a box registers with the world)
		auto *box = static_cast<SDK::UBoxComponent *>(a->AddComponentByClass(SDK::UBoxComponent::StaticClass(), false, t, true));
		if (box == nullptr)
			return;
		box->SetBoxExtent(SDK::FVector(50.0, 50.0, 50.0), false);
		box->SetCollisionObjectType(SDK::ECollisionChannel::ECC_WorldStatic);
		box->SetCollisionResponseToAllChannels(SDK::ECollisionResponse::ECR_Block);
		box->SetCollisionEnabled(SDK::ECollisionEnabled::QueryAndPhysics);
		box->SetHiddenInGame(true, false);
		box->bCanEverAffectNavigation = 0; // (the hole is the nav box's)
		a->FinishAddComponent(box, false, t);
		// A hole in the arena's navmesh: the fighters' AI (MoveToLocation) paths around Minecraft's blocks instead of
		// walking into them (the arena's navmesh is "dynamic modifiers only": obstacles like this are cut in at
		// runtime). The navmesh grows every hole by the fighters' radius (35 cm) and its 19 cm cells, which closed
		// one-block gaps: the hole is 60 cm across, not the block's 100 - a one-block gap keeps ~70 cm of path, two
		// blocks side by side still close up (40 cm apart, grown 35 cm each).
		auto *nav = static_cast<SDK::UBoxComponent *>(a->AddComponentByClass(SDK::UBoxComponent::StaticClass(), false, t, true));
		SDK::UObject *navRef = nullptr;
		if (nav != nullptr)
		{
			nav->SetBoxExtent(SDK::FVector(kNavHoleHalf, kNavHoleHalf, 50.0), false);
			nav->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
			nav->SetHiddenInGame(true, false);
			nav->bCanEverAffectNavigation = 1;
			nav->bDynamicObstacle = 1; // (a dynamic obstacle needs no collision)
			nav->bUseSystemDefaultObstacleAreaClass = 0;
			static SDK::UClass *nullArea = SDK::UObject::FindClassFast("NavArea_Null");
			static bool said = false;
			if (!said)
			{
				said = true;
				hslog::info("blocks cut holes in the arena's navmesh: %s", nullArea != nullptr ? "on" : "NavArea_Null not found (fighters walk into blocks)");
			}
			nav->AreaClassOverride = nullArea;
			a->FinishAddComponent(nav, false, t);
			navRef = nav;
		}
		g_blocks[key] = {x, y, z, {box, box->Index}, {navRef, navRef != nullptr ? navRef->Index : -1}};
	}

	/// {"t":"blocks","set":[x,y,z,...],"clear":[x,y,z,...]}: Minecraft's solid blocks that appeared or went.
	void on_blocks(SDK::UWorld *world, const std::string &m)
	{
		for (const char *key : {"clear", "set"})
		{
			const char *p = json_at(m, key);
			if (p == nullptr || *p != '[')
				continue;
			++p;
			int v[3], n = 0;
			while (*p != '\0' && *p != ']')
			{
				char *end = nullptr;
				v[n++] = int(std::strtol(p, &end, 10));
				if (end == p)
					break;
				p = end;
				while (*p == ',' || *p == ' ')
					++p;
				if (n == 3)
				{
					set_block(world, v[0], v[1], v[2], key[0] == 's');
					n = 0;
				}
			}
		}
	}

	/// The ground was levelled again (the offset changed): every box to its block's new place.
	void move_blocks()
	{
		for (auto &[key, b] : g_blocks)
			for (Ref *r : {&b.box, &b.nav})
				if (r->alive())
				{
					SDK::FHitResult hit{};
					static_cast<SDK::USceneComponent *>(r->obj)->K2_SetWorldLocation(block_centre(b.x, b.y, b.z), false, &hit, true);
				}
	}

	// ---- fighters in Minecraft's boats and minecarts: kneeling on a hidden plank that goes where the seat goes ----
	// (a fighter's body is a free ragdoll that walks after his capsule: holding the capsule alone leaves him standing
	// on the ground. A plank under him rises to the vehicle's floor and lifts him; then it follows the vehicle and his
	// body rides on it, his capsule held over it, movement off. He keeps his own physics: he can still fight.)
	constexpr double kBoatFloor = 0.15;     // blocks over a boat's bottom where the knees go
	constexpr double kCartFloor = 0.25;     // and a minecart's
	constexpr double kRideRiseMs = 600.0;   // the plank rises from his feet to the seat in this long
	constexpr double kRideCapsule = 90.0;   // his capsule's centre over the plank (cm)
	constexpr double kPlankHalf = 55.0;     // the plank's half width (cm; a boat is 137 wide)
	constexpr double kPlankHalfThick = 8.0;
	constexpr double kRideLeadMs = 100.0;   // the server's seat is predicted this far at most past its last word
	constexpr ULONGLONG kDrawnSeatMs = 300; // the seat where Minecraft draws the vehicle counts this long after its last word
	constexpr double kRidePull = 6.0;       // per second: how hard his body is pulled back onto the seat (of the way left)
	constexpr double kRideMaxCorrection = 1500.0; // cm/s of speed he can be given in one frame
	constexpr double kRideLostCm = 200.0;   // his hips this far from the seat...
	constexpr double kRideLostMs = 1000.0;  // ...this long: he fell out
	struct Rider
	{
		int kind = 0;                         // 0 boat, 1 minecart
		SDK::FVector seat{}, vel{};           // the plank's top centre (Half Sword cm), and how fast it goes (cm/s)
		ULONGLONG seatAt = 0, seen = 0, since = 0, kneelAt = 0, logAt = 0, lostSince = 0;
		SDK::FVector velFrom{};               // the seat a moment ago, for its speed
		ULONGLONG velFromAt = 0;
		ULONGLONG drawnAt = 0;                // the seat came from Minecraft's picture (not predicted: it is what's seen)
		SDK::FVector from{};                  // where the plank started (under his feet)
		Ref plank;
		bool started = false;
	};
	std::unordered_map<int, Rider> g_riders; // fighter handle ->
	std::unordered_map<int, ULONGLONG> g_unrideUntil; // fighters who fell out: Minecraft's word that they still ride is old until then

	const char *vehicle_name(const Rider &r)
	{
		return r.kind == 1 ? "minecart" : "boat";
	}

	void drop_plank(Rider &r)
	{
		if (r.plank.alive())
		{
			auto *c = static_cast<SDK::UActorComponent *>(r.plank.obj);
			c->K2_DestroyComponent(c);
		}
		r.plank = {};
	}

	/// He's out: on his own feet again (walking, standing up), the plank gone.
	void release_rider(int handle, Rider &r, const char *why)
	{
		drop_plank(r);
		if (auto *w = fighter_by_handle(handle); w != nullptr && r.started)
		{
			if (w->CharacterMovement != nullptr)
				w->CharacterMovement->SetMovementMode(SDK::EMovementMode::MOVE_Walking, 0);
			if (!w->DED && w->Health > 0.0)
				w->Un_Kneel_Event();
		}
		hslog::info("fighter %d out of the %s (%s)", handle, vehicle_name(r), why);
	}

	void release_riders(const char *why)
	{
		for (auto &[handle, r] : g_riders)
			release_rider(handle, r, why);
		g_riders.clear();
	}

	/// A rider's seat is here now (and its speed over at least 40 ms: two words can arrive in the same frame).
	void set_seat(Rider &r, const SDK::FVector &seat, ULONGLONG now)
	{
		r.seat = seat;
		r.seatAt = now;
		const double dt = double(now - r.velFromAt) / 1000.0;
		if (dt >= 0.04)
		{
			r.vel = dt < 0.3 ? SDK::FVector((seat.X - r.velFrom.X) / dt, (seat.Y - r.velFrom.Y) / dt, (seat.Z - r.velFrom.Z) / dt) : SDK::FVector(0, 0, 0);
			r.velFrom = seat;
			r.velFromAt = now;
		}
	}

	/// {"t":"seats","s":[[handle, x, vehicle y, z],...]}: where Minecraft draws the riders' vehicles this frame. The
	/// server's own seats run ahead of the picture (the client draws vehicles smoothed, a few ticks behind): a fast
	/// cart's rider knelt a metre or two in front of it.
	void on_seats(const std::string &m)
	{
		const ULONGLONG now = GetTickCount64();
		size_t p = m.find("\"s\":[");
		if (p == std::string::npos)
			return;
		p += 5;
		while ((p = m.find('[', p)) != std::string::npos)
		{
			int handle = 0;
			double x = 0, y = 0, z = 0;
			const int n = sscanf_s(m.c_str() + p, "[%d,%lf,%lf,%lf", &handle, &x, &y, &z);
			p = m.find(']', p);
			if (p == std::string::npos)
				break;
			++p;
			auto it = g_riders.find(handle);
			if (n < 4 || it == g_riders.end())
				continue;
			Rider &r = it->second;
			set_seat(r, to_ue(x, y + (r.kind == 1 ? kCartFloor : kBoatFloor), z), now);
			r.drawnAt = now;
		}
	}

	/// {"t":"ride","r":[[handle, x, y, z, kind],...]}: every fighter in a boat or cart now (y the vehicle's bottom).
	void on_ride(const std::string &m)
	{
		const ULONGLONG now = GetTickCount64();
		std::unordered_set<int> listed;
		size_t p = m.find("\"r\":[");
		if (p == std::string::npos)
			return;
		p += 5;
		while ((p = m.find('[', p)) != std::string::npos)
		{
			int handle = 0, kind = 0;
			double x = 0, y = 0, z = 0;
			const int n = sscanf_s(m.c_str() + p, "[%d,%lf,%lf,%lf,%d", &handle, &x, &y, &z, &kind);
			p = m.find(']', p);
			if (p == std::string::npos)
				break;
			++p;
			if (n < 5)
				continue;
			if (auto u = g_unrideUntil.find(handle); u != g_unrideUntil.end())
			{
				if (now < u->second)
					continue;
				g_unrideUntil.erase(u);
			}
			listed.insert(handle);
			const SDK::FVector seat = to_ue(x, y + (kind == 1 ? kCartFloor : kBoatFloor), z);
			auto it = g_riders.find(handle);
			if (it == g_riders.end())
			{
				Rider r;
				r.kind = kind;
				r.seat = seat;
				r.seatAt = r.seen = r.since = r.velFromAt = now;
				r.velFrom = seat;
				g_riders[handle] = r;
				hslog::info("fighter %d got in a %s", handle, vehicle_name(r));
				continue;
			}
			Rider &r = it->second;
			// (the server's word: only while Minecraft's picture says nothing - "seats" is what's seen)
			if (now - r.drawnAt > kDrawnSeatMs)
				set_seat(r, seat, now);
			r.kind = kind;
			r.seen = now;
		}
		for (auto it = g_riders.begin(); it != g_riders.end();)
		{
			if (listed.count(it->first))
			{
				++it;
				continue;
			}
			release_rider(it->first, it->second, "out in Minecraft");
			it = g_riders.erase(it);
		}
	}

	/// A rider's first frame: a plank right under his feet (his lowest limb: the capsule floats over them), kneeling.
	bool start_rider(SDK::UWorld *world, int handle, Rider &r, SDK::AWillie_BP_C *w, ULONGLONG now)
	{
		double low = 1e30;
		if (w->Mesh != nullptr)
			for (const wchar_t *b : {L"foot_l", L"foot_r", L"calf_l", L"calf_r", L"ball_l", L"ball_r"})
				low = std::min(low, w->Mesh->GetSocketLocation(name(b)).Z);
		const SDK::FVector at = w->K2_GetActorLocation();
		const SDK::FVector hips = w->Mesh != nullptr ? w->Mesh->GetSocketLocation(name(L"pelvis")) : at;
		r.from = SDK::FVector(hips.X, hips.Y, low < 1e29 ? low - 10.0 : at.Z - 100.0);
		SDK::AActor *a = block_actor(world);
		if (a == nullptr)
			return false;
		SDK::FTransform t{};
		t.Rotation.W = 1.0;
		t.Scale3D = SDK::FVector(1, 1, 1);
		t.Translation = SDK::FVector(r.from.X, r.from.Y, r.from.Z - kPlankHalfThick);
		auto *box = static_cast<SDK::UBoxComponent *>(a->AddComponentByClass(SDK::UBoxComponent::StaticClass(), false, t, true));
		if (box == nullptr)
			return false;
		box->SetBoxExtent(SDK::FVector(kPlankHalf, kPlankHalf, kPlankHalfThick), false);
		box->SetCollisionObjectType(SDK::ECollisionChannel::ECC_WorldStatic);
		box->SetCollisionResponseToAllChannels(SDK::ECollisionResponse::ECR_Block);
		box->SetCollisionEnabled(SDK::ECollisionEnabled::QueryAndPhysics);
		box->SetHiddenInGame(true, false);
		box->bCanEverAffectNavigation = 0; // (it moves: no holes in the arena's navmesh)
		a->FinishAddComponent(box, false, t);
		r.plank = {box, box->Index};
		r.started = true;
		r.since = r.kneelAt = now;
		w->Kneel_Event();
		hslog::info("fighter %d kneels in the %s: plank from %.0f up to %.0f", handle, vehicle_name(r), r.from.Z, r.seat.Z);
		return true;
	}

	/// Each frame: every rider's plank where his seat is, him on it, kneeling.
	void keep_riders(SDK::UWorld *world)
	{
		const ULONGLONG now = GetTickCount64();
		for (auto it = g_riders.begin(); it != g_riders.end();)
		{
			const int handle = it->first;
			Rider &r = it->second;
			auto *w = fighter_by_handle(handle);
			if (w == nullptr && now - r.seen <= 1000)
			{
				drop_plank(r); // (his body went - a corpse cleaned up - but Minecraft still seats him: wait for it to say)
				r.started = false;
				++it;
				continue;
			}
			const char *why = w == nullptr ? "fighter gone" : (w->DED || w->Health <= 0.0) ? "dead" : now - r.seen > 1000 ? "no word from Minecraft" : nullptr;
			if (why != nullptr)
			{
				release_rider(handle, r, why);
				it = g_riders.erase(it);
				continue;
			}
			++it;
			if (!r.started && !start_rider(world, handle, r, w, now))
				continue;
			// the seat now: Minecraft's last word, carried on at the speed it was going
			// (the picture's seat isn't predicted: it is where the vehicle is seen; the server's comes 20 times a second,
			// Half Sword's frames 60+, so in between it is)
			const double lead = now - r.drawnAt <= kDrawnSeatMs ? 0.0 : std::min(double(now - r.seatAt), kRideLeadMs) / 1000.0;
			SDK::FVector seat(r.seat.X + r.vel.X * lead, r.seat.Y + r.vel.Y * lead, r.seat.Z + r.vel.Z * lead);
			// getting in: from under his feet up (and over) to the seat
			const double f = std::clamp(double(now - r.since) / kRideRiseMs, 0.0, 1.0);
			if (f < 1.0)
				seat = SDK::FVector(r.from.X + (seat.X - r.from.X) * f, r.from.Y + (seat.Y - r.from.Y) * f,
					seat.Z < r.from.Z ? seat.Z : r.from.Z + (seat.Z - r.from.Z) * f);
			if (r.plank.alive())
			{
				SDK::FHitResult hit{};
				// (no teleport: a moving plank carries what lies on it)
				static_cast<SDK::USceneComponent *>(r.plank.obj)->K2_SetWorldLocation(SDK::FVector(seat.X, seat.Y, seat.Z - kPlankHalfThick), false, &hit,
					false);
			}
			if (auto *cmc = w->CharacterMovement; cmc != nullptr)
			{
				if (cmc->MovementMode != SDK::EMovementMode::MOVE_None)
					cmc->DisableMovement();
				cmc->StopMovementImmediately();
			}
			SDK::FHitResult hit{};
			w->K2_SetActorLocation(SDK::FVector(seat.X, seat.Y, seat.Z + kRideCapsule), false, &hit, true);
			// kneeling (Half Sword stands him up after a fall, say: down again)
			if (!w->L_Kneel && !w->R_Kneel && now - r.kneelAt > 1000)
			{
				w->Kneel_Event();
				r.kneelAt = now;
			}
			if (w->Mesh == nullptr)
				continue;
			// his body goes with the seat: the plank alone leaves him behind (he only walks after his capsule, slowly,
			// on his knees). Every frame his whole body gets the speed it lacks, the seat's own and a pull back onto
			// it, as a knockback's push is given (relative motion stays: he can still swing)
			static const SDK::FName pelvis = name(L"pelvis");
			const SDK::FVector hips = w->Mesh->GetSocketLocation(pelvis);
			const double ax = seat.X - hips.X, ay = seat.Y - hips.Y, aside = std::sqrt(ax * ax + ay * ay);
			const SDK::FVector moving = f < 1.0 ? SDK::FVector(0, 0, 0) : r.vel;
			const SDK::FVector has = w->Mesh->GetPhysicsLinearVelocity(pelvis);
			double dx = moving.X + ax * kRidePull - has.X, dy = moving.Y + ay * kRidePull - has.Y;
			if (const double len = std::sqrt(dx * dx + dy * dy); len > kRideMaxCorrection)
			{
				dx *= kRideMaxCorrection / len;
				dy *= kRideMaxCorrection / len;
			}
			if (aside < kRideLostCm)
				w->Mesh->AddImpulseToAllBodiesBelow(SDK::FVector(dx, dy, 0.0), pelvis, true, true);
			// knocked out of it all the same (a blow, a crash): he's out in Minecraft too, on his feet again
			if (aside >= kRideLostCm && f >= 1.0)
			{
				if (r.lostSince == 0)
					r.lostSince = now;
				else if (now - r.lostSince > kRideLostMs)
				{
					sendf("{\"t\":\"unride\",\"h\":%d}", handle);
					g_unrideUntil[handle] = now + 3000;
					hslog::info("rider %d is %.0f cm from his seat: out", handle, aside);
					release_rider(handle, r, "fell out");
					it = g_riders.erase(std::prev(it));
					continue;
				}
			}
			else
				r.lostSince = 0;
			if (now - r.logAt > 3000)
			{
				r.logAt = now;
				hslog::info("rider %d: hips %.0f cm over the plank, %.0f cm aside of the seat; seat going %.0f cm/s, him %.0f", handle, hips.Z - seat.Z,
					aside, std::sqrt(r.vel.X * r.vel.X + r.vel.Y * r.vel.Y), std::sqrt(has.X * has.X + has.Y * has.Y));
			}
		}
	}

	/// /hs ground: how the ground scan sees Steve's column (its middle line): every trace and what it hit, the solid
	/// stretches that make, and what Minecraft has in each cell (# barrier, p partial, s partial with a support barrier).
	void ground_why(SDK::UWorld *world)
	{
		if (!g_steve.valid || !g_haveOffset)
			return;
		const int x = int(std::floor(g_steve.x)), z = int(std::floor(g_steve.z)), refY = int(std::floor(g_steve.y));
		const double zTop = (refY - g_yOffset) * 100.0 + kScanFromAbove, zBot = (refY - kCellsBelow - 2 - g_yOffset) * 100.0;
		std::string why;
		g_groundWhy = &why;
		std::vector<std::pair<double, double>> st;
		solid_stretches(world, (x + 0.5) * 100.0, (z + 0.5) * 100.0, zTop, zBot, st);
		g_groundWhy = nullptr;
		why += "  solid, in Minecraft heights:";
		for (const auto &[lo, hi] : st)
		{
			char r[64];
			snprintf(r, sizeof(r), " %.2f..%.2f", lo / 100.0 + g_yOffset, hi / 100.0 + g_yOffset);
			why += r;
		}
		why += "\n  cells from y " + std::to_string(refY + kCellsAbove) + " down:";
		for (int y = refY + kCellsAbove; y >= refY - kCellsBelow; --y)
		{
			auto it = g_sentCells.find(cell_key(x, y, z));
			why += it == g_sentCells.end() ? " ." : it->second == 1 ? " #" : " p";
			if (y == refY)
				why += "(Steve)";
		}
		hslog::info("ground at %d %d %d (Steve's column, its middle; scan from %.0f down to %.0f cm):", x, refY, z, zTop, zBot);
		for (size_t a = 0, b; a < why.size(); a = b + 1)
		{
			b = why.find('\n', a);
			if (b == std::string::npos)
				b = why.size();
			if (b > a)
				hslog::info("%s", why.substr(a, b - a).c_str());
		}
	}

	// ---- Steve's blows on Half Sword's loose things ----
	constexpr double kPropImpulse = 4000.0; // kg cm/s a full blow gives (a 10 kg candle stand: 4 m/s)
	constexpr double kPropMaxSpeed = 1200.0; // cm/s, the most a full blow gives something light (a weapon, a chain link)
	constexpr float kPropBehind = 20.0f;     // cm: a loose thing this far behind the first thing hit (the floor it lies on) still is

	/// {"t":"swing","o":[eyes],"d":[look],"r":reach,"s":charge}: Steve swung at no mob or fighter. The first thing on that
	/// line, as far as he reaches: if it's loose (simulating physics: a candle stand, a chain, a weapon on the floor, not a
	/// fighter), it's pushed the way he swung, at the point he hit, as Minecraft knocks things back (a little up too),
	/// charged as his blow was. Walls and Minecraft's own blocks stop the blow.
	void on_swing(SDK::UWorld *world, const std::string &m)
	{
		double o[3], d[3];
		if (g_cfg.props <= 0.0 || json_nums(m, "o", o, 3) != 3 || json_nums(m, "d", d, 3) != 3)
			return;
		const double reach = json_num(m, "r", 3.5) * 100.0, charge = std::clamp(json_num(m, "s", 1.0), 0.0, 1.0);
		const SDK::FVector from = to_ue(o[0], o[1], o[2]);
		SDK::FVector dir(d[0], d[2], d[1]);
		const double len = std::sqrt(dir.X * dir.X + dir.Y * dir.Y + dir.Z * dir.Z);
		if (len < 1e-6)
			return;
		dir = SDK::FVector(dir.X / len, dir.Y / len, dir.Z / len);
		const SDK::FVector to(from.X + dir.X * reach, from.Y + dir.Y * reach, from.Z + dir.Z * reach);
		// everything (the map, its blocks, loose things), and loose things alone: Half Sword's weapons are of its own
		// collision types (custom ones: ObjectTypeQuery7 on), and one lying on the floor is reached by the floor first -
		// it still counts when it's no further than kPropBehind past the first thing hit
		using Q = SDK::EObjectTypeQuery;
		Q all[] = {Q::ObjectTypeQuery1, Q::ObjectTypeQuery2, Q::ObjectTypeQuery4, Q::ObjectTypeQuery6, Q::ObjectTypeQuery7, Q::ObjectTypeQuery8,
			Q::ObjectTypeQuery9, Q::ObjectTypeQuery10, Q::ObjectTypeQuery11, Q::ObjectTypeQuery12};
		Q loose[] = {Q::ObjectTypeQuery2, Q::ObjectTypeQuery4, Q::ObjectTypeQuery6, Q::ObjectTypeQuery7, Q::ObjectTypeQuery8, Q::ObjectTypeQuery9,
			Q::ObjectTypeQuery10, Q::ObjectTypeQuery11, Q::ObjectTypeQuery12};
		SDK::TArray<Q> allTypes(all, 10, 10), looseTypes(loose, 9, 9);
		SDK::AActor *skip[1] = {g_standIn.ref.alive() ? static_cast<SDK::AActor *>(g_standIn.ref.obj) : nullptr};
		SDK::TArray<SDK::AActor *> ignore(skip, skip[0] != nullptr ? 1 : 0, 1);
		auto pushable = [](const SDK::FHitResult &h) {
			SDK::UPrimitiveComponent *pc = h.Component.Get();
			return pc != nullptr && as_willie(pc->GetOwner()) == nullptr && pc->IsSimulatingPhysics(h.BoneName); // (a fighter's hits go through his proxy)
		};
		SDK::FHitResult first{}, hit{};
		const bool any = SDK::UKismetSystemLibrary::SphereTraceSingleForObjects(world, from, to, 8.0f, allTypes, false, ignore, SDK::EDrawDebugTrace::None,
							 &first, true, SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f) && first.bBlockingHit;
		if (!any)
			return;
		if (pushable(first))
			hit = first;
		else if (!(SDK::UKismetSystemLibrary::SphereTraceSingleForObjects(world, from, to, 8.0f, looseTypes, false, ignore, SDK::EDrawDebugTrace::None, &hit,
					   true, SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f) && hit.bBlockingHit && pushable(hit) && hit.Distance <= first.Distance + kPropBehind))
			return; // a wall, a block, the floor
		SDK::UPrimitiveComponent *c = hit.Component.Get();
		SDK::AActor *owner = c->GetOwner();
		const double mass = std::max(double(c->GetMass()), 0.2);
		const double k = g_cfg.props * (0.2 + 0.8 * charge * charge); // Minecraft's own: an uncharged blow is a fifth
		const double speed = std::min(kPropImpulse * k / mass, kPropMaxSpeed * k);
		SDK::FVector push(dir.X, dir.Y, dir.Z + 0.15);
		const double plen = std::sqrt(push.X * push.X + push.Y * push.Y + push.Z * push.Z);
		push = SDK::FVector(push.X / plen * speed * mass, push.Y / plen * speed * mass, push.Z / plen * speed * mass);
		c->AddImpulseAtLocation(push, hit.ImpactPoint, hit.BoneName);
		hslog::info("swing: pushed %s (%s, %.1f kg) at %.0f cm/s", owner != nullptr ? owner->GetName().c_str() : "?", c->GetName().c_str(), mass, speed);
	}

	// ---- messages from Minecraft ----
	void handle_event(SDK::UWorld *world, const std::string &m)
	{
		const std::string t = json_str(m, "t");
		if (t == "me")
		{
			double p[3];
			if (json_nums(m, "p", p, 3) == 3)
			{
				g_steve.x = p[0];
				g_steve.y = p[1];
				g_steve.z = p[2];
				g_steve.yaw = float(json_num(m, "y"));
				g_steve.dead = json_bool(m, "dead");
				g_steve.valid = true;
			}
			g_mcScreen = json_bool(m, "scr");
			g_nightVision = float(json_num(m, "nv"));
		}
		else if (t == "hit")
			on_hit(m);
		else if (t == "explosion")
			on_explosion(m);
		else if (t == "cfg")
			on_config(m);
		else if (t == "burn")
			on_burn(m);
		else if (t == "lightning")
			on_lightning(m);
		else if (t == "magic")
			on_magic(m);
		else if (t == "heal")
			on_heal(m);
		else if (t == "fx")
			on_fx(m);
		else if (t == "env")
			on_env(m);
		else if (t == "mobs")
			on_mobs(world, m);
		else if (t == "push")
			on_push(m);
		else if (t == "blocks")
			on_blocks(world, m);
		else if (t == "ride")
			on_ride(m);
		else if (t == "seats")
			on_seats(m);
		else if (t == "swing")
			on_swing(world, m);
		else if (t == "groundwhy")
			ground_why(world);
		else if (t == "resample")
		{
			reset_ground(); // Minecraft lost some of the ground (or cleaned up): send it all again
			hslog::info("ground: sending it again");
		}
		else if (t == "hello")
			hslog::info("minecraft: %s", m.c_str());
	}

	void level_at(SDK::UWorld *world, const SDK::FVector &feet)
	{
		double groundZ = 0.0;
		if (!ground_z(world, feet.X, feet.Y, feet.Z + 100.0, groundZ))
			groundZ = feet.Z;
		g_yOffset = kGroundLevel - groundZ / 100.0;
		g_haveOffset = true;
		reset_ground();
		g_ws.send("{\"t\":\"clear\"}");
		g_startFeet = SDK::FVector(feet.X, feet.Y, groundZ);
		g_tpIn = 15; // the ground around the start streams in first
		move_blocks();
		hslog::info("levelled on %s: ground z %.1f cm -> y %.0f (offset %.3f)", world != nullptr ? world->GetName().c_str() : "?", groundZ, kGroundLevel,
			g_yOffset);
	}

	// The poses the camera actor was aimed with (newest last): the tick finds which one this frame's view used.
	struct Aim
	{
		int64_t frame = -1;
		SDK::FVector loc;
		SDK::FRotator rot;
	};
	Aim g_aims[16];
	unsigned g_aimCount = 0;
	double g_aspect = 16.0 / 9.0;
	bool g_aiming = false; // the bridge is on (set by the tick): ProcessEvent calls between ticks aim too

	/// Aim the camera actor at Minecraft's newest frame, if it's new. Game thread, any time in the frame: the camera
	/// update takes whatever was set last.
	void aim_camera(SDK::UWorld *world, SDK::APlayerController *pc, SDK::APlayerCameraManager *cm)
	{
		McPose cam;
		if (!newest_frame_pose(cam))
		{
			++g_tickStats.readFails;
			return;
		}
		if (cam.frame == g_mcCam.frame && g_camera.alive())
			return;
		++g_tickStats.newFrames;
		g_mcCam = cam;
		const double d2r = 3.14159265358979 / 180.0;
		g_camLoc = to_ue(cam.x, cam.y, cam.z);
		g_camRot = SDK::FRotator(-cam.pitch, cam.yaw + 90.0, 0.0);
		g_camFov = float(2.0 * std::atan(std::tan(cam.fov * d2r * 0.5) * g_aspect) / d2r);
		apply_camera(world, pc, cm);
		g_camFrameSet = cam.frame;
		g_aims[g_aimCount & 15] = {cam.frame, g_camLoc, g_camRot};
		++g_aimCount;
	}

	/// Which aimed frame this view was made from (the closest pose), and how far off it is.
	int64_t aimed_frame(const SDK::FMinimalViewInfo &pov, double &error)
	{
		auto angle = [](double a, double b) { return std::fabs(std::remainder(a - b, 360.0)); };
		int64_t best = g_camFrameSet;
		error = 1e30;
		const unsigned n = std::min(g_aimCount, 16u);
		for (unsigned i = 0; i < n; ++i)
		{
			const Aim &a = g_aims[(g_aimCount - 1 - i) & 15];
			const double dx = pov.Location.X - a.loc.X, dy = pov.Location.Y - a.loc.Y, dz = pov.Location.Z - a.loc.Z;
			const double e = std::sqrt(dx * dx + dy * dy + dz * dz) + angle(pov.Rotation.Yaw, a.rot.Yaw) + angle(pov.Rotation.Pitch, a.rot.Pitch);
			if (e < error)
			{
				error = e;
				best = a.frame;
			}
		}
		return best;
	}

	void set_frame_cap(SDK::UWorld *world, SDK::APlayerController *pc, int fps)
	{
		if (fps == g_fpsApplied)
			return;
		g_fpsApplied = fps;
		wchar_t cmd[64];
		swprintf_s(cmd, L"t.MaxFPS %d", fps);
		SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(cmd), pc);
	}

	/// Half Sword's cap: the fixed one (/hs fps), or with the balance on, a little above Minecraft's rate.
	void update_frame_cap(SDK::UWorld *world, SDK::APlayerController *pc)
	{
		if (g_shm == nullptr)
			return;
		const ULONGLONG now = GetTickCount64();
		if (now - g_balanceSince < 500)
			return;
		const int64_t published = rd<int64_t>(g_shm + 32);
		if (g_balancePublished >= 0 && g_balanceSince != 0)
		{
			const double fps = double(published - g_balancePublished) * 1000.0 / double(now - g_balanceSince);
			g_mcFps = g_mcFps <= 0 ? fps : g_mcFps * 0.6 + fps * 0.4;
		}
		g_balancePublished = published;
		g_balanceSince = now;
		int cap = g_cfg.fps;
		if (cap == 0 && g_cfg.balance && g_mcFps > 5)
		{
			cap = std::clamp(int(g_mcFps * 1.15 + 0.5), 40, 250);
			if (g_fpsApplied != 0 && std::abs(cap - g_fpsApplied) < 3)
				cap = g_fpsApplied; // (no churn over a frame or two)
		}
		if (cap != g_fpsApplied)
			hslog::info("frame cap %d (Minecraft %.0f fps%s)", cap, g_mcFps, g_cfg.fps ? ", fixed" : g_cfg.balance ? ", balance" : "");
		set_frame_cap(world, pc, cap);
	}

	void tick(SDK::UWorld *world, SDK::APlayerController *pc, SDK::APlayerCameraManager *cm)
	{
		++g_frame;
		input::attach();
		if (input::take_hotkey(VK_F9))
		{
			g_enabled = !g_enabled;
			hslog::info("passthrough %s", g_enabled ? "on" : "off");
		}
		if (input::take_hotkey(VK_F7))
			g_userHalfSword = !g_userHalfSword;
		// (only counted while Minecraft drives, below: Escape with all this off is Half Sword's alone, and toggling the
		// guess then left it wrong when it came back on - the input stayed with Half Sword for good)
		const bool escape = input::take_escape_to_half_sword();
		const bool relevel = input::take_hotkey(VK_F8);
		if (input::take_hotkey(VK_F10))
		{
			compositor::set_pose_lag((compositor::pose_lag() + 1) % 4);
			hslog::info("pose lag %d frames", compositor::pose_lag());
		}

		if (world != g_world)
		{
			// a new map: the ground starts over where the match starts
			g_world = world;
			g_mobBodies.clear(); // (their actors went with the old world)
			g_mobBodyOf.clear();
			g_aiming = false;
			g_aimCount = 0;
			g_haveOffset = false;
			g_fighters.clear();
			g_scanFighters.clear();
			g_scanAt = 0;
			g_carried.clear();
			g_camera = {};
			g_hadFighter = g_matchOver = false;
			g_overTicks = 0;
			input::share_continue_key(false);
			g_blocks.clear(); // (the boxes went with the old world)
			g_riders.clear(); // (and the riders' planks, the riders)
			g_savedJoints.clear(); // (the given-back body went too)
			g_savedJointsMesh = {};
			g_unrideUntil.clear();
			g_blockActor = {};
			g_burning.clear();
			g_effects.clear(); // (the fighters went with the old world)
			g_surroundings.clear();
			g_fallUntil.clear();
			g_fireActor = {};
			g_fireSource = nullptr;
			g_steveHits.clear();
			g_hitEvents.clear(); // (found again: the classes may have been loaded anew)
			g_standInObj = nullptr;
			g_worldSetup = false;
			g_standIn = {};
			g_halfSwordMenu = g_menuSeen = false;
			g_willieClass = SDK::AWillie_BP_C::StaticClass();
		}
		if (g_willieClass == nullptr)
			g_willieClass = SDK::AWillie_BP_C::StaticClass();

		auto *w = as_willie(pc->Pawn);
		const bool on = g_enabled && g_ws.connected() && w != nullptr;
		compositor::set_active(on);
		if (!on)
		{
			g_aiming = false;
			clear_mob_bodies();
			clear_effects();
			release_riders("passthrough off");
			release_camera(pc, pc->Pawn);
			restore_stand_in();
			settle_restored();
			input::route_to_minecraft(false);
			g_halfSwordMenu = g_menuSeen = false; // (on again: Minecraft's input, whatever Escape did meanwhile)
			set_frame_cap(world, pc, g_cfg.fps); // (menus: the player's own cap)
			g_balanceSince = 0;
			g_balancePublished = -1;
			std::string ignored;
			while (g_ws.poll(ignored))
			{
			}
			return;
		}
		if (!g_worldSetup)
		{
			// camera motion blur smears Half Sword's picture but never Minecraft's: the two would look apart
			g_worldSetup = true;
			SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"r.MotionBlurQuality 0"), pc);
			SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"r.DefaultFeature.MotionBlur 0"), pc);
			// a fixed render pipeline: the game thread waits until each frame is presented, so the camera pose
			// recorded for a frame is always the one the presented picture used (UE5 otherwise drifts 1-2 frames)
			SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"r.OneFrameThreadLag 0"), pc);
			SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"r.GTSyncType 2"), pc);
			hslog::info("cvars: r.OneFrameThreadLag %d, r.GTSyncType %d, r.MotionBlurQuality %d",
				SDK::UKismetSystemLibrary::GetConsoleVariableIntValue(SDK::FString(L"r.OneFrameThreadLag")),
				SDK::UKismetSystemLibrary::GetConsoleVariableIntValue(SDK::FString(L"r.GTSyncType")),
				SDK::UKismetSystemLibrary::GetConsoleVariableIntValue(SDK::FString(L"r.MotionBlurQuality")));
		}

		update_frame_cap(world, pc);

		if (g_ws.generation() != g_generation)
		{
			// (re)connected: size Minecraft's window to the picture, start the ground over
			g_generation = g_ws.generation();
			g_haveOffset = false;
			g_viewSent = 0;
		}
		sendf("{\"t\":\"own\",\"f\":%llu}", g_frame);
		int bw = 0, bh = 0;
		compositor::backbuffer_size(bw, bh);
		if (bw > 0 && bh > 0 && (bw * 65536 + bh) != g_viewSent)
		{
			g_viewSent = bw * 65536 + bh;
			const double scale = std::min(1.0, std::sqrt(kMaxMinecraftPixels / (double(bw) * bh)));
			sendf("{\"t\":\"view\",\"w\":%d,\"h\":%d}", int(bw * scale + 0.5), int(bh * scale + 0.5));
		}

		// input: Minecraft's, unless Half Sword's menu is open or the player gave it to Half Sword (F7)
		input::set_minecraft_screen(g_mcScreen);
		// and while Half Sword shows its mouse cursor (a menu: after a death, the next-fight choice...)
		if (escape && !g_userHalfSword)
		{
			g_halfSwordMenu = !g_halfSwordMenu;
			g_menuSeen = false;
		}
		// Half Sword's menu is up while it shows its cursor or pauses the game: once it has been up and is gone again
		// (closed with Escape, Resume, anything), the input is Minecraft's again
		if (g_halfSwordMenu)
		{
			if (pc->bShowMouseCursor || SDK::UGameplayStatics::IsGamePaused(world))
				g_menuSeen = true;
			else if (g_menuSeen)
			{
				g_halfSwordMenu = g_menuSeen = false;
				hslog::info("Half Sword's menu closed");
			}
		}
		input::route_to_minecraft(!g_userHalfSword && !g_halfSwordMenu && !pc->bShowMouseCursor);
		hold_give_up();
		input::flush(); // (the input goes out as it happens: anything left over)

		if (!g_haveOffset)
		{
			level_at(world, feet_of(w));
			g_standIn = {};
		}
		else if (relevel && g_steve.valid)
			level_at(world, to_ue(g_steve.x, g_steve.y, g_steve.z));
		// Steve goes to the start once the ground there has streamed in: the columns around it scanned and sent (put
		// there before, he fell into the gap and the ground then appeared around him - he spawned inside it). At most
		// ~5 s of waiting.
		static int tpWaited = 0;
		bool tpNow = false;
		if (g_tpIn > 0)
		{
			--g_tpIn;
			tpWaited = 0;
		}
		else if (g_tpIn == 0)
		{
			double sx, sy, sz;
			to_mc(g_startFeet, sx, sy, sz);
			const int bx = int(std::floor(sx)), by = int(std::floor(sy)), bz = int(std::floor(sz));
			bool ready = true;
			for (int dx = -2; dx <= 2 && ready; ++dx)
				for (int dz = -2; dz <= 2 && ready; ++dz)
				{
					// scanned for the start's height (a scan around somewhere else leaves the start's floor out)
					auto col = g_scanned.find(column_key(bx + dx, bz + dz));
					ready = col != g_scanned.end() && std::abs(col->second.refY - by) < kRescanCells;
				}
			if (ready || ++tpWaited > 300)
			{
				if (!ready)
					hslog::info("the ground around the start still isn't all scanned: Steve goes there anyway");
				g_tpIn = -1;
				tpNow = true;
			}
		}
		if (tpNow)
		{
			double x, y, z;
			to_mc(g_startFeet, x, y, z);
			sendf("{\"t\":\"cmd\",\"c\":\"tp @a %.3f %.3f %.3f\"}", x, y + 0.05, z);
			sendf("{\"t\":\"cmd\",\"c\":\"setworldspawn %d %d %d\"}", int(std::floor(x)), int(std::floor(y + 0.05)), int(std::floor(z)));
			sendf("{\"t\":\"cmd\",\"c\":\"spawnpoint @a %d %d %d\"}", int(std::floor(x)), int(std::floor(y + 0.05)), int(std::floor(z)));
			sendf("{\"t\":\"blocksync\",\"r\":%d}", kBlocksRadius); // Minecraft's blocks around there, solid here
			g_steve.valid = false;
			hslog::info("Steve to the start: %.2f %.2f %.2f", x, y, z);
		}

		keep_stand_in(w);
		keep_mob_bodies();
		refresh_fighters(world, w); // (a slice of the scan every tick)
		keep_riders(world);
		send_fighters(world);
		burn_fighters(world);
		apply_effects();
		apply_surroundings();
		for (auto it = g_fallUntil.begin(); it != g_fallUntil.end();)
		{
			if (GetTickCount64() < it->second)
			{
				++it;
				continue;
			}
			if (auto *f = fighter_by_handle(it->first); f != nullptr && g_burning.count(it->first) == 0)
				f->Force_Fall = false;
			it = g_fallUntil.erase(it);
		}


		// Half Sword's camera = the camera of Minecraft's newest frame. The camera actor is aimed at each new frame as
		// it's published (aim_camera, from every ProcessEvent): what this frame's view was made from is in the camera
		// cache now (the update ran just before us); find which frame that was: the one paired with this frame.
		TickStats &ts = g_tickStats;
		++ts.ticks;
		if (GetTickCount64() - ts.since >= 2000)
		{
			if (ts.since != 0)
				hslog::info("bridge: %d ticks, camera matched %d (error %.4f), aimed between ticks %d, newest Minecraft frame changed %d times, "
					"read failed %d", ts.ticks, ts.matched, ts.matched ? ts.matchErr / ts.matched : 0.0, ts.between, ts.newFrames, ts.readFails);
			// frame generation (DLSS/FSR) presents made-up frames between the game's: Minecraft's picture only
			// follows the real ones, and the extra latency is large. Say so in the log.
			const uint64_t presents = compositor::presents();
			if (ts.since != 0 && ts.ticks > 20 && double(presents - ts.presentsAt) > 1.6 * ts.ticks)
				hslog::info("WARNING: %llu frames presented for %d game frames: frame generation is on? Turn it off in Half Sword's "
					"settings (Minecraft follows only the real frames, and it adds latency)", presents - ts.presentsAt, ts.ticks);
			ts = TickStats{};
			ts.since = GetTickCount64();
			ts.presentsAt = presents;
		}
		g_aspect = bw > 0 && bh > 0 ? double(bw) / bh : 16.0 / 9.0;
		if (g_mcCam.valid && g_aimCount > 0)
		{
			const double d2r = 3.14159265358979 / 180.0;
			const SDK::FMinimalViewInfo &pov = cm->CameraCachePrivate.POV;
			double err = 0;
			const int64_t shownFrame = aimed_frame(pov, err);
			++ts.matched;
			ts.matchErr += err;
			double hx, hy, hz;
			to_mc(pov.Location, hx, hy, hz);
			const float vfov = float(2.0 * std::atan(std::tan(pov.FOV * d2r * 0.5) / g_aspect) / d2r);
			compositor::set_host_pose(wrap_degrees(pov.Rotation.Yaw - 90.0), float(-pov.Rotation.Pitch), float(-pov.Rotation.Roll), vfov, hx, hy, hz,
				shownFrame);
		}
		aim_camera(world, pc, cm);
		g_aiming = true;
		if (g_mcCam.valid)
		{
			// Half Sword's own camera shakes (steps, hits) would move its picture off Minecraft's: Steve's camera is Minecraft's
			cm->StopAllCameraShakes(true);
			auto *engine = SDK::UEngine::GetEngine();
			compositor::set_host_planes(engine != nullptr ? engine->NearClipPlane / 100.0f : 0.001f, 1.0e7f);
		}

		// until Steve is at the start, the ground around the start (wherever he is meanwhile: in the void, as the old
		// ground went - scanned around him, the start's floor was out of the window and he arrived to no collision)
		if (g_tpIn >= 0)
			sample_ground(world, g_startFeet);
		else if (g_steve.valid)
			sample_ground(world, to_ue(g_steve.x, g_steve.y, g_steve.z));


		std::string message;
		while (g_ws.poll(message))
			handle_event(world, message);
	}
}

namespace bridge
{
	void start()
	{
		QueryPerformanceFrequency(&g_freq);
		g_ws.start("127.0.0.1", kPort);
		input::set_sender([](const std::string &m) {
			if (g_ws.connected())
				g_ws.send(m);
		});
		hslog::info("bridge started, Minecraft link on 127.0.0.1:%d", kPort);
	}

	// ---- Willies mining Minecraft's blocks ----
	constexpr double kMineWeaponSpeed = 250.0; // cm/s: a weapon this fast into a block chips it
	constexpr double kMineBodySpeed = 450.0;   // a fist or a kick (walking into one, standing on one: slower)
	constexpr ULONGLONG kMineCooldownMs = 200; // one blow, however many frames it touches
	std::unordered_map<int64_t, ULONGLONG> g_minedAt;

	/// A Willie's weapon, fist or foot hitting one of Minecraft's blocks (their boxes on the block actor): {"t":"mine",
	/// "c":[x,y,z],"v":m/s} - Minecraft wears the block down by how hard, and breaks it. Not the stand-in nor a mob's
	/// body (Steve and the mobs are Minecraft's own).
	void note_block_hit(const void *object, const void *function, void *parms)
	{
		const HitEvent *e = nullptr;
		for (const HitEvent &h : g_hitEvents)
			if (h.fn == function)
				e = &h;
		if (e == nullptr || e->kind == HitKind::Overlap)
			return;
		auto *p = static_cast<HitParms *>(parms);
		if (p->otherActor == nullptr || p->otherActor != g_blockActor.obj || p->hitComponent == nullptr || !g_blockActor.alive())
			return;
		if ((g_standInObj != nullptr && object == g_standInObj) || g_mobBodyOf.count(static_cast<const SDK::UObject *>(object)))
			return;
		const SDK::FVector v = p->hitComponent->GetPhysicsLinearVelocity(p->hit.MyBoneName);
		const double speed = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
		if (speed < (e->kind == HitKind::Body ? kMineBodySpeed : kMineWeaponSpeed))
			return;
		// the block is the one behind the face that was hit
		const SDK::FVector in(p->hit.ImpactPoint.X - p->hit.ImpactNormal.X * 5.0, p->hit.ImpactPoint.Y - p->hit.ImpactNormal.Y * 5.0,
			p->hit.ImpactPoint.Z - p->hit.ImpactNormal.Z * 5.0);
		double mx, my, mz;
		to_mc(in, mx, my, mz);
		const int bx = int(std::floor(mx)), by = int(std::floor(my)), bz = int(std::floor(mz));
		const ULONGLONG now = GetTickCount64();
		if (g_minedAt.size() > 4096)
			g_minedAt.clear();
		ULONGLONG &last = g_minedAt[cell_key(bx, by, bz)];
		if (now - last < kMineCooldownMs)
			return;
		last = now;
		sendf("{\"t\":\"mine\",\"c\":[%d,%d,%d],\"v\":%.2f}", bx, by, bz, speed / 100.0);
	}

	bool filter_event(const void *object, const void *function, void *parms)
	{
		if (block_script(object, function, parms))
			return true;
		if (parms != nullptr && g_blockActor.obj != nullptr)
			note_block_hit(object, function, parms);
		const bool settling = object != nullptr && object == g_restored.obj && g_restored.alive() &&
			(g_restorePending || GetTickCount64() - g_restoredAt <= kRestoreSettleMs);
		if ((g_standInObj == nullptr && g_mobBodyOf.empty() && !settling) || parms == nullptr)
			return false;
		const HitEvent *e = nullptr;
		for (const HitEvent &h : g_hitEvents)
			if (h.fn == function)
				e = &h;
		if (e == nullptr)
			return false;
		// the body just given back feels nothing of its own while it settles (it landing on the ground put its back
		// out, 0.3 s in): only its own hits, a fighter's blow on it still lands
		if (settling)
		{
			static ULONGLONG said = 0;
			if (GetTickCount64() - said > 500)
			{
				said = GetTickCount64();
				hslog::info("the body given back: its own hit ignored while it settles, %llu ms in", GetTickCount64() - g_restoredAt);
			}
			return true;
		}
		if ((g_standInObj != nullptr && object == g_standInObj) || g_mobBodyOf.count(static_cast<const SDK::UObject *>(object)))
			return true; // the stand-in and the mobs' bodies feel nothing themselves: the floor, a landing, a weapon touching them
		auto *p = static_cast<HitParms *>(parms);
		if (e->kind == HitKind::Overlap || p->hitComponent == nullptr)
			return false;
		std::vector<SteveHit> *hits = nullptr;
		if (g_standInObj != nullptr && p->otherActor == g_standInObj)
			hits = &g_steveHits;
		else if (auto it = g_mobBodyOf.find(p->otherActor); it != g_mobBodyOf.end())
			hits = &g_mobBodies[it->second].hits;
		if (hits == nullptr)
			return false;
		if (hits->size() < 64)
			hits->push_back({{p->hitComponent, p->hitComponent->Index}, e->kind, p->hit.ImpactPoint, p->hit.BoneName, p->hit.MyBoneName});
		return true; // Steve's and the mobs' damage is Minecraft's: Half Sword never applies this hit
	}

	/// Between frames (every game-thread event): the stand-in stays exactly where it was put for Steve. Anything of Half
	/// Sword's that moves it (back to the ground) is undone before the physics, where the fighters' blows land.
	void hold_stand_in()
	{
		static bool busy = false; // (setting it is an event of its own)
		if (busy || !g_standInPlaced.valid || g_standInObj == nullptr || !g_standIn.ref.alive() || g_standIn.ref.obj != g_standInObj)
			return;
		auto *w = as_willie(g_standIn.ref.obj);
		if (w == nullptr || w->DED)
			return;
		const SDK::FVector now = w->K2_GetActorLocation();
		const SDK::FVector &at = g_standInPlaced.at;
		if (std::abs(now.Z - at.Z) > 10.0 || std::abs(now.X - at.X) > 10.0 || std::abs(now.Y - at.Y) > 10.0)
		{
			busy = true;
			SDK::FHitResult hit{};
			w->K2_SetActorLocation(at, false, &hit, true);
			busy = false;
			++g_standInHeld;
		}
	}

	/// After any event of the stand-in's own (its per-frame logic above all): if that moved it, back where Steve is.
	void after_event(const void *object)
	{
		if (object != nullptr && object == g_standInObj)
			hold_stand_in();
	}

	void on_process_event()
	{
		if (g_inTick)
			return;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (now.QuadPart < g_nextCheck)
			return;
		g_inTick = true;
		g_nextCheck = now.QuadPart + g_freq.QuadPart / 2000; // look again in 0.5 ms
		SDK::UWorld *world = SDK::UWorld::GetWorld();
		SDK::APlayerController *pc = nullptr;
		if (world != nullptr && world->OwningGameInstance != nullptr && world->OwningGameInstance->LocalPlayers.Num() > 0 &&
			world->OwningGameInstance->LocalPlayers[0] != nullptr)
			pc = world->OwningGameInstance->LocalPlayers[0]->PlayerController;
		SDK::APlayerCameraManager *cm = pc != nullptr ? pc->PlayerCameraManager : nullptr;
		// the camera cache's timestamp changes once per frame, after the camera update: tick on the first call after it
		if (cm != nullptr && cm->CameraCachePrivate.Timestamp != g_lastStamp)
		{
			g_lastStamp = cm->CameraCachePrivate.Timestamp;
			tick(world, pc, cm);
		}
		else if (g_aiming && cm != nullptr && world == g_world)
		{
			// between ticks: a newer Minecraft frame goes to the camera at once (the next camera update uses it)
			const unsigned before = g_aimCount;
			aim_camera(world, pc, cm);
			if (g_aimCount != before)
				++g_tickStats.between;
		}
		if (world == g_world)
			hold_stand_in();

		g_inTick = false;
	}

	/// Steve's stand-in loses the fight only when Steve dies in Minecraft. Half Sword also ended fights as lost on its
	/// own (its give-up, a few seconds into a fight: "the G loop"): Willie_BP_C::Event_Lose_Match on the stand-in while
	/// Steve lives is skipped. Logged, with the stand-in's other give-up / lose / fear functions that ran (what led to it).
	/// Any other object's function that sounds like ending a fight (game mode, widgets, controller): logged, so the
	/// route Half Sword takes when it ends a fight on its own can be found (game thread only).
	void watch_fight_end(const void *context, const void *function)
	{
		static std::unordered_map<const void *, std::string> seen; // function -> its name, if it's one to log
		auto it = seen.find(function);
		if (it == seen.end())
		{
			std::string n = static_cast<const SDK::UObject *>(function)->GetName(), low;
			for (char ch : n)
				if (std::isalnum(static_cast<unsigned char>(ch)))
					low += char(std::tolower(static_cast<unsigned char>(ch)));
			static const char *const words[] = {"losematch", "matchlost", "youlose", "youlost", "give", "surrend", "restart", "defeat", "endmatch", "matchend", "endfight",
				"fightend", "nextfight", "newfight", "startfight", "startmatch", "endround", "roundend", "gameover", "yield", "forfeit"};
			bool interesting = false;
			for (const char *w : words)
				interesting = interesting || low.find(w) != std::string::npos;
			it = seen.emplace(function, interesting ? n : std::string()).first;
		}
		if (it->second.empty())
			return;
		static ULONGLONG said = 0;
		static const void *last = nullptr;
		if (function == last && GetTickCount64() - said < 2000)
			return;
		said = GetTickCount64();
		last = function;
		const auto *o = static_cast<const SDK::UObject *>(context);
		hslog::info("%s runs %s (Steve %s, match over %d, input %s)", o->Class != nullptr ? o->Class->GetName().c_str() : "?", it->second.c_str(),
			g_steve.dead ? "dead" : "alive", int(g_matchOver), input::routed_to_minecraft() ? "Minecraft's" : "Half Sword's");
	}

	/// The last game functions run (game thread), dumped when Half Sword's loss screen first shows up in a fight
	/// while Steve is alive: what put it up.
	struct RecentCall
	{
		const void *cls;
		const void *fn;
		bool standIn;
	};
	RecentCall g_recent[64] = {};
	unsigned g_recentAt = 0;
	const void *g_loseDumpedFor = nullptr; // the stand-in the dump was made for (one per fight)

	void note_call(const void *context, const void *function)
	{
		const auto *o = static_cast<const SDK::UObject *>(context);
		// only calls not among the last 64 kept (per-frame ticks would fill it otherwise): rare calls stand out
		static std::unordered_map<uint64_t, unsigned> keptAt;
		const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(function)) * 31 ^ uint64_t(reinterpret_cast<uintptr_t>(o->Class));
		auto k = keptAt.find(key);
		if (k == keptAt.end() || g_recentAt - k->second >= 64)
		{
			keptAt[key] = g_recentAt;
			g_recent[g_recentAt++ & 63] = {o->Class, function, context == g_standInObj};
		}
		if (keptAt.size() > 20000)
			keptAt.clear();
		static std::unordered_map<const void *, bool> loseClass;
		auto it = loseClass.find(o->Class);
		if (it == loseClass.end())
			it = loseClass.emplace(o->Class, o->Class != nullptr && o->Class->GetName().find("UI_Lose") != std::string::npos).first;
		if (!it->second || g_standInObj == nullptr || g_loseDumpedFor == g_standInObj)
			return;
		g_loseDumpedFor = g_standInObj;
		auto *w = (SDK::AWillie_BP_C *)(g_standInObj);
		hslog::info("LOSS SCREEN while Steve is %s: stand-in health %.1f, consciousness %.1f, DED %d, give up %d; the last calls before it:",
			g_steve.dead ? "dead" : "alive", w->Health, w->Consciousness, int(w->DED), int(w->Give_Up));
		for (unsigned i = 0; i < 64; ++i)
		{
			const RecentCall &c = g_recent[(g_recentAt + i) & 63];
			if (c.fn == nullptr)
				continue;
			const auto *cls = static_cast<const SDK::UClass *>(c.cls);
			hslog::info("  %s%s :: %s", c.standIn ? "[stand-in] " : "", cls != nullptr ? cls->GetName().c_str() : "?",
				static_cast<const SDK::UObject *>(c.fn)->GetName().c_str());
		}
	}

	/// The arena map's own trigger volumes (its level Blueprint's ActorBeginOverlap events) touched by the stand-in:
	/// one of them gives up the fight (seen: Map_Arena_Yard TriggerVolume_2 -> Give_Up on, loss screen, 1 s into a
	/// fight while the stand-in's body was being set up). While Steve is alive, the stand-in never sets them off.
	bool block_map_trigger(const void *context, const void *function, const void *parms)
	{
		if (parms == nullptr || g_standInObj == nullptr)
			return false;
		static std::unordered_map<const void *, bool> overlapFn; // function -> a level Blueprint's begin-overlap event
		auto it = overlapFn.find(function);
		if (it == overlapFn.end())
		{
			const auto *o = static_cast<const SDK::UObject *>(context);
			const std::string n = static_cast<const SDK::UObject *>(function)->GetName();
			const bool levelScript = o->IsA(SDK::ALevelScriptActor::StaticClass());
			it = overlapFn.emplace(function, levelScript && n.find("BeginOverlap") != std::string::npos).first;
		}
		if (!it->second)
			return false;
		// (OverlappedActor, OtherActor): the ActorBeginOverlap signature
		const void *other = *reinterpret_cast<const void *const *>(static_cast<const uint8_t *>(parms) + 8);
		const void *self = *reinterpret_cast<const void *const *>(parms);
		// a Minecraft mob's invisible body: never sets off the map's triggers (one gives up the fight; for a body on
		// the mobs' team that counted an enemy down and won the match the moment a zombie spawned)
		const auto *otherObj = static_cast<const SDK::UObject *>(other), *selfObj = static_cast<const SDK::UObject *>(self);
		if ((otherObj != nullptr && g_mobBodyOf.count(otherObj)) || (selfObj != nullptr && g_mobBodyOf.count(selfObj)))
		{
			static ULONGLONG saidMob = 0;
			if (GetTickCount64() - saidMob > 2000)
			{
				saidMob = GetTickCount64();
				const auto *o = static_cast<const SDK::UObject *>(context);
				hslog::info("a mob's body touched %s's trigger (%s): blocked", o->Class != nullptr ? o->Class->GetName().c_str() : "?",
					static_cast<const SDK::UObject *>(function)->GetName().c_str());
			}
			return true;
		}
		if (other != g_standInObj && self != g_standInObj)
			return false;
		if (g_steve.dead || g_steveWasDead)
			return false;
		static ULONGLONG said = 0;
		if (GetTickCount64() - said > 1000)
		{
			said = GetTickCount64();
			const auto *o = static_cast<const SDK::UObject *>(context);
			hslog::info("the stand-in touched %s's trigger (%s) while Steve is alive: blocked (it gives up the fight)",
				o->Class != nullptr ? o->Class->GetName().c_str() : "?", static_cast<const SDK::UObject *>(function)->GetName().c_str());
		}
		return true;
	}

	/// The game mode's Minus_Enemy (an enemy is out) when the fighter whose script called it is a Minecraft mob's body:
	/// those aren't the match's enemies (spawn_mob_body keeps them out of Enemy_Count), so it must not count down.
	std::vector<const void *> g_scriptStack; // the objects whose Blueprint code is running, innermost last (game thread)

	bool block_mob_minus_enemy(const void *context, const void *function)
	{
		// (looked up by name: a search of every object, so only while fighters exist and at most once a second until
		// found - every call doing it froze the game in its menus, where there is no game mode to find)
		static const void *minusEnemy = nullptr;
		static ULONGLONG lookedAt = 0;
		if (g_willieClass == nullptr || g_mobBodyOf.empty())
			return false;
		if (minusEnemy == nullptr)
		{
			const ULONGLONG now = GetTickCount64();
			if (now - lookedAt < 1000)
				return false;
			lookedAt = now;
			minusEnemy = SDK::UObject::FindObjectFast<SDK::UFunction>("Minus_Enemy", SDK::EClassCastFlags::Function);
			if (minusEnemy == nullptr)
				return false;
		}
		if (function != minusEnemy)
			return false;
		// the innermost fighter whose code is calling it
		const void *caller = nullptr;
		for (auto it = g_scriptStack.rbegin(); it != g_scriptStack.rend() && caller == nullptr; ++it)
		{
			const auto *o = static_cast<const SDK::UObject *>(*it);
			if (o != nullptr && o->Class != nullptr && o->Class->IsSubclassOf(g_willieClass))
				caller = *it;
		}
		if (caller == nullptr || !g_mobBodyOf.count(static_cast<const SDK::UObject *>(caller)))
			return false;
		hslog::info("a mob's body counted itself out of the match (Minus_Enemy): blocked");
		return true;
	}

	/// The body just given back (restore_stand_in), its first moments: Half Sword breaking its bones then is the
	/// physics coming back on, not a blow - Break_Leg_L/R (it was always a leg), arms, back, head, Snap_Neck are skipped,
	/// and so is Half Sword's own joint check (Event Check Bone Dislocation Status: it put both legs out 94-125 ms in,
	/// with every joint unbreakable).
	bool block_restore_break(const void *context, const void *function)
	{
		if (context != g_restored.obj || !g_restored.alive() || (!g_restorePending && GetTickCount64() - g_restoredAt > kRestoreSettleMs))
			return false;
		static std::unordered_map<const void *, std::string> breaks; // function -> its name if it breaks a bone, else ""
		auto it = breaks.find(function);
		if (it == breaks.end())
		{
			std::string n = static_cast<const SDK::UObject *>(function)->GetName(), low;
			for (char ch : n)
				if (std::isalnum(static_cast<unsigned char>(ch)))
					low += char(std::tolower(static_cast<unsigned char>(ch)));
			static const char *kBreaks[] = {"breaklegl", "breaklegr", "breakarml", "breakarmr", "breakback", "breakhead", "snapneck",
				"eventcheckbonedislocationstatus"};
			bool found = false;
			for (const char *b : kBreaks)
				found = found || low == b;
			it = breaks.emplace(function, found ? n : std::string()).first;
		}
		if (it->second.empty())
			return false;
		hslog::info("the body given back would run %s %llu ms in: skipped", it->second.c_str(), GetTickCount64() - g_restoredAt);
		return true;
	}

	bool block_script(const void *context, const void *function, const void *parms)
	{
		if (context != nullptr && function != nullptr)
			note_call(context, function);
		if (context != nullptr && function != nullptr && block_mob_minus_enemy(context, function))
			return true;
		if (context != nullptr && function != nullptr && block_map_trigger(context, function, parms))
			return true;
		if (context != nullptr && function != nullptr && block_restore_break(context, function))
			return true;
		if (context != nullptr && function != nullptr && context != g_standInObj && g_standInObj != nullptr)
			watch_fight_end(context, function);
		if (context == nullptr || context != g_standInObj || g_willieClass == nullptr)
			return false;
		static SDK::UClass *cls = nullptr;
		static const void *loseMatch = nullptr;
		static std::unordered_map<const void *, std::string> watched; // function -> its name, if it's one to log
		if (cls != g_willieClass)
		{
			cls = g_willieClass;
			loseMatch = nullptr;
			watched.clear();
			for (SDK::UStruct *c = g_willieClass; c != nullptr && loseMatch == nullptr; c = c->SuperStruct)
				for (SDK::UField *f = c->Children; f != nullptr; f = f->Next)
					if (f->IsA(SDK::EClassCastFlags::Function))
					{
						// (the game's own name has spaces, "Event Lose Match": compare letters only)
						std::string n;
						for (char ch : f->GetName())
							if (std::isalnum(static_cast<unsigned char>(ch)))
								n += char(std::tolower(static_cast<unsigned char>(ch)));
						if (n == "eventlosematch")
							loseMatch = f;
					}
			hslog::info("lose-match guard: %s", loseMatch != nullptr ? "on" : "Event_Lose_Match not found");
		}
		auto *w = static_cast<SDK::AWillie_BP_C *>(const_cast<void *>(context));
		auto it = watched.find(function);
		if (it == watched.end())
		{
			std::string n = static_cast<const SDK::UObject *>(function)->GetName(), low = n;
			for (char &ch : low)
				ch = char(std::tolower(static_cast<unsigned char>(ch)));
			const bool interesting = low.find("give") != std::string::npos || low.find("lose") != std::string::npos ||
				low.find("surrend") != std::string::npos || low.find("yield") != std::string::npos || low.find("fear") != std::string::npos ||
				low.find("talk") != std::string::npos || low.find("death") != std::string::npos;
			it = watched.emplace(function, interesting ? n : std::string()).first;
		}
		if (!it->second.empty())
		{
			static ULONGLONG said = 0;
			static const void *last = nullptr;
			if (function != last || GetTickCount64() - said > 2000)
			{
				said = GetTickCount64();
				last = function;
				hslog::info("stand-in runs %s (Steve %s, health %.0f, give up %d)", it->second.c_str(), g_steve.dead ? "dead" : "alive", w->Health,
					int(w->Give_Up));
			}
		}
		if (function != loseMatch || loseMatch == nullptr)
			return false;
		if (g_steve.dead || g_steveWasDead || w->DED || w->Health <= 0.0)
			return false; // Steve died: Half Sword's loss, as it should be
		hslog::info("Half Sword ended the fight as lost while Steve is alive: blocked");
		return true;
	}

	void enter_script(const void *context)
	{
		g_scriptStack.push_back(context);
	}

	void leave_script()
	{
		if (!g_scriptStack.empty())
			g_scriptStack.pop_back();
	}

	bool before_script(void *, const void *, void *)
	{
		return false; // Half Sword's own hits run Blueprint to Blueprint and never pass here; the stand-in is measured per frame
	}

	void after_script(void *, const void *, void *)
	{
	}
}
