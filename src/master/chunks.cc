/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA, 2013-2014 EditShare, 2013-2015 Skytechnology sp. z o.o..

   This file was part of MooseFS and is part of LizardFS.

   LizardFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LizardFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LizardFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "master/chunks.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <unordered_map>
#include <algorithm>
#include <deque>

#include "common/chunks_availability_state.h"
#include "common/datapack.h"
#include "common/exceptions.h"
#include "common/flat_set.h"
#include "common/goal.h"
#include "common/hashfn.h"
#include "common/lizardfs_version.h"
#include "common/main.h"
#include "common/massert.h"
#include "common/slice_traits.h"
#include "common/small_vector.h"
#include "protocol/MFSCommunication.h"
#include "master/checksum.h"
#include "master/chunk_copies_calculator.h"
#include "master/chunk_goal_counters.h"
#include "master/filesystem.h"
#include "master/goal_cache.h"

#ifdef METARESTORE
#  include <time.h>
#else
#  include "common/cfg.h"
#  include "common/main.h"
#  include "common/random.h"
#  include "master/matoclserv.h"
#  include "master/matocsserv.h"
#  include "master/topology.h"
#endif

#define MINLOOPTIME 1
#define MAXLOOPTIME 7200
#define MAXCPS 10000000
#define MINCPS 500
#define MINCHUNKSLOOPPERIOD 40
#define MAXCHUNKSLOOPPERIOD 10000
#define MINCHUNKSLOOPCPU    10
#define MAXCHUNKSLOOPCPU    90

#define HASHSIZE 0x100000
#define HASHPOS(chunkid) (((uint32_t)chunkid)&0xFFFFF)

#define CHECKSUMSEED 78765491511151883ULL

#ifndef METARESTORE

static uint64_t gEndangeredChunksServingLimit;
static uint64_t gEndangeredChunksMaxCapacity;

/* chunk.operation */
enum {NONE,CREATE,SET_VERSION,DUPLICATE,TRUNCATE,DUPTRUNC};
/* slist.valid */
/* INVALID - wrong version / or got info from chunkserver (IO error etc.)  ->  to delete */
/* DEL - deletion in progress */
/* BUSY - operation in progress */
/* VALID - ok */
/* TDBUSY - to delete + BUSY */
/* TDVALID - want to be deleted */
enum {INVALID,DEL,BUSY,VALID,TDBUSY,TDVALID};

/* List of servers containing the chunk */
struct slist {
	matocsserventry *ptr;
	uint32_t version;
	ChunkPartType chunkType;
	uint8_t valid;
//      uint8_t sectionid; - idea - Split machines into sctions. Try to place each copy of particular chunk in different section.
//      uint16_t machineid; - idea - If there are many different processes on the same physical computer then place there only one copy of chunk.
	slist *next;
	slist()
			: ptr(nullptr),
			  version(0),
			  chunkType(),
			  valid(INVALID),
			  next(nullptr) {
	}

	bool is_busy() const {
		return valid == BUSY || valid == TDBUSY;
	}

	bool is_valid() const {
		return valid != INVALID && valid != DEL;
	}

	bool is_todel() const {
		return valid == TDVALID || valid == TDBUSY;
	}

	void mark_busy() {
		switch (valid) {
		case VALID:
			valid = BUSY;
			break;
		case TDVALID:
			valid = TDBUSY;
			break;
		default:
			sassert(!"slist::mark_busy(): wrong state");
		}
	}
	void unmark_busy() {
		switch (valid) {
		case BUSY:
			valid = VALID;
			break;
		case TDBUSY:
			valid = TDVALID;
			break;
		default:
			sassert(!"slist::unmark_busy(): wrong state");
		}
	}
	void mark_todel() {
		switch (valid) {
		case VALID:
			valid = TDVALID;
			break;
		case BUSY:
			valid = TDBUSY;
			break;
		default:
			sassert(!"slist::mark_todel(): wrong state");
		}
	}
	void unmark_todel() {
		switch (valid) {
		case TDVALID:
			valid = VALID;
			break;
		case TDBUSY:
			valid = BUSY;
			break;
		default:
			sassert(!"slist::unmark_todel(): wrong state");
		}
	}
};

#ifndef METARESTORE
static std::vector<matocsserventry*> zombieServersHandledInThisLoop;
static std::vector<matocsserventry*> zombieServersToBeHandledInNextLoop;
static void*                         gChunkLoopEventHandle = NULL;

static uint32_t ReplicationsDelayDisconnect=3600;
static uint32_t ReplicationsDelayInit=300;

static uint32_t MaxWriteRepl;
static uint32_t MaxReadRepl;
static uint32_t MaxDelSoftLimit;
static uint32_t MaxDelHardLimit;
static double   TmpMaxDelFrac;
static uint32_t TmpMaxDel;
static uint32_t HashSteps;
static uint32_t HashCPS;
static uint32_t ChunksLoopPeriod;
static uint32_t ChunksLoopTimeout;
static double   AcceptableDifference;
static bool     RebalancingBetweenLabels = false;

static uint32_t jobshpos;
static uint32_t jobsnorepbefore;

static uint32_t starttime;
#endif // METARESTORE

#define SLIST_BUCKET_SIZE 5000
struct slist_bucket {
	slist bucket[SLIST_BUCKET_SIZE];
	uint32_t firstfree;
	slist_bucket *next;
};

static inline slist* slist_malloc();
static inline void slist_free(slist *p);
#endif

class chunk {
public:
	uint64_t chunkid;
	uint64_t checksum;
	chunk *next;
#ifndef METARESTORE
	slist *slisthead;
#endif
	uint32_t version;
	uint32_t lockid;
	uint32_t lockedto;
private: // public/private sections are mixed here to make the struct as small as possible
	ChunkGoalCounters goalCounters_;
#ifndef METARESTORE
	uint8_t copiesInStats_;
#endif
#ifndef METARESTORE
public:
	uint8_t inEndangeredQueue:1;
	uint8_t needverincrease:1;
	uint8_t interrupted:1;
	uint8_t operation:4;
#endif
#ifndef METARESTORE
private:
	uint8_t allMissingParts_, regularMissingParts_;
	uint8_t allRedundantParts_, regularRedundantParts_;
	uint8_t allFullCopies_, regularFullCopies_;
	uint8_t allAvailabilityState_, regularAvailabilityState_;
#endif

public:
#ifndef METARESTORE
	static ChunksAvailabilityState allChunksAvailability, regularChunksAvailability;
	static ChunksReplicationState allChunksReplicationState, regularChunksReplicationState;
	static uint64_t count;
	static uint64_t allFullChunkCopies[CHUNK_MATRIX_SIZE][CHUNK_MATRIX_SIZE];
	static uint64_t regularFullChunkCopies[CHUNK_MATRIX_SIZE][CHUNK_MATRIX_SIZE];
	static std::deque<chunk *> endangeredChunks;
	static GoalCache goalCache;
#endif

	// Highest id of the chunk's goal
	// This function is preserved only for backward compatibility of metadata checksums
	// and shouldn't be used anywhere else.
	uint8_t highestIdGoal() const {
		return goalCounters_.highestIdGoal();
	}

	// Number of files this chunk belongs to
	uint32_t fileCount() const {
		return goalCounters_.fileCount();
	}

	// Called when this chunk becomes a part of a file with the given goal
	void addFileWithGoal(uint8_t goal) {
#ifndef METARESTORE
		removeFromStats();
#endif
		goalCounters_.addFile(goal);
#ifndef METARESTORE
		updateStats(false);
#endif
	}

	// Called when a file that this chunk belongs to is removed
	void removeFileWithGoal(uint8_t goal) {
#ifndef METARESTORE
		removeFromStats();
#endif
		goalCounters_.removeFile(goal);
#ifndef METARESTORE
		updateStats(false);
#endif
	}

	// Called when a file that this chunk belongs to changes goal
	void changeFileGoal(uint8_t prevGoal, uint8_t newGoal) {
#ifndef METARESTORE
		removeFromStats();
#endif
		goalCounters_.changeFileGoal(prevGoal, newGoal);
#ifndef METARESTORE
		updateStats(false);
#endif
	}

#ifndef METARESTORE
	Goal getGoal() {
		// Do not search for empty goalCounters in cache
		if (goalCounters_.size() == 0) {
			return Goal();
		}

		auto it = goalCache.find(goalCounters_);
		if (it != goalCache.end()) {
			return it->second;
		}

		Goal result;
		int prev_goal = -1;
		for (auto counter : goalCounters_) {
			const Goal &goal = fs_get_goal_definition(counter.goal);
			if (prev_goal != (int)counter.goal) {
				result.mergeIn(goal);
				prev_goal = counter.goal;
			}
		}

		goalCache.put(goalCounters_, result);
		return result;
	}

	// This method should be called on a new chunk
	void initStats() {
		count++;
		allMissingParts_ = regularMissingParts_ = 0;
		allRedundantParts_ = regularRedundantParts_ = 0;
		allFullCopies_ = regularFullCopies_ = 0;
		allAvailabilityState_ = regularAvailabilityState_ = ChunksAvailabilityState::kSafe;
		copiesInStats_ = 0;
		updateStats(false);
	}

	// This method should be called when a chunk is removed
	void freeStats() {
		count--;
		removeFromStats();
	}

	// Updates statistics of all chunks
	void updateStats(bool remove_from_stats = true) {
		int oldAllMissingParts = allMissingParts_;

		if (remove_from_stats) {
			removeFromStats();
		}

		Goal g = getGoal();

		ChunkCopiesCalculator all(g), regular(g);

		for (slist* s = slisthead; s != nullptr; s = s->next) {
			if (!s->is_valid()) {
				continue;
			}
			all.addPart(s->chunkType, matocsserv_get_label(s->ptr));
			if (!s->is_todel()) {
				regular.addPart(s->chunkType, matocsserv_get_label(s->ptr));
			}
		}

		all.optimize();
		regular.optimize();

		allFullCopies_ = all.getFullCopiesCount();
		allAvailabilityState_ = all.getState();
		allMissingParts_ = std::min(200, all.countPartsToRecover());
		allRedundantParts_ = std::min(200, all.countPartsToRemove());
		regularFullCopies_ = regular.getFullCopiesCount();
		regularAvailabilityState_ = regular.getState();
		regularMissingParts_ = std::min(200, regular.countPartsToRecover());
		regularRedundantParts_ = std::min(200, regular.countPartsToRemove());
		copiesInStats_ = ChunkCopiesCalculator::getFullCopiesCount(g);

		/* Enqueue a chunk as endangered only if:
		 * 1. Endangered chunks prioritization is on (limit > 0)
		 * 2. Limit of endangered chunks in queue is not reached
		 * 3. Chunk has more missing parts than it used to
		 * 4. Chunk is endangered
		 * 5. It is not already in queue
		 * By checking conditions below we assert no repetitions in endangered queue. */
		if (gEndangeredChunksServingLimit > 0
				&& endangeredChunks.size() < gEndangeredChunksMaxCapacity
				&& allMissingParts_ > oldAllMissingParts
				&& allAvailabilityState_ == ChunksAvailabilityState::kEndangered
				&& !inEndangeredQueue) {
			inEndangeredQueue = 1;
			endangeredChunks.push_back(this);
		}

		addToStats();
	}

	bool isSafe() const {
		return allAvailabilityState_ == ChunksAvailabilityState::kSafe;
	}

	bool isEndangered() const {
		return allAvailabilityState_ == ChunksAvailabilityState::kEndangered;
	}

	bool isLost() const {
		return allAvailabilityState_ == ChunksAvailabilityState::kLost;
	}

	int countMissingParts() const {
		return allMissingParts_;
	}

	bool countRedundantParts() const {
		return allRedundantParts_;
	}

	uint8_t getFullCopiesCount() const {
		return allFullCopies_;
	}

	bool isLocked() const {
		return lockedto >= main_time();
	}

	void markCopyAsHavingWrongVersion(slist *s) {
		s->valid = INVALID;
		updateStats();
	}

	void invalidateCopy(slist *s) {
		s->valid = INVALID;
		s->version = 0;
		updateStats();
	}

	void deleteCopy(slist *s) {
		s->valid = DEL;
		updateStats();
	}

	void unlinkCopy(slist *s, slist **prev_next) {
		*prev_next = s->next;
		slist_free(s);
		updateStats();
	}

	slist* addCopyNoStatsUpdate(matocsserventry *ptr, uint8_t valid, uint32_t version, ChunkPartType type) {
		slist *s = slist_malloc();
		s->ptr = ptr;
		s->valid = valid;
		s->version = version;
		s->chunkType = type;
		s->next = slisthead;
		slisthead = s;
		return s;
	}

	slist *addCopy(matocsserventry *ptr, uint8_t valid, uint32_t version, ChunkPartType type) {
		slist *s = addCopyNoStatsUpdate(ptr, valid, version, type);
		updateStats();
		return s;
	}

private:
	ChunksAvailabilityState::State allCopiesState() const {
		return static_cast<ChunksAvailabilityState::State>(allAvailabilityState_);
	}

	ChunksAvailabilityState::State regularCopiesState() const {
		return static_cast<ChunksAvailabilityState::State>(regularAvailabilityState_);
	}

	void removeFromStats() {
		int prev_goal = -1;
		for(const auto& counter : goalCounters_) {
			if (prev_goal == (int)counter.goal) {
				continue;
			}
			prev_goal = counter.goal;
			allChunksAvailability.removeChunk(counter.goal, allCopiesState());
			allChunksReplicationState.removeChunk(counter.goal, allMissingParts_, allRedundantParts_);

			regularChunksAvailability.removeChunk(counter.goal, regularCopiesState());
			regularChunksReplicationState.removeChunk(counter.goal,
				regularMissingParts_, regularRedundantParts_);
		}

		uint8_t limitedGoal = std::min<uint8_t>(CHUNK_MATRIX_SIZE - 1, copiesInStats_);
		uint8_t limitedAll = std::min<uint8_t>(CHUNK_MATRIX_SIZE - 1, allFullCopies_);
		uint8_t limitedRegular = std::min<uint8_t>(CHUNK_MATRIX_SIZE - 1, regularFullCopies_);
		allFullChunkCopies[limitedGoal][limitedAll]--;
		regularFullChunkCopies[limitedGoal][limitedRegular]--;
	}

	void addToStats() {
		int prev_goal = -1;
		for(const auto& counter : goalCounters_) {
			if (prev_goal == (int)counter.goal) {
				continue;
			}
			prev_goal = counter.goal;
			allChunksAvailability.addChunk(counter.goal, allCopiesState());
			allChunksReplicationState.addChunk(counter.goal, allMissingParts_, allRedundantParts_);

			regularChunksAvailability.addChunk(counter.goal, regularCopiesState());
			regularChunksReplicationState.addChunk(counter.goal,
				regularMissingParts_, regularRedundantParts_);
		}

		uint8_t limitedGoal = std::min<uint8_t>(CHUNK_MATRIX_SIZE - 1, copiesInStats_);
		uint8_t limitedAll = std::min<uint8_t>(CHUNK_MATRIX_SIZE - 1, allFullCopies_);
		uint8_t limitedRegular = std::min<uint8_t>(CHUNK_MATRIX_SIZE - 1, regularFullCopies_);
		allFullChunkCopies[limitedGoal][limitedAll]++;
		regularFullChunkCopies[limitedGoal][limitedRegular]++;
	}
#endif
};

#ifndef METARESTORE

std::deque<chunk *> chunk::endangeredChunks;
GoalCache chunk::goalCache(10000);
ChunksAvailabilityState chunk::allChunksAvailability, chunk::regularChunksAvailability;
ChunksReplicationState chunk::allChunksReplicationState, chunk::regularChunksReplicationState;
uint64_t chunk::count;
uint64_t chunk::allFullChunkCopies[CHUNK_MATRIX_SIZE][CHUNK_MATRIX_SIZE];
uint64_t chunk::regularFullChunkCopies[CHUNK_MATRIX_SIZE][CHUNK_MATRIX_SIZE];
#endif

#define CHUNK_BUCKET_SIZE 20000
struct chunk_bucket {
	chunk bucket[CHUNK_BUCKET_SIZE];
	uint32_t firstfree;
	chunk_bucket *next;
};

namespace {
struct ChunksMetadata {
#ifndef METARESTORE
	// server lists
	slist_bucket *sbhead;
	slist *slfreehead;
#endif

	// chunks
	chunk_bucket *cbhead;
	chunk *chfreehead;
	chunk *chunkhash[HASHSIZE];
	uint64_t lastchunkid;
	chunk* lastchunkptr;

	// other chunks metadata information
	uint64_t nextchunkid;
	uint64_t chunksChecksum;
	uint64_t chunksChecksumRecalculated;
	uint32_t checksumRecalculationPosition;

	ChunksMetadata() :
#ifndef METARESTORE
			sbhead{},
			slfreehead{},
#endif
			cbhead{},
			chfreehead{},
			chunkhash{},
			lastchunkid{},
			lastchunkptr{},
			nextchunkid{1},
			chunksChecksum{},
			chunksChecksumRecalculated{},
			checksumRecalculationPosition{0} {
	}

	~ChunksMetadata() {
#ifndef METARESTORE
		slist_bucket *sbn;
		for (slist_bucket *sb = sbhead; sb; sb = sbn) {
			sbn = sb->next;
			delete sb;
		}
#endif

		chunk_bucket *cbn;
		for (chunk_bucket *cb = cbhead; cb; cb = cbn) {
			cbn = cb->next;
			delete cb;
		}
	}
};
} // anonymous namespace

static ChunksMetadata* gChunksMetadata;

#define LOCKTIMEOUT 120
#define UNUSED_DELETE_TIMEOUT (86400*7)

#ifndef METARESTORE
class ReplicationDelayInfo {
public:
	ReplicationDelayInfo()
		: disconnectedServers_(0),
		  timestamp_() {}

	void serverDisconnected() {
		refresh();
		++disconnectedServers_;
		timestamp_ = main_time() + ReplicationsDelayDisconnect;
	}

	void serverConnected() {
		refresh();
		if (disconnectedServers_ > 0) {
			--disconnectedServers_;
		}
	}

	bool replicationAllowed(int missingCopies) {
		refresh();
		return missingCopies > disconnectedServers_;
	}

private:
	uint16_t disconnectedServers_;
	uint32_t timestamp_;

	void refresh() {
		if (main_time() > timestamp_) {
			disconnectedServers_ = 0;
		}
	}

};

/*
 * Information about recently disconnected and connected servers
 * necessary for replication to unlabeled servers.
 */
static ReplicationDelayInfo replicationDelayInfoForAll;

/*
 * Information about recently disconnected and connected servers
 * necessary for replication to servers with specified label.
 */
static std::unordered_map<MediaLabel, ReplicationDelayInfo, MediaLabel::hash> replicationDelayInfoForLabel;

struct job_info {
	uint32_t del_invalid;
	uint32_t del_unused;
	uint32_t del_diskclean;
	uint32_t del_overgoal;
	uint32_t copy_undergoal;
};

struct loop_info {
	job_info done,notdone;
	uint32_t copy_rebalance;
};

static loop_info chunksinfo = {{0,0,0,0,0},{0,0,0,0,0},0};
static uint32_t chunksinfo_loopstart=0,chunksinfo_loopend=0;

static uint32_t stats_deletions=0;
static uint32_t stats_replications=0;

void chunk_stats(uint32_t *del,uint32_t *repl) {
	*del = stats_deletions;
	*repl = stats_replications;
	stats_deletions = 0;
	stats_replications = 0;
}

#endif // ! METARESTORE

static uint64_t chunk_checksum(const chunk* c) {
	if (c == nullptr || c->fileCount() == 0) {
		// We treat chunks with fileCount=0 as non-existent, so that we don't have to notify shadow
		// masters when we remove them from our structures.
		return 0;
	}
	uint64_t checksum = 64517419147637ULL;
	// Only highest id goal is taken into checksum for compatibility reasons
	hashCombine(checksum, c->chunkid, c->version, c->lockedto, c->highestIdGoal(), c->fileCount());

	return checksum;
}

static void chunk_checksum_add_to_background(chunk* ch) {
	if (!ch) {
		return;
	}
	removeFromChecksum(gChunksMetadata->chunksChecksum, ch->checksum);
	ch->checksum = chunk_checksum(ch);
	addToChecksum(gChunksMetadata->chunksChecksumRecalculated, ch->checksum);
	addToChecksum(gChunksMetadata->chunksChecksum, ch->checksum);
}

static void chunk_update_checksum(chunk* ch) {
	if (!ch) {
		return;
	}
	if (HASHPOS(ch->chunkid) < gChunksMetadata->checksumRecalculationPosition) {
		removeFromChecksum(gChunksMetadata->chunksChecksumRecalculated, ch->checksum);
	}
	removeFromChecksum(gChunksMetadata->chunksChecksum, ch->checksum);
	ch->checksum = chunk_checksum(ch);
	if (HASHPOS(ch->chunkid) < gChunksMetadata->checksumRecalculationPosition) {
		DEBUG_LOG("master.fs.checksum.changing_recalculated_chunk");
		addToChecksum(gChunksMetadata->chunksChecksumRecalculated, ch->checksum);
	} else {
		DEBUG_LOG("master.fs.checksum.changing_not_recalculated_chunk");
	}
	addToChecksum(gChunksMetadata->chunksChecksum, ch->checksum);
}

/*!
 * \brief update chunks checksum in the background
 * \param limit for processed chunks per function call
 * \return info whether all chunks were updated or not.
 */

ChecksumRecalculationStatus chunks_update_checksum_a_bit(uint32_t speedLimit) {
	if (gChunksMetadata->checksumRecalculationPosition == 0) {
		gChunksMetadata->chunksChecksumRecalculated = CHECKSUMSEED;
	}
	uint32_t recalculated = 0;
	while (gChunksMetadata->checksumRecalculationPosition < HASHSIZE) {
		chunk* c;
		for (c = gChunksMetadata->chunkhash[gChunksMetadata->checksumRecalculationPosition]; c; c=c->next) {
			chunk_checksum_add_to_background(c);
			++recalculated;
		}
		++gChunksMetadata->checksumRecalculationPosition;
		if (recalculated >= speedLimit) {
			return ChecksumRecalculationStatus::kInProgress;
		}
	}
	// Recalculating chunks checksum finished
	gChunksMetadata->checksumRecalculationPosition = 0;
	if (gChunksMetadata->chunksChecksum != gChunksMetadata->chunksChecksumRecalculated) {
		syslog(LOG_WARNING,"Chunks metadata checksum mismatch found, replacing with a new value.");
		DEBUG_LOG("master.fs.checksum.mismatch");
		gChunksMetadata->chunksChecksum = gChunksMetadata->chunksChecksumRecalculated;
	}
	return ChecksumRecalculationStatus::kDone;
}

static void chunk_recalculate_checksum() {
	gChunksMetadata->chunksChecksum = CHECKSUMSEED;
	for (int i = 0; i < HASHSIZE; ++i) {
		for (chunk* ch = gChunksMetadata->chunkhash[i]; ch; ch = ch->next) {
			ch->checksum = chunk_checksum(ch);
			addToChecksum(gChunksMetadata->chunksChecksum, ch->checksum);
		}
	}
}

uint64_t chunk_checksum(ChecksumMode mode) {
	uint64_t checksum = 46586918175221;
	addToChecksum(checksum, gChunksMetadata->nextchunkid);
	if (mode == ChecksumMode::kForceRecalculate) {
		chunk_recalculate_checksum();
	}
	addToChecksum(checksum, gChunksMetadata->chunksChecksum);
	return checksum;
}

#ifndef METARESTORE
static inline slist* slist_malloc() {
	slist_bucket *sb;
	slist *ret;
	if (gChunksMetadata->slfreehead) {
		ret = gChunksMetadata->slfreehead;
		gChunksMetadata->slfreehead = ret->next;
		return ret;
	}
	if (gChunksMetadata->sbhead==NULL || gChunksMetadata->sbhead->firstfree==SLIST_BUCKET_SIZE) {
		sb = new slist_bucket;
		passert(sb);
		sb->next = gChunksMetadata->sbhead;
		sb->firstfree = 0;
		gChunksMetadata->sbhead = sb;
	}
	ret = (gChunksMetadata->sbhead->bucket)+(gChunksMetadata->sbhead->firstfree);
	gChunksMetadata->sbhead->firstfree++;
	return ret;
}

static inline void slist_free(slist *p) {
	p->next = gChunksMetadata->slfreehead;
	gChunksMetadata->slfreehead = p;
}
#endif /* !METARESTORE */

static inline chunk* chunk_malloc() {
	chunk_bucket *cb;
	chunk *ret;
	if (gChunksMetadata->chfreehead) {
		ret = gChunksMetadata->chfreehead;
		gChunksMetadata->chfreehead = ret->next;
		return ret;
	}
	if (gChunksMetadata->cbhead==NULL || gChunksMetadata->cbhead->firstfree==CHUNK_BUCKET_SIZE) {
		cb = new chunk_bucket;
		cb->next = gChunksMetadata->cbhead;
		cb->firstfree = 0;
		gChunksMetadata->cbhead = cb;
	}
	ret = (gChunksMetadata->cbhead->bucket)+(gChunksMetadata->cbhead->firstfree);
	gChunksMetadata->cbhead->firstfree++;
	return ret;
}

#ifndef METARESTORE
static inline void chunk_free(chunk *p) {
	p->next = gChunksMetadata->chfreehead;
	gChunksMetadata->chfreehead = p;
}
#endif /* METARESTORE */

chunk* chunk_new(uint64_t chunkid, uint32_t chunkversion) {
	uint32_t chunkpos = HASHPOS(chunkid);
	chunk *newchunk;
	newchunk = chunk_malloc();
	newchunk->next = gChunksMetadata->chunkhash[chunkpos];
	gChunksMetadata->chunkhash[chunkpos] = newchunk;
	newchunk->chunkid = chunkid;
	newchunk->version = chunkversion;
	newchunk->lockid = 0;
	newchunk->lockedto = 0;
#ifndef METARESTORE
	newchunk->inEndangeredQueue = 0;
	newchunk->needverincrease = 1;
	newchunk->interrupted = 0;
	newchunk->operation = NONE;
	newchunk->slisthead = NULL;
	newchunk->initStats();
#endif
	gChunksMetadata->lastchunkid = chunkid;
	gChunksMetadata->lastchunkptr = newchunk;
	newchunk->checksum = 0;
	chunk_update_checksum(newchunk);
	return newchunk;
}

#ifndef METARESTORE
void chunk_emergency_increase_version(chunk *c) {
	if (c->isLost()) { // should always be false !!!
		syslog(LOG_ERR, "chunk_emergency_increase_version called on a lost chunk");
		matoclserv_chunk_status(c->chunkid, LIZARDFS_ERROR_CHUNKLOST);
		c->operation = NONE;
		return;
	}
	uint32_t i = 0;
	for (slist *s=c->slisthead ;s ; s=s->next) {
		if (s->is_valid()) {
			if (!s->is_busy()) {
				s->mark_busy();
			}
			s->version = c->version+1;
			matocsserv_send_setchunkversion(s->ptr,c->chunkid,c->version+1,c->version,
					s->chunkType);
			i++;
		}
	}
	if (i>0) { // should always be true !!!
		c->interrupted = 0;
		c->operation = SET_VERSION;
		c->version++;
		chunk_update_checksum(c);
		fs_incversion(c->chunkid);
	} else {
		syslog(LOG_ERR, "chunk %016" PRIX64 " lost in emergency increase version", c->chunkid);
		matoclserv_chunk_status(c->chunkid, LIZARDFS_ERROR_CHUNKLOST);
		c->operation = NONE;
	}
}

bool chunk_server_is_disconnected(matocsserventry* ptr) {
	for (auto zombies : {&zombieServersHandledInThisLoop, &zombieServersToBeHandledInNextLoop}) {
		if (std::find(zombies->begin(), zombies->end(), ptr) != zombies->end()) {
			return true;
		}
	}
	return false;
}

void chunk_handle_disconnected_copies(chunk* c) {
	slist *s, **st;
	st = &(c->slisthead);
	bool lostCopyFound = false;
	while (*st) {
		s = *st;
		if (chunk_server_is_disconnected(s->ptr)) {
			c->unlinkCopy(s, st);
			c->needverincrease=1;
			lostCopyFound = true;
		} else {
			st = &(s->next);
		}
	}
	if (lostCopyFound && c->operation!=NONE) {
		bool any_copy_busy = false;
		for (s=c->slisthead ; s ; s=s->next) {
			any_copy_busy |= s->is_busy();
		}
		if (any_copy_busy) {
			c->interrupted = 1;
		} else {
			if (!c->isLost()) {
				chunk_emergency_increase_version(c);
			} else {
				matoclserv_chunk_status(c->chunkid,LIZARDFS_ERROR_NOTDONE);
				c->operation=NONE;
			}
		}
	}
}
#endif

chunk* chunk_find(uint64_t chunkid) {
	uint32_t chunkpos = HASHPOS(chunkid);
	chunk *chunkit;
	if (gChunksMetadata->lastchunkid==chunkid) {
		return gChunksMetadata->lastchunkptr;
	}
	for (chunkit = gChunksMetadata->chunkhash[chunkpos] ; chunkit ; chunkit = chunkit->next) {
		if (chunkit->chunkid == chunkid) {
			gChunksMetadata->lastchunkid = chunkid;
			gChunksMetadata->lastchunkptr = chunkit;
#ifndef METARESTORE
			chunk_handle_disconnected_copies(chunkit);
#endif // METARESTORE
			return chunkit;
		}
	}
	return NULL;
}

#ifndef METARESTORE
void chunk_delete(chunk* c) {
	if (gChunksMetadata->lastchunkptr==c) {
		gChunksMetadata->lastchunkid=0;
		gChunksMetadata->lastchunkptr=NULL;
	}
	c->freeStats();
	chunk_free(c);
}

uint32_t chunk_count(void) {
	return chunk::count;
}

void chunk_info(uint32_t *allchunks,uint32_t *allcopies,uint32_t *regularvalidcopies) {
	*allchunks = chunk::count;
	*allcopies = 0;
	*regularvalidcopies = 0;
	for (int actualCopies = 1; actualCopies < CHUNK_MATRIX_SIZE; actualCopies++) {
		uint32_t ag = 0;
		uint32_t rg = 0;
		for (int expectedCopies = 0; expectedCopies < CHUNK_MATRIX_SIZE; expectedCopies++) {
			ag += chunk::allFullChunkCopies[expectedCopies][actualCopies];
			rg += chunk::regularFullChunkCopies[expectedCopies][actualCopies];
		}
		*allcopies += ag * actualCopies;
		*regularvalidcopies += rg * actualCopies;
	}
}

uint32_t chunk_get_missing_count(void) {
	uint32_t res = 0;
	for (uint8_t goal = GoalId::kMin; goal <= GoalId::kMax; ++goal) {
		res += chunk::allChunksAvailability.lostChunks(goal);
	}
	return res;
}

void chunk_store_chunkcounters(uint8_t *buff,uint8_t matrixid) {
	if (matrixid == MATRIX_ALL_COPIES) {
		for (int i = 0; i < CHUNK_MATRIX_SIZE; i++) {
			for (int j = 0; j < CHUNK_MATRIX_SIZE; j++) {
				put32bit(&buff, chunk::allFullChunkCopies[i][j]);
			}
		}
	} else if (matrixid == MATRIX_REGULAR_COPIES) {
		for (int i = 0; i < CHUNK_MATRIX_SIZE; i++) {
			for (int j = 0; j < CHUNK_MATRIX_SIZE; j++) {
				put32bit(&buff, chunk::regularFullChunkCopies[i][j]);
			}
		}
	} else {
		memset(buff, 0, CHUNK_MATRIX_SIZE * CHUNK_MATRIX_SIZE * sizeof(uint32_t));
	}
}
#endif

/// updates chunk's goal after a file goal has been changed
int chunk_change_file(uint64_t chunkid,uint8_t prevgoal,uint8_t newgoal) {
	chunk *c;
	if (prevgoal==newgoal) {
		return LIZARDFS_STATUS_OK;
	}
	c = chunk_find(chunkid);
	if (c==NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	try {
		c->changeFileGoal(prevgoal, newgoal);
	} catch (Exception& ex) {
		syslog(LOG_WARNING, "chunk_change_file: %s", ex.what());
		return LIZARDFS_ERROR_CHUNKLOST;
	}
	chunk_update_checksum(c);
	return LIZARDFS_STATUS_OK;
}

/// updates chunk's goal after a file with goal `goal' has been removed
static inline int chunk_delete_file_int(chunk *c,uint8_t goal) {
	try {
		c->removeFileWithGoal(goal);
	} catch (Exception& ex) {
		syslog(LOG_WARNING, "chunk_delete_file_int: %s", ex.what());
		return LIZARDFS_ERROR_CHUNKLOST;
	}
	chunk_update_checksum(c);
	return LIZARDFS_STATUS_OK;
}

/// updates chunk's goal after a file with goal `goal' has been added
static inline int chunk_add_file_int(chunk *c,uint8_t goal) {
	try {
		c->addFileWithGoal(goal);
	} catch (Exception& ex) {
		syslog(LOG_WARNING, "chunk_add_file_int: %s", ex.what());
		return LIZARDFS_ERROR_CHUNKLOST;
	}
	chunk_update_checksum(c);
	return LIZARDFS_STATUS_OK;
}

int chunk_delete_file(uint64_t chunkid,uint8_t goal) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	return chunk_delete_file_int(c,goal);
}

int chunk_add_file(uint64_t chunkid,uint8_t goal) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	return chunk_add_file_int(c,goal);
}

int chunk_can_unlock(uint64_t chunkid, uint32_t lockid) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	if (lockid == 0) {
		// lockid == 0 -> force unlock
		return LIZARDFS_STATUS_OK;
	}
	// We will let client unlock the chunk even if c->lockedto < main_time()
	// if he provides lockId that was used to lock the chunk -- this means that nobody
	// else used this chunk since it was locked (operations like truncate or replicate
	// would remove such a stale lock before modifying the chunk)
	if (c->lockid == lockid) {
		return LIZARDFS_STATUS_OK;
	} else if (c->lockedto == 0) {
		return LIZARDFS_ERROR_NOTLOCKED;
	} else {
		return LIZARDFS_ERROR_WRONGLOCKID;
	}
}

int chunk_unlock(uint64_t chunkid) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	// Don't remove lockid to safely accept retransmission of FUSE_CHUNK_UNLOCK message
	c->lockedto = 0;
	chunk_update_checksum(c);
	return LIZARDFS_STATUS_OK;
}

#ifndef METARESTORE

int chunk_invalidate_goal_cache(){
	chunk::goalCache.invalidate();
	return LIZARDFS_STATUS_OK;
}

bool chunk_has_only_invalid_copies(uint64_t chunkid) {
	if (chunkid == 0) {
		return false;
	}
	chunk *c = chunk_find(chunkid);
	if (c == NULL || !c->isLost()) {
		return false;
	}
	// Chunk is lost, so it can only have INVALID or DEL copies.
	// Return true it there is at least one INVALID.
	for (slist *s = c->slisthead; s != nullptr; s = s->next) {
		if (s->valid == INVALID) {
			return true;
		}
	}
	return false;
}

int chunk_get_fullcopies(uint64_t chunkid,uint8_t *vcopies) {
	chunk *c;
	*vcopies = 0;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}

	*vcopies = c->getFullCopiesCount();

	return LIZARDFS_STATUS_OK;
}

int chunk_get_partstomodify(uint64_t chunkid, int &recover, int &remove) {
	chunk *c;
	recover = 0;
	remove = 0;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	recover = c->countMissingParts();
	remove = c->countRedundantParts();
	return LIZARDFS_STATUS_OK;
}

uint8_t chunk_multi_modify(uint64_t ochunkid, uint32_t *lockid, uint8_t goal,
		bool usedummylockid, bool quota_exceeded, uint8_t *opflag, uint64_t *nchunkid,
		uint32_t min_server_version = 0) {
	chunk *c = NULL;
	if (ochunkid == 0) { // new chunk
		if (quota_exceeded) {
			return LIZARDFS_ERROR_QUOTA;
		}
		auto serversWithChunkTypes = matocsserv_getservers_for_new_chunk(goal, min_server_version);
		if (serversWithChunkTypes.empty()) {
			uint16_t uscount,tscount;
			double minusage,maxusage;
			matocsserv_usagedifference(&minusage,&maxusage,&uscount,&tscount);
			if ((uscount > 0) && (main_time() > (starttime+600))) { // if there are chunkservers and it's at least one minute after start then it means that there is no space left
				return LIZARDFS_ERROR_NOSPACE;
			} else {
				return LIZARDFS_ERROR_NOCHUNKSERVERS;
			}
		}
		c = chunk_new(gChunksMetadata->nextchunkid++, 1);
		c->interrupted = 0;
		c->operation = CREATE;
		chunk_add_file_int(c,goal);
		for (uint32_t i = 0; i < serversWithChunkTypes.size(); ++i) {
			slist *s = c->addCopyNoStatsUpdate(serversWithChunkTypes[i].first, BUSY,
					c->version, serversWithChunkTypes[i].second);
			matocsserv_send_createchunk(s->ptr, c->chunkid, s->chunkType, c->version);
		}
		c->updateStats();
		*opflag=1;
		*nchunkid = c->chunkid;
	} else {
		chunk *oc = chunk_find(ochunkid);
		if (oc==NULL) {
			return LIZARDFS_ERROR_NOCHUNK;
		}
		if (*lockid != 0 && *lockid != oc->lockid) {
			if (oc->lockid == 0 || oc->lockedto == 0) {
				// Lock was removed by some chunk operation or by a different client
				return LIZARDFS_ERROR_NOTLOCKED;
			} else {
				return LIZARDFS_ERROR_WRONGLOCKID;
			}
		}
		if (*lockid == 0 && oc->isLocked()) {
			return LIZARDFS_ERROR_LOCKED;
		}
		if (oc->isLost()) {
			return LIZARDFS_ERROR_CHUNKLOST;
		}

		if (oc->fileCount() == 1) { // refcount==1
			*nchunkid = ochunkid;
			c = oc;
			if (c->operation!=NONE) {
				return LIZARDFS_ERROR_CHUNKBUSY;
			}
			if (c->needverincrease) {
				uint32_t i = 0;
				for (slist *s=c->slisthead ;s ; s=s->next) {
					if (s->is_valid()) {
						if (!s->is_busy()) {
							s->mark_busy();
						}
						s->version = c->version+1;
						matocsserv_send_setchunkversion(s->ptr, ochunkid, c->version+1, c->version,
								s->chunkType);
						i++;
					}
				}
				if (i>0) {
					c->interrupted = 0;
					c->operation = SET_VERSION;
					c->version++;
					*opflag=1;
				} else {
					// This should never happen - we verified this using ChunkCopiesCalculator
					return LIZARDFS_ERROR_CHUNKLOST;
				}
			} else {
				*opflag=0;
			}
		} else {
			if (oc->fileCount() == 0) { // it's serious structure error
				syslog(LOG_WARNING,"serious structure inconsistency: (chunkid:%016" PRIX64 ")",ochunkid);
				return LIZARDFS_ERROR_CHUNKLOST; // ERROR_STRUCTURE
			}
			if (quota_exceeded) {
				return LIZARDFS_ERROR_QUOTA;
			}
			uint32_t i = 0;
			for (slist *os=oc->slisthead ;os ; os=os->next) {
				if (os->is_valid()) {
					if (c==NULL) {
						c = chunk_new(gChunksMetadata->nextchunkid++, 1);
						c->interrupted = 0;
						c->operation = DUPLICATE;
						chunk_delete_file_int(oc,goal);
						chunk_add_file_int(c,goal);
					}
					slist *s = c->addCopyNoStatsUpdate(os->ptr, BUSY, c->version, os->chunkType);
					matocsserv_send_duplicatechunk(s->ptr, c->chunkid, c->version, os->chunkType,
							oc->chunkid, oc->version);
					i++;
				}
			}
			if (c!=NULL) {
				c->updateStats();
			}
			if (i>0) {
				*nchunkid = c->chunkid;
				*opflag=1;
			} else {
				return LIZARDFS_ERROR_CHUNKLOST;
			}
		}
	}

	c->lockedto = main_time() + LOCKTIMEOUT;
	if (*lockid == 0) {
		if (usedummylockid) {
			*lockid = 1;
		} else {
			*lockid = 2 + rnd_ranged<uint32_t>(0xFFFFFFF0); // some random number greater than 1
		}
	}
	c->lockid = *lockid;
	chunk_update_checksum(c);
	return LIZARDFS_STATUS_OK;
}

uint8_t chunk_multi_truncate(uint64_t ochunkid, uint32_t lockid, uint32_t length,
		uint8_t goal, bool denyTruncatingParityParts, bool quota_exceeded, uint64_t *nchunkid) {
	uint32_t i;
	chunk *oc,*c;

	c=NULL;
	oc = chunk_find(ochunkid);
	if (oc==NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	if (oc->isLocked() && (lockid == 0 || lockid != oc->lockid)) {
		return LIZARDFS_ERROR_LOCKED;
	}
	if (denyTruncatingParityParts) {
		for (slist *s = oc->slisthead; s; s = s->next) {
			if (slice_traits::xors::isXorParity(s->chunkType)) {
				return LIZARDFS_ERROR_NOTPOSSIBLE;
			}
		}
	}
	if (oc->fileCount() == 1) { // refcount==1
		*nchunkid = ochunkid;
		c = oc;
		if (c->operation!=NONE) {
			return LIZARDFS_ERROR_CHUNKBUSY;
		}
		i=0;
		for (slist *s=c->slisthead ; s ; s=s->next) {
			if (s->is_valid()) {
				if (!s->is_busy()) {
					s->mark_busy();
				}
				s->version = c->version+1;
				uint32_t chunkTypeLength =
						slice_traits::chunkLengthToChunkPartLength(s->chunkType, length);
				matocsserv_send_truncatechunk(s->ptr, ochunkid, s->chunkType, chunkTypeLength,
						c->version + 1, c->version);
				i++;
			}
		}
		if (i>0) {
			c->interrupted = 0;
			c->operation = TRUNCATE;
			c->version++;
		} else {
			return LIZARDFS_ERROR_CHUNKLOST;
		}
	} else {
		if (oc->fileCount() == 0) { // it's serious structure error
			syslog(LOG_WARNING,"serious structure inconsistency: (chunkid:%016" PRIX64 ")",ochunkid);
			return LIZARDFS_ERROR_CHUNKLOST; // ERROR_STRUCTURE
		}
		if (quota_exceeded) {
			return LIZARDFS_ERROR_QUOTA;
		}
		i=0;
		for (slist *os=oc->slisthead ; os ; os=os->next) {
			if (os->is_valid()) {
				if (c==NULL) {
					c = chunk_new(gChunksMetadata->nextchunkid++, 1);
					c->interrupted = 0;
					c->operation = DUPTRUNC;
					chunk_delete_file_int(oc,goal);
					chunk_add_file_int(c,goal);
				}
				slist *s = c->addCopyNoStatsUpdate(os->ptr, BUSY, c->version, os->chunkType);
				matocsserv_send_duptruncchunk(s->ptr, c->chunkid, c->version,
						s->chunkType, oc->chunkid, oc->version,
						slice_traits::chunkLengthToChunkPartLength(s->chunkType, length));
				i++;
			}
		}
		if (c!=NULL) {
			c->updateStats();
		}
		if (i>0) {
			*nchunkid = c->chunkid;
		} else {
			return LIZARDFS_ERROR_CHUNKLOST;
		}
	}

	c->lockedto=(uint32_t)main_time()+LOCKTIMEOUT;
	c->lockid = lockid;
	chunk_update_checksum(c);
	return LIZARDFS_STATUS_OK;
}
#endif // ! METARESTORE

uint8_t chunk_apply_modification(uint32_t ts, uint64_t oldChunkId, uint32_t lockid, uint8_t goal,
		bool doIncreaseVersion, uint64_t *newChunkId) {
	chunk *c;
	if (oldChunkId == 0) { // new chunk
		c = chunk_new(gChunksMetadata->nextchunkid++, 1);
		chunk_add_file_int(c, goal);
	} else {
		chunk *oc = chunk_find(oldChunkId);
		if (oc == NULL) {
			return LIZARDFS_ERROR_NOCHUNK;
		}
		if (oc->fileCount() == 0) { // refcount == 0
			syslog(LOG_WARNING,
					"serious structure inconsistency: (chunkid:%016" PRIX64 ")", oldChunkId);
			return LIZARDFS_ERROR_CHUNKLOST; // ERROR_STRUCTURE
		} else if (oc->fileCount() == 1) { // refcount == 1
			c = oc;
			if (doIncreaseVersion) {
				c->version++;
			}
		} else {
			c = chunk_new(gChunksMetadata->nextchunkid++, 1);
			chunk_delete_file_int(oc, goal);
			chunk_add_file_int(c, goal);
		}
	}
	c->lockedto = ts + LOCKTIMEOUT;
	c->lockid = lockid;
	chunk_update_checksum(c);
	*newChunkId = c->chunkid;
	return LIZARDFS_STATUS_OK;
}

#ifndef METARESTORE
int chunk_repair(uint8_t goal, uint64_t ochunkid, uint32_t *nversion) {
	uint32_t best_version;
	chunk *c;
	slist *s;

	*nversion=0;
	if (ochunkid==0) {
		return 0; // not changed
	}

	c = chunk_find(ochunkid);
	if (c==NULL) { // no such chunk - erase (nchunkid already is 0 - so just return with "changed" status)
		return 1;
	}
	if (c->isLocked()) { // can't repair locked chunks - but if it's locked, then likely it doesn't need to be repaired
		return 0;
	}

	// calculators will be sorted by decreasing keys, so highest version will be first.
	std::map<uint32_t, ChunkCopiesCalculator, std::greater<uint32_t>> calculators;
	best_version = 0;
	for (s = c->slisthead; s ; s=s->next) {
		// ignore chunks which are being deleted
		if (s->valid != DEL) {
			ChunkCopiesCalculator &calculator = calculators[s->version];
			calculator.addPart(s->chunkType, matocsserv_get_label(s->ptr));
		}
	}
	// find best version which can be recovered
	// calculators are sorted by decreasing keys, so highest version will be first.
	for (auto &version_and_calculator : calculators) {
		uint32_t version = version_and_calculator.first;
		ChunkCopiesCalculator &calculator = version_and_calculator.second;
		calculator.optimize();
		// calculator.isRecoveryPossible() won't work below, because target goal is empty.
		if (calculator.getFullCopiesCount() > 0) {
			best_version = version;
			break;
		}
	}
	// current version is readable
	if (best_version == c->version) {
		return 0;
	}
	// didn't find sensible chunk - so erase it
	if (best_version == 0) {
		chunk_delete_file_int(c, goal);
		return 1;
	}
	// found previous version which is readable
	c->version = best_version;
	for (s=c->slisthead ; s ; s=s->next) {
		if (s->valid == INVALID && s->version==best_version) {
			s->valid = VALID;
		}
	}
	*nversion = best_version;
	c->needverincrease=1;
	c->updateStats();
	chunk_update_checksum(c);
	return 1;
}
#endif

int chunk_set_version(uint64_t chunkid,uint32_t version) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	c->version = version;
	chunk_update_checksum(c);
	return LIZARDFS_STATUS_OK;
}

int chunk_increase_version(uint64_t chunkid) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	c->version++;
	chunk_update_checksum(c);
	return LIZARDFS_STATUS_OK;
}

uint8_t chunk_set_next_chunkid(uint64_t nextChunkIdToBeSet) {
	if (nextChunkIdToBeSet >= gChunksMetadata->nextchunkid) {
		gChunksMetadata->nextchunkid = nextChunkIdToBeSet;
		return LIZARDFS_STATUS_OK;
	} else {
		syslog(LOG_WARNING,"was asked to increase the next chunk id to %" PRIu64 ", but it was"
				"already set to a bigger value %" PRIu64 ". Ignoring.",
				nextChunkIdToBeSet, gChunksMetadata->nextchunkid);
		return LIZARDFS_ERROR_MISMATCH;
	}
}

#ifndef METARESTORE

const ChunksReplicationState& chunk_get_replication_state(bool regularChunksOnly) {
	return regularChunksOnly ?
			chunk::regularChunksReplicationState :
			chunk::allChunksReplicationState;
}

const ChunksAvailabilityState& chunk_get_availability_state(bool regularChunksOnly) {
	return regularChunksOnly ?
			chunk::regularChunksAvailability :
			chunk::allChunksAvailability;
}

struct ChunkLocation {
	ChunkLocation() : chunkType(slice_traits::standard::ChunkPartType()),
			distance(0), random(0) {
	}
	NetworkAddress address;
	ChunkPartType chunkType;
	uint32_t distance;
	uint32_t random;
	MediaLabel label;
	bool operator<(const ChunkLocation& other) const {
		if (distance < other.distance) {
			return true;
		} else if (distance > other.distance) {
			return false;
		} else {
			return random < other.random;
		}
	}
};

// TODO deduplicate
int chunk_getversionandlocations(uint64_t chunkid, uint32_t currentIp, uint32_t& version,
		uint32_t maxNumberOfChunkCopies, std::vector<ChunkTypeWithAddress>& serversList) {
	chunk *c;
	slist *s;
	uint8_t cnt;

	sassert(serversList.empty());
	c = chunk_find(chunkid);

	if (c == NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	version = c->version;
	cnt = 0;
	std::vector<ChunkLocation> chunkLocation;
	ChunkLocation chunkserverLocation;
	for (s = c->slisthead; s; s = s->next) {
		if (s->is_valid()) {
			if (cnt < maxNumberOfChunkCopies && matocsserv_getlocation(s->ptr,
					&(chunkserverLocation.address.ip),
					&(chunkserverLocation.address.port),
					&(chunkserverLocation.label)) == 0) {
				chunkserverLocation.chunkType = s->chunkType;
				chunkserverLocation.distance =
						topology_distance(chunkserverLocation.address.ip, currentIp);
						// in the future prepare more sophisticated distance function
				chunkserverLocation.random = rnd<uint32_t>();
				chunkLocation.push_back(chunkserverLocation);
				cnt++;
			}
		}
	}
	std::sort(chunkLocation.begin(), chunkLocation.end());
	for (uint32_t i = 0; i < chunkLocation.size(); ++i) {
		const ChunkLocation& loc = chunkLocation[i];
		serversList.emplace_back(loc.address, loc.chunkType);
	}
	return LIZARDFS_STATUS_OK;
}

int chunk_getversionandlocations(uint64_t chunkid, uint32_t currentIp, uint32_t& version,
		uint32_t maxNumberOfChunkCopies, std::vector<ChunkWithAddressAndLabel>& serversList) {
	chunk *c;
	slist *s;
	uint8_t cnt;

	sassert(serversList.empty());
	c = chunk_find(chunkid);

	if (c == NULL) {
		return LIZARDFS_ERROR_NOCHUNK;
	}
	version = c->version;
	cnt = 0;
	std::vector<ChunkLocation> chunkLocation;
	ChunkLocation chunkserverLocation;
	for (s = c->slisthead; s; s = s->next) {
		if (s->is_valid()) {
			if (cnt < maxNumberOfChunkCopies && matocsserv_getlocation(s->ptr,
					&(chunkserverLocation.address.ip),
					&(chunkserverLocation.address.port),
					&(chunkserverLocation.label)) == 0) {
				chunkserverLocation.chunkType = s->chunkType;
				chunkserverLocation.distance =
						topology_distance(chunkserverLocation.address.ip, currentIp);
						// in the future prepare more sophisticated distance function
				chunkserverLocation.random = rnd<uint32_t>();
				chunkLocation.push_back(chunkserverLocation);
				cnt++;
			}
		}
	}
	std::sort(chunkLocation.begin(), chunkLocation.end());
	for (uint32_t i = 0; i < chunkLocation.size(); ++i) {
		const ChunkLocation& loc = chunkLocation[i];
		serversList.emplace_back(loc.address, static_cast<std::string>(loc.label), loc.chunkType);
	}
	return LIZARDFS_STATUS_OK;
}

void chunk_server_has_chunk(matocsserventry *ptr, uint64_t chunkid, uint32_t version, ChunkPartType chunkType) {
	chunk *c;
	const uint32_t new_version = version & 0x7FFFFFFF;
	const bool todel = version & 0x80000000;
	c = chunk_find(chunkid);
	if (c==NULL) {
		// chunkserver has nonexistent chunk, so create it for future deletion
		if (chunkid>=gChunksMetadata->nextchunkid) {
			fs_set_nextchunkid(FsContext::getForMaster(main_time()), chunkid + 1);
		}
		c = chunk_new(chunkid, new_version);
		c->lockedto = (uint32_t)main_time()+UNUSED_DELETE_TIMEOUT;
		c->lockid = 0;
		chunk_update_checksum(c);
	}
	for (slist *s=c->slisthead ; s ; s=s->next) {
		if (s->ptr == ptr && s->chunkType == chunkType) {
			// This server already notified us about its copy.
			// We normally don't get repeated notifications about the same copy, but
			// they can arrive after chunkserver configuration reload (particularly,
			// when folders change their 'to delete' status) or due to bugs.
			// Let's try to handle them as well as we can.
			switch (s->valid) {
			case DEL:
				// We requested deletion, but the chunkserver 'has' this copy again.
				// Repeat deletion request.
				c->invalidateCopy(s);
				// fallthrough
			case INVALID:
				// leave this copy alone
				return;
			default:
				break;
			}
			if (s->version != new_version) {
				syslog(LOG_WARNING, "chunk %016" PRIX64 ": master data indicated "
						"version %08" PRIX32 ", chunkserver reports %08"
						PRIX32 "!!! Updating master data.", c->chunkid,
						s->version, new_version);
				s->version = new_version;
			}
			if (s->version != c->version) {
				c->markCopyAsHavingWrongVersion(s);
				return;
			}
			if (!s->is_todel() && todel) {
				s->mark_todel();
				c->updateStats();
			}
			if (s->is_todel() && !todel) {
				s->unmark_todel();
				c->updateStats();
			}
			return;
		}
	}
	const uint8_t state = (new_version == c->version) ? (todel ? TDVALID : VALID) : INVALID;
	c->addCopy(ptr, state, new_version, chunkType);
}

void chunk_damaged(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunk_type) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c == NULL) {
		// syslog(LOG_WARNING,"chunkserver has nonexistent chunk (%016" PRIX64 "), so create it for future deletion",chunkid);
		if (chunkid >= gChunksMetadata->nextchunkid) {
			gChunksMetadata->nextchunkid = chunkid + 1;
		}
		c = chunk_new(chunkid, 0);
	}
	for (slist *s=c->slisthead ; s ; s=s->next) {
		if (s->ptr == ptr && s->chunkType == chunk_type) {
			c->invalidateCopy(s);
			c->needverincrease=1;
			return;
		}
	}
	c->addCopy(ptr, INVALID, 0, slice_traits::standard::ChunkPartType());
	c->needverincrease=1;
}

void chunk_lost(matocsserventry *ptr,uint64_t chunkid, ChunkPartType chunk_type) {
	chunk *c;
	slist **sptr,*s;
	c = chunk_find(chunkid);
	if (c == nullptr) {
		return;
	}
	sptr=&(c->slisthead);
	while ((s = *sptr)) {
		if (s->ptr == ptr && s->chunkType == chunk_type) {
			c->unlinkCopy(s, sptr);
			c->needverincrease = 1;
		} else {
			sptr = &(s->next);
		}
	}
}

void chunk_server_disconnected(matocsserventry *ptr, const MediaLabel &label) {
	zombieServersToBeHandledInNextLoop.push_back(ptr);
	if (zombieServersHandledInThisLoop.empty()) {
		std::swap(zombieServersToBeHandledInNextLoop, zombieServersHandledInThisLoop);
	}
	replicationDelayInfoForAll.serverDisconnected();
	if (label != MediaLabel::kWildcard) {
		replicationDelayInfoForLabel[label].serverDisconnected();
	}
	main_make_next_poll_nonblocking();
	fs_cs_disconnected();
	gChunksMetadata->lastchunkid = 0;
	gChunksMetadata->lastchunkptr = NULL;
}

void chunk_server_unlabelled_connected() {
	replicationDelayInfoForAll.serverConnected();
}

void chunk_server_label_changed(const MediaLabel &previousLabel, const MediaLabel &newLabel) {
	/*
	 * Only server with no label can be considered as newly connected
	 * and it was added to replicationDelayInfoForAll earlier
	 * in chunk_server_unlabelled_connected call.
	 */
	if (previousLabel == MediaLabel::kWildcard) {
		replicationDelayInfoForLabel[newLabel].serverConnected();
	}
}

/*
 * A function that is called in every main loop iteration, that cleans chunk structs
 */
void chunk_clean_zombie_servers_a_bit() {
	static uint32_t currentPosition = 0;
	if (zombieServersHandledInThisLoop.empty()) {
		return;
	}
	for (auto i = 0; i < 100 ; ++i) {
		if (currentPosition < HASHSIZE) {
			chunk* c;
			for (c=gChunksMetadata->chunkhash[currentPosition] ; c ; c=c->next) {
				chunk_handle_disconnected_copies(c);
			}
			++currentPosition;
		} else {
			for (auto& server : zombieServersHandledInThisLoop) {
				matocsserv_remove_server(server);
			}
			zombieServersHandledInThisLoop.clear();
			std::swap(zombieServersHandledInThisLoop, zombieServersToBeHandledInNextLoop);
			currentPosition = 0;
			break;
		}
	}
	main_make_next_poll_nonblocking();
}

void chunk_clean_zombie_servers() {
	for (auto& server : zombieServersHandledInThisLoop) {
		matocsserv_remove_server(server);
	}
	for (auto& server : zombieServersToBeHandledInNextLoop) {
		matocsserv_remove_server(server);
	}
}

int chunk_canexit(void) {
	if (zombieServersHandledInThisLoop.size() + zombieServersToBeHandledInNextLoop.size() > 0) {
		return 0;
	}
	return 1;
}

void chunk_got_delete_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	chunk *c;
	slist *s,**st;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	st = &(c->slisthead);
	while (*st) {
		s = *st;
		if (s->ptr == ptr && s->chunkType == chunkType) {
			if (s->valid!=DEL) {
				syslog(LOG_WARNING,"got unexpected delete status");
			}
			c->unlinkCopy(s, st);
		} else {
			st = &(s->next);
		}
	}
	if (status!=0) {
		return ;
	}
}

void chunk_got_replicate_status(matocsserventry *ptr, uint64_t chunkId, uint32_t chunkVersion,
		ChunkPartType chunkType, uint8_t status) {
	chunk *c = chunk_find(chunkId);
	if (c == NULL || status != 0) {
		return;
	}

	for (slist *s = c->slisthead; s; s = s->next) {
		if (s->chunkType == chunkType && s->ptr == ptr) {
			syslog(LOG_WARNING,
					"got replication status from server which had had that chunk before (chunk:%016"
					PRIX64 "_%08" PRIX32 ")", chunkId, chunkVersion);
			if (s->valid == VALID && chunkVersion != c->version) {
				s->version = chunkVersion;
				c->markCopyAsHavingWrongVersion(s);
			}
			return;
		}
	}
	const uint8_t state = (c->isLocked() || chunkVersion != c->version) ? INVALID : VALID;
	c->addCopy(ptr, state, chunkVersion, chunkType);
}

void chunk_operation_status(chunk *c, ChunkPartType chunkType, uint8_t status,matocsserventry *ptr) {
	slist *s;
	bool any_copy_busy = false;
	for (s=c->slisthead ; s ; s=s->next) {
		if (s->ptr == ptr && s->chunkType == chunkType) {
			if (status!=0) {
				c->interrupted = 1; // increase version after finish, just in case
				c->invalidateCopy(s);
			} else {
				if (s->is_busy()) {
					s->unmark_busy();
				}
			}
		}
		any_copy_busy |= s->is_busy();
	}
	if (!any_copy_busy) {
		if (!c->isLost()) {
			if (c->interrupted) {
				chunk_emergency_increase_version(c);
			} else {
				matoclserv_chunk_status(c->chunkid,LIZARDFS_STATUS_OK);
				c->operation=NONE;
				c->needverincrease = 0;
			}
		} else {
			matoclserv_chunk_status(c->chunkid,LIZARDFS_ERROR_NOTDONE);
			c->operation=NONE;
		}
	}
}

void chunk_got_create_status(matocsserventry *ptr,uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	chunk *c;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

void chunk_got_duplicate_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	chunk *c;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

void chunk_got_setversion_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	chunk *c;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

void chunk_got_truncate_status(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType, uint8_t status) {
	chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

void chunk_got_duptrunc_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	chunk *c;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

/* ----------------------- */
/* JOBS (DELETE/REPLICATE) */
/* ----------------------- */

void chunk_store_info(uint8_t *buff) {
	put32bit(&buff,chunksinfo_loopstart);
	put32bit(&buff,chunksinfo_loopend);
	put32bit(&buff,chunksinfo.done.del_invalid);
	put32bit(&buff,chunksinfo.notdone.del_invalid);
	put32bit(&buff,chunksinfo.done.del_unused);
	put32bit(&buff,chunksinfo.notdone.del_unused);
	put32bit(&buff,chunksinfo.done.del_diskclean);
	put32bit(&buff,chunksinfo.notdone.del_diskclean);
	put32bit(&buff,chunksinfo.done.del_overgoal);
	put32bit(&buff,chunksinfo.notdone.del_overgoal);
	put32bit(&buff,chunksinfo.done.copy_undergoal);
	put32bit(&buff,chunksinfo.notdone.copy_undergoal);
	put32bit(&buff,chunksinfo.copy_rebalance);
}

//jobs state: jobshpos

class ChunkWorker {
public:
	ChunkWorker();
	void doEveryLoopTasks();
	void doEverySecondTasks();
	void doChunkJobs(chunk *c, uint16_t serverCount);

private:
	typedef std::vector<ServerWithUsage> ServersWithUsage;

	bool tryReplication(chunk *c, ChunkPartType type, matocsserventry *destinationServer);

	void deleteInvalidChunkParts(chunk *c);
	void deleteAllChunkParts(chunk *c);
	bool replicateChunkPart(chunk *c, Goal::Slice::Type slice_type, int slice_part, ChunkCopiesCalculator& calc);
	bool removeUnneededChunkPart(chunk *c, Goal::Slice::Type slice_type, int slice_part, ChunkCopiesCalculator& calc);
	bool rebalanceChunkParts(chunk *c, ChunkCopiesCalculator& calc, bool only_todel);

	loop_info inforec_;
	uint32_t deleteNotDone_;
	uint32_t deleteDone_;
	uint32_t prevToDeleteCount_;
	uint32_t deleteLoopCount_;

	/// All chunkservers sorted by disk usage.
	ServersWithUsage sortedServers_;

	/// For each label, all servers with this label sorted by disk usage.
	std::map<MediaLabel, ServersWithUsage> labeledSortedServers_;
};

ChunkWorker::ChunkWorker()
		: deleteNotDone_(0),
		  deleteDone_(0),
		  prevToDeleteCount_(0),
		  deleteLoopCount_(0) {
	memset(&inforec_,0,sizeof(loop_info));
}

void ChunkWorker::doEveryLoopTasks() {
	deleteLoopCount_++;
	if (deleteLoopCount_ >= 16) {
		uint32_t toDeleteCount = deleteDone_ + deleteNotDone_;
		deleteLoopCount_ = 0;
		if ((deleteNotDone_ > deleteDone_) && (toDeleteCount > prevToDeleteCount_)) {
			TmpMaxDelFrac *= 1.5;
			if (TmpMaxDelFrac>MaxDelHardLimit) {
				syslog(LOG_NOTICE,"DEL_LIMIT hard limit (%" PRIu32 " per server) reached",MaxDelHardLimit);
				TmpMaxDelFrac=MaxDelHardLimit;
			}
			TmpMaxDel = TmpMaxDelFrac;
			syslog(LOG_NOTICE,"DEL_LIMIT temporary increased to: %" PRIu32 " per server",TmpMaxDel);
		}
		if ((toDeleteCount < prevToDeleteCount_) && (TmpMaxDelFrac > MaxDelSoftLimit)) {
			TmpMaxDelFrac /= 1.5;
			if (TmpMaxDelFrac<MaxDelSoftLimit) {
				syslog(LOG_NOTICE,"DEL_LIMIT back to soft limit (%" PRIu32 " per server)",MaxDelSoftLimit);
				TmpMaxDelFrac = MaxDelSoftLimit;
			}
			TmpMaxDel = TmpMaxDelFrac;
			syslog(LOG_NOTICE,"DEL_LIMIT decreased back to: %" PRIu32 " per server",TmpMaxDel);
		}
		prevToDeleteCount_ = toDeleteCount;
		deleteNotDone_ = 0;
		deleteDone_ = 0;
	}
	chunksinfo = inforec_;
	memset(&inforec_,0,sizeof(inforec_));
	chunksinfo_loopstart = chunksinfo_loopend;
	chunksinfo_loopend = main_time();
}

void ChunkWorker::doEverySecondTasks() {
	sortedServers_ = matocsserv_getservers_sorted();
	labeledSortedServers_.clear();
	for (const ServerWithUsage& sw : sortedServers_) {
		labeledSortedServers_[sw.label].push_back(sw);
	}
}

static bool chunkPresentOnServer(chunk *c, matocsserventry *server) {
	for (slist *s = c->slisthead; s; s = s->next) {
		if (s->ptr == server) {
			return true;
		}
	}
	return false;
}

static bool chunkPresentOnServer(chunk *c, Goal::Slice::Type slice_type, matocsserventry *server) {
	for (slist *s = c->slisthead; s; s = s->next) {
		if (s->ptr == server && s->chunkType.getSliceType() == slice_type) {
			return true;
		}
	}
	return false;
}

bool ChunkWorker::tryReplication(chunk *c, ChunkPartType part_to_recover,
				matocsserventry *destinationServer) {
	// TODO(msulikowski) Prefer VALID over TDVALID copies.
	// NOTE: we don't allow replicating xor chunks from pre-xor chunkservers
	std::vector<matocsserventry *> standard_servers;
	std::vector<matocsserventry *> xor_capable_servers;
	std::vector<ChunkPartType> xor_capable_parts;
	ChunkCopiesCalculator xor_capable_calc(c->getGoal());

	for (slist *s = c->slisthead; s; s = s->next) {
		if (s->is_valid() && !s->is_busy()) {
			if (matocsserv_get_version(s->ptr) >= kFirstXorVersion) {
				xor_capable_servers.push_back(s->ptr);
				xor_capable_parts.push_back(s->chunkType);
				xor_capable_calc.addPart(s->chunkType,
				                         matocsserv_get_label(s->ptr));
			}
			if (slice_traits::isStandard(s->chunkType)) {
				standard_servers.push_back(s->ptr);
			}
		}
	}

	// we calculate only chunk state here
	// target optimization is not required
	xor_capable_calc.evalState();

	if (xor_capable_calc.isRecoveryPossible() &&
	    matocsserv_get_version(destinationServer) >= kFirstXorVersion) {
		// new replication possible - use it
		matocsserv_send_liz_replicatechunk(destinationServer, c->chunkid, c->version,
		                                   part_to_recover, xor_capable_servers,
		                                   xor_capable_parts);
	} else if (slice_traits::isStandard(part_to_recover) && !standard_servers.empty()) {
		// fall back to legacy replication
		matocsserv_send_replicatechunk(
		        destinationServer, c->chunkid, c->version,
		        standard_servers[rnd_ranged<uint32_t>(standard_servers.size())]);
	} else {
		// no replication possible
		return false;
	}
	stats_replications++;
	c->needverincrease = 1;
	return true;
}

void ChunkWorker::deleteInvalidChunkParts(chunk *c) {
	for (slist *s = c->slisthead; s; s = s->next) {
		if (matocsserv_deletion_counter(s->ptr) < TmpMaxDel) {
			if (!s->is_valid()) {
				if (s->valid == DEL) {
					syslog(LOG_WARNING,
					       "chunk hasn't been deleted since previous loop - "
					       "retry");
				}
				s->valid = DEL;
				stats_deletions++;
				matocsserv_send_deletechunk(s->ptr, c->chunkid, 0, s->chunkType);
				inforec_.done.del_invalid++;
				deleteDone_++;
			}
		} else {
			if (s->valid == INVALID) {
				inforec_.notdone.del_invalid++;
				deleteNotDone_++;
			}
		}
	}
}

void ChunkWorker::deleteAllChunkParts(chunk *c) {
	for (slist *s = c->slisthead; s; s = s->next) {
		if (matocsserv_deletion_counter(s->ptr) < TmpMaxDel) {
			if (s->is_valid() && !s->is_busy()) {
				c->deleteCopy(s);
				c->needverincrease = 1;
				stats_deletions++;
				matocsserv_send_deletechunk(s->ptr, c->chunkid, c->version,
				                            s->chunkType);
				inforec_.done.del_unused++;
				deleteDone_++;
			}
		} else {
			if (s->valid == VALID || s->valid == TDVALID) {
				inforec_.notdone.del_unused++;
				deleteNotDone_++;
			}
		}
	}
}

bool ChunkWorker::replicateChunkPart(chunk *c, Goal::Slice::Type slice_type, int slice_part,
					ChunkCopiesCalculator &calc) {
	std::vector<matocsserventry *> servers;
	int skipped_replications = 0, valid_parts_count = 0, expected_copies = 0;
	bool tried_to_replicate = false;
	Goal::Slice::Labels replicate_labels;

	replicate_labels = calc.getLabelsToRecover(slice_type, slice_part);

	if (calc.getAvailable().find(slice_type) != calc.getAvailable().end()) {
		valid_parts_count =
		        Goal::Slice::countLabels(calc.getAvailable()[slice_type][slice_part]);
	}

	expected_copies = Goal::Slice::countLabels(calc.getTarget()[slice_type][slice_part]);

	for (const auto &label_and_count : replicate_labels) {
		tried_to_replicate = true;

		if (jobsnorepbefore >= main_time()) {
			break;
		}

		if (label_and_count.first == MediaLabel::kWildcard) {
			if (!replicationDelayInfoForAll.replicationAllowed(
			            label_and_count.second)) {
				continue;
			}
		} else if (!replicationDelayInfoForLabel[label_and_count.first].replicationAllowed(
		                   label_and_count.second)) {
			skipped_replications += label_and_count.second;
			continue;
		}

		// Get a list of possible destination servers
		int total_matching, returned_matching, temporarily_unavailable;
		matocsserv_getservers_lessrepl(label_and_count.first, MaxWriteRepl, servers,
		                               total_matching, returned_matching, temporarily_unavailable);

		// Find a destination server for replication -- the first one without a copy of 'c'
		matocsserventry *destination = nullptr;
		matocsserventry *backup_destination = nullptr;
		for (const auto &server : servers) {
			if (!chunkPresentOnServer(c, server)) {
				destination = server;
				break;
			}
			if (backup_destination == nullptr && !chunkPresentOnServer(c, slice_type, server)) {
				backup_destination = server;
			}
		}

		// If destination was not found, use a backup one, i.e. server where
		// there is a copy of this chunk, but from different slice.
		// Do it only if there are no available chunkservers in the system,
		// not if they merely reached their replication limit.
		if (destination == nullptr && temporarily_unavailable == 0) {
			// there are no servers which are expected to be available soon,
			// so backup server must be used
			destination = backup_destination;
		}

		if (destination == nullptr) {
			// there is no server suitable for replication to be written to
			break;
		}

		if (!(label_and_count.first == MediaLabel::kWildcard ||
		      matocsserv_get_label(destination) == label_and_count.first)) {
			// found server doesn't match requested label
			if (total_matching > returned_matching) {
				// There is a server which matches the current label, but it has
				// exceeded the
				// replication limit. In this case we won't try to use servers with
				// non-matching
				// labels as our destination -- we will wait for that server to be
				// ready.
				skipped_replications += label_and_count.second;
				continue;
			}
			if (valid_parts_count + skipped_replications >= expected_copies) {
				// Don't create copies on non-matching servers if there already are
				// enough replicas.
				continue;
			}
		}

		if (tryReplication(c, ChunkPartType(slice_type, slice_part), destination)) {
			inforec_.done.copy_undergoal++;
			return true;
		} else {
			// There is no server suitable for replication to be read from
			skipped_replications += label_and_count.second;
			break;  // there's no need to analyze other labels if there's no free source
			        // server
		}
	}
	if (tried_to_replicate) {
		inforec_.notdone.copy_undergoal++;
		// Enqueue chunk again only if it was taken directly from endangered chunks queue
		// to avoid repetitions. If it was taken from chunk hashmap, inEndangeredQueue bit
		// would be still up.
		if (gEndangeredChunksServingLimit > 0 && chunk::endangeredChunks.size() < gEndangeredChunksMaxCapacity
			&& !c->inEndangeredQueue && calc.getState() == ChunksAvailabilityState::kEndangered) {
			c->inEndangeredQueue = 1;
			chunk::endangeredChunks.push_back(c);
		}
	}

	return false;
}

bool ChunkWorker::removeUnneededChunkPart(chunk *c, Goal::Slice::Type slice_type, int slice_part,
					ChunkCopiesCalculator &calc) {
	Goal::Slice::Labels remove_pool = calc.getRemovePool(slice_type, slice_part);
	if (remove_pool.empty()) {
		return false;
	}

	slist *candidate = nullptr;
	bool candidate_todel = false;
	double candidate_usage = std::numeric_limits<double>::lowest();
	for (slist *s = c->slisthead; s != nullptr; s = s->next) {
		if (!s->is_valid() || s->chunkType != ChunkPartType(slice_type, slice_part)) {
			continue;
		}
		if (matocsserv_deletion_counter(s->ptr) >= TmpMaxDel) {
			continue;
		}

		MediaLabel server_label = matocsserv_get_label(s->ptr);
		if (remove_pool.find(server_label) == remove_pool.end()) {
			continue;
		}

		bool is_todel = s->is_todel();
		double usage = matocsserv_get_usage(s->ptr);
		if (std::make_pair(is_todel, usage) > std::make_pair(candidate_todel, candidate_usage)) {
			candidate = s;
			candidate_usage = usage;
			candidate_todel = is_todel;
		}
	}

	if (candidate &&
	    calc.canRemovePart(slice_type, slice_part, matocsserv_get_label(candidate->ptr))) {
		c->deleteCopy(candidate);
		c->needverincrease = 1;
		stats_deletions++;
		matocsserv_send_deletechunk(candidate->ptr, c->chunkid, 0, candidate->chunkType);

		int overgoal_copies = calc.countPartsToMove(slice_type, slice_part).second;

		inforec_.done.del_overgoal++;
		deleteDone_++;
		inforec_.notdone.del_overgoal += overgoal_copies - 1;
		deleteNotDone_ += overgoal_copies - 1;

		return true;
	}

	return false;
}

bool ChunkWorker::rebalanceChunkParts(chunk *c, ChunkCopiesCalculator &calc, bool only_todel) {
	if(!only_todel) {
		double min_usage = sortedServers_.front().diskUsage;
		double max_usage = sortedServers_.back().diskUsage;
		if ((max_usage - min_usage) <= AcceptableDifference) {
			return false;
		}
	}

	// Consider each copy to be moved to a server with disk usage much less than actual.
	// There are at least two servers with a disk usage difference grater than
	// AcceptableDifference, so it's worth checking.
	for (slist *s = c->slisthead; s != nullptr; s = s->next) {
		if (!s->is_valid() || matocsserv_replication_read_counter(s->ptr) >= MaxReadRepl) {
			continue;
		}

		if(only_todel && !s->is_todel()) {
			continue;
		}

		MediaLabel current_copy_label = matocsserv_get_label(s->ptr);
		double current_copy_disk_usage = matocsserv_get_usage(s->ptr);
		// Look for a server that has disk usage much less than currentCopyDiskUsage.
		// If such a server exists consider creating a new copy of this chunk there.
		// First, choose all possible candidates for the destination server: we consider
		// only
		// servers with the same label is rebalancing between labels if turned off or the
		// goal
		// requires our copy to exist on a server labeled 'currentCopyLabel'.
		bool multi_label_rebalance =
		        RebalancingBetweenLabels &&
		        (current_copy_label == MediaLabel::kWildcard ||
		         calc.canMovePartToDifferentLabel(s->chunkType.getSliceType(),
		                                          s->chunkType.getSlicePart(),
		                                          current_copy_label));

		const ServersWithUsage &sorted_servers =
		        multi_label_rebalance ? sortedServers_
		                              : labeledSortedServers_[current_copy_label];
		for (const auto &empty_server : sorted_servers) {
			if (!only_todel && empty_server.diskUsage >
			    current_copy_disk_usage - AcceptableDifference) {
				break;  // No more suitable destination servers (next servers have
				        // higher usage)
			}
			if (slice_traits::isXor(s->chunkType) &&
			    matocsserv_get_version(empty_server.server) < kFirstXorVersion) {
				continue;  // We can't place xor chunks on old servers
			}
			if (chunkPresentOnServer(c, s->chunkType.getSliceType(),
			                         empty_server.server)) {
				continue;  // A copy is already here
			}
			if (matocsserv_replication_write_counter(empty_server.server) >=
			    MaxWriteRepl) {
				continue;  // We can't create a new copy here
			}
			if (tryReplication(c, s->chunkType, empty_server.server)) {
				inforec_.copy_rebalance++;
				return true;
			}
		}
	}

	return false;
}

void ChunkWorker::doChunkJobs(chunk *c, uint16_t serverCount) {
	// step 0. Update chunk's statistics
	// Useful e.g. if definitions of goals did change.
	c->updateStats();
	if (serverCount == 0) {
		return;
	}

	int invalid_parts = 0;
	ChunkCopiesCalculator calc(c->getGoal());

	// Chunk is in degenerate state if it has more than 1 part
	// on the same chunkserver (i.e. 1 std and 1 xor)
	// TODO(sarna): this flat_set should be removed after
	// 'slists' are rewritten to use sensible data structures
	bool degenerate = false;
	flat_set<matocsserventry *, small_vector<matocsserventry *, 64>> servers;

	// step 1. calculate number of valid and invalid copies
	for (slist *s = c->slisthead; s; s = s->next) {
		if (s->is_valid()) {
			calc.addPart(s->chunkType, matocsserv_get_label(s->ptr));
			if (!degenerate) {
				degenerate = servers.count(s->ptr) > 0;
				servers.insert(s->ptr);
			}
		} else {
			++invalid_parts;
		}
	}
	calc.optimize();

	// step 2. check number of copies
	if (c->isLost() && invalid_parts > 0 && calc.getAvailable().getExpectedCopies() == 0 &&
	    c->fileCount() > 0) {
		syslog(LOG_WARNING, "chunk %016" PRIX64
		                    " has only invalid copies (%d) - please repair it manually",
		       c->chunkid, invalid_parts);
		for (slist *s = c->slisthead; s; s = s->next) {
			syslog(LOG_NOTICE, "chunk %016" PRIX64 "_%08" PRIX32
			                   " - invalid copy on (%s - ver:%08" PRIX32 ")",
			       c->chunkid, c->version, matocsserv_getstrip(s->ptr), s->version);
		}
		return;
	}

	// step 3. delete invalid parts
	deleteInvalidChunkParts(c);

	// step 4. return if chunk is during some operation
	if (c->operation != NONE || (c->isLocked())) {
		return;
	}

	// step 5. check busy count
	for (slist *s = c->slisthead; s; s = s->next) {
		if (s->is_busy()) {
			syslog(LOG_WARNING, "chunk %016" PRIX64 " has unexpected BUSY copies",
			       c->chunkid);
			return;
		}
	}

	// step 6. delete unused chunk
	if (c->fileCount() == 0) {
		deleteAllChunkParts(c);
		return;
	}

	if (c->isLost()) {
		return;
	}

	// step 7. check if chunk needs any replication
	for (const auto &slice : calc.getTarget()) {
		for (int i = 0; i < slice.size(); ++i) {
			if (replicateChunkPart(c, slice.getType(), i, calc)) {
				return;
			}
		}
	}

	// Do not remove any parts if more than 1 part resides on 1 chunkserver
	if (degenerate && calc.countPartsToRecover() > 0) {
		return;
	}

	// step 8. if chunk has too many copies then delete some of them
	for (const auto &slice : calc.getAvailable()) {
		for (int i = 0; i < slice.size(); ++i) {
			std::pair<int, int> operations = calc.countPartsToMove(slice.getType(), i);
			if (operations.first > 0 || operations.second == 0) {
				// do not remove elements if some are missing
				continue;
			}

			if (removeUnneededChunkPart(c, slice.getType(), i, calc)) {
				return;
			}
		}
	}

	// step 9. If chunk has parts marked as "to delete" then move them to other servers
	if(rebalanceChunkParts(c, calc, true)) {
		return;
	}

	if (chunksinfo.notdone.copy_undergoal > 0 && chunksinfo.done.copy_undergoal > 0) {
		return;
	}

	// step 10. if there is too big difference between chunkservers then make copy of chunk from
	// a server with a high disk usage on a server with low disk usage
	rebalanceChunkParts(c, calc, false);
}

static std::unique_ptr<ChunkWorker> gChunkWorker;

void chunk_jobs_main(void) {
	uint32_t i,l,lc,r;
	uint16_t usableServerCount;
	double minUsage, maxUsage;
	chunk *c,**cp;
	Timeout work_limit{std::chrono::milliseconds(ChunksLoopTimeout)};

	if (starttime + ReplicationsDelayInit > main_time()) {
		return;
	}

	matocsserv_usagedifference(&minUsage, &maxUsage, &usableServerCount, nullptr);

	if (minUsage > maxUsage) {
		return;
	}

	gChunkWorker->doEverySecondTasks();

	// Serve endangered chunks first if it is possible to replicate
	size_t endangeredToServe = 0;
	if (jobsnorepbefore < main_time()) {
		endangeredToServe = std::min<uint64_t>(
				gEndangeredChunksServingLimit,
				chunk::endangeredChunks.size());
		for (uint64_t served = 0; served < endangeredToServe; ++served) {
			c = chunk::endangeredChunks.front();
			chunk::endangeredChunks.pop_front();
			c->inEndangeredQueue = 0;
			gChunkWorker->doChunkJobs(c, usableServerCount);
		}
	}
	lc = 0;
	for (i = 0 ; i < HashSteps - endangeredToServe && lc < HashCPS ; i++) {
		if (jobshpos==0) {
			gChunkWorker->doEveryLoopTasks();
		}
		// Delete unused chunks from structures
		l=0;
		cp = &(gChunksMetadata->chunkhash[jobshpos]);
		while ((c=*cp)!=NULL) {
			chunk_handle_disconnected_copies(c);
			if (c->fileCount()==0 && c->slisthead==NULL) {
				*cp = (c->next);
				chunk_delete(c);
			} else {
				cp = &(c->next);
				l++;
				lc++;
			}
		}
		if (l>0) {
			r = rnd_ranged<uint32_t>(l);
			l=0;
			// do jobs on rest of them
			for (c=gChunksMetadata->chunkhash[jobshpos] ; c ; c=c->next) {
				if (l>=r) {
					gChunkWorker->doChunkJobs(c, usableServerCount);
				}
				l++;
			}
			l=0;
			for (c=gChunksMetadata->chunkhash[jobshpos] ; l<r && c ; c=c->next) {
				gChunkWorker->doChunkJobs(c, usableServerCount);
				l++;
			}
		}
		jobshpos+=123; // if HASHSIZE is any power of 2 then any odd number is good here
		jobshpos%=HASHSIZE;

		if(work_limit.expired()) {
			break;
		}
	}
}

#endif

constexpr uint32_t kSerializedChunkSizeNoLockId = 16;
constexpr uint32_t kSerializedChunkSizeWithLockId = 20;
#define CHUNKCNT 1000

#ifdef METARESTORE

void chunk_dump(void) {
	chunk *c;
	uint32_t i;

	for (i=0 ; i<HASHSIZE ; i++) {
		for (c=gChunksMetadata->chunkhash[i] ; c ; c=c->next) {
			printf("*|i:%016" PRIX64 "|v:%08" PRIX32 "|g:%" PRIu8 "|t:%10" PRIu32 "\n",c->chunkid,c->version,c->highestIdGoal(),c->lockedto);
		}
	}
}

#endif

int chunk_load(FILE *fd, bool loadLockIds) {
	uint8_t hdr[8];
	const uint8_t *ptr;
	int32_t r;
	chunk *c;
// chunkdata
	uint64_t chunkid;

	if (fread(hdr,1,8,fd)!=8) {
		return -1;
	}
	ptr = hdr;
	gChunksMetadata->nextchunkid = get64bit(&ptr);
	int32_t serializedChunkSize = (loadLockIds
			? kSerializedChunkSizeWithLockId : kSerializedChunkSizeNoLockId);
	std::vector<uint8_t> loadbuff(serializedChunkSize);
	for (;;) {
		r = fread(loadbuff.data(), 1, serializedChunkSize, fd);
		if (r != serializedChunkSize) {
			return -1;
		}
		ptr = loadbuff.data();
		chunkid = get64bit(&ptr);
		if (chunkid>0) {
			uint32_t version = get32bit(&ptr);
			c = chunk_new(chunkid, version);
			c->lockedto = get32bit(&ptr);
			if (loadLockIds) {
				c->lockid = get32bit(&ptr);
			}
		} else {
			uint32_t version = get32bit(&ptr);
			uint32_t lockedto = get32bit(&ptr);
			if (version==0 && lockedto==0) {
				return 0;
			} else {
				return -1;
			}
		}
	}
	return 0;       // unreachable
}

void chunk_store(FILE *fd) {
	passert(gChunksMetadata);
	uint8_t hdr[8];
	uint8_t storebuff[kSerializedChunkSizeWithLockId * CHUNKCNT];
	uint8_t *ptr;
	uint32_t i,j;
	chunk *c;
// chunkdata
	uint64_t chunkid;
	uint32_t version;
	uint32_t lockedto, lockid;
	ptr = hdr;
	put64bit(&ptr,gChunksMetadata->nextchunkid);
	if (fwrite(hdr,1,8,fd)!=(size_t)8) {
		return;
	}
	j=0;
	ptr = storebuff;
	for (i=0 ; i<HASHSIZE ; i++) {
		for (c=gChunksMetadata->chunkhash[i] ; c ; c=c->next) {
#ifndef METARESTORE
			chunk_handle_disconnected_copies(c);
#endif
			chunkid = c->chunkid;
			put64bit(&ptr,chunkid);
			version = c->version;
			put32bit(&ptr,version);
			lockedto = c->lockedto;
			lockid = c->lockid;
			put32bit(&ptr,lockedto);
			put32bit(&ptr,lockid);
			j++;
			if (j==CHUNKCNT) {
				size_t writtenBlockSize = kSerializedChunkSizeWithLockId * CHUNKCNT;
				if (fwrite(storebuff, 1, writtenBlockSize, fd) != writtenBlockSize) {
					return;
				}
				j=0;
				ptr = storebuff;
			}
		}
	}
	memset(ptr, 0, kSerializedChunkSizeWithLockId);
	j++;
	size_t writtenBlockSize = kSerializedChunkSizeWithLockId * j;
	if (fwrite(storebuff, 1, writtenBlockSize, fd) != writtenBlockSize) {
		return;
	}
}

void chunk_unload(void) {
	delete gChunksMetadata;
	gChunksMetadata = nullptr;
}

void chunk_newfs(void) {
#ifndef METARESTORE
	chunk::count = 0;
#endif
	gChunksMetadata->nextchunkid = 1;
}

#ifndef METARESTORE
void chunk_become_master() {
	starttime = main_time();
	jobsnorepbefore = starttime + ReplicationsDelayInit;
	gChunkWorker = std::unique_ptr<ChunkWorker>(new ChunkWorker());
	gChunkLoopEventHandle = main_timeregister_ms(ChunksLoopPeriod, chunk_jobs_main);
	return;
}

void chunk_reload(void) {
	uint32_t repl;
	uint32_t looptime;

	ReplicationsDelayInit = cfg_getuint32("REPLICATIONS_DELAY_INIT",300);
	ReplicationsDelayDisconnect = cfg_getuint32("REPLICATIONS_DELAY_DISCONNECT",3600);

	uint32_t disableChunksDel = cfg_getuint32("DISABLE_CHUNKS_DEL", 0);
	if (disableChunksDel) {
		MaxDelSoftLimit = MaxDelHardLimit = 0;
	} else {
		uint32_t oldMaxDelSoftLimit = MaxDelSoftLimit;
		uint32_t oldMaxDelHardLimit = MaxDelHardLimit;

		MaxDelSoftLimit = cfg_getuint32("CHUNKS_SOFT_DEL_LIMIT",10);
		if (cfg_isdefined("CHUNKS_HARD_DEL_LIMIT")) {
			MaxDelHardLimit = cfg_getuint32("CHUNKS_HARD_DEL_LIMIT",25);
			if (MaxDelHardLimit<MaxDelSoftLimit) {
				MaxDelSoftLimit = MaxDelHardLimit;
				syslog(LOG_WARNING,"CHUNKS_SOFT_DEL_LIMIT is greater than CHUNKS_HARD_DEL_LIMIT - using CHUNKS_HARD_DEL_LIMIT for both");
			}
		} else {
			MaxDelHardLimit = 3 * MaxDelSoftLimit;
		}
		if (MaxDelSoftLimit==0) {
			MaxDelSoftLimit = oldMaxDelSoftLimit;
			MaxDelHardLimit = oldMaxDelHardLimit;
		}
	}
	if (TmpMaxDelFrac<MaxDelSoftLimit) {
		TmpMaxDelFrac = MaxDelSoftLimit;
	}
	if (TmpMaxDelFrac>MaxDelHardLimit) {
		TmpMaxDelFrac = MaxDelHardLimit;
	}
	if (TmpMaxDel<MaxDelSoftLimit) {
		TmpMaxDel = MaxDelSoftLimit;
	}
	if (TmpMaxDel>MaxDelHardLimit) {
		TmpMaxDel = MaxDelHardLimit;
	}

	repl = cfg_getuint32("CHUNKS_WRITE_REP_LIMIT", 2);
	if (repl > 0) {
		MaxWriteRepl = repl;
	}

	repl = cfg_getuint32("CHUNKS_READ_REP_LIMIT", 10);
	if (repl > 0) {
		MaxReadRepl = repl;
	}

	ChunksLoopPeriod = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_PERIOD", 1000, MINCHUNKSLOOPPERIOD, MAXCHUNKSLOOPPERIOD);
	if (gChunkLoopEventHandle) {
		main_timechange_ms(gChunkLoopEventHandle, ChunksLoopPeriod);
	}

	repl = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MAX_CPU", 60, MINCHUNKSLOOPCPU, MAXCHUNKSLOOPCPU);
	ChunksLoopTimeout = repl * ChunksLoopPeriod / 100;

	if (cfg_isdefined("CHUNKS_LOOP_TIME")) {
		looptime = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_TIME", 300, MINLOOPTIME, MAXLOOPTIME);
		uint64_t scaled_looptime = std::max((uint64_t)1000 * looptime / ChunksLoopPeriod, (uint64_t)1);
		HashSteps = 1 + ((HASHSIZE) / scaled_looptime);
		HashCPS   = 0xFFFFFFFF;
	} else {
		looptime = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MIN_TIME", 300, MINLOOPTIME, MAXLOOPTIME);
		HashCPS = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MAX_CPS", 100000, MINCPS, MAXCPS);
		uint64_t scaled_looptime = std::max((uint64_t)1000 * looptime / ChunksLoopPeriod, (uint64_t)1);
		HashSteps = 1 + ((HASHSIZE) / scaled_looptime);
		HashCPS   = (uint64_t)ChunksLoopPeriod * HashCPS / 1000;
	}
	double endangeredChunksPriority = cfg_ranged_get("ENDANGERED_CHUNKS_PRIORITY", 0.0, 0.0, 1.0);
	gEndangeredChunksServingLimit = HashSteps * endangeredChunksPriority;
	gEndangeredChunksMaxCapacity = cfg_get("ENDANGERED_CHUNKS_MAX_CAPACITY", static_cast<uint64_t>(1024*1024UL));
	AcceptableDifference = cfg_ranged_get("ACCEPTABLE_DIFFERENCE",0.1, 0.001, 10.0);
	RebalancingBetweenLabels = cfg_getuint32("CHUNKS_REBALANCING_BETWEEN_LABELS", 0) == 1;
}
#endif

int chunk_strinit(void) {
	gChunksMetadata = new ChunksMetadata;

#ifndef METARESTORE
	chunk::count = 0;
	for (int i = 0; i < CHUNK_MATRIX_SIZE; ++i) {
		for (int j = 0; j < CHUNK_MATRIX_SIZE; ++j) {
			chunk::allFullChunkCopies[i][j] = 0;
			chunk::regularFullChunkCopies[i][j] = 0;
		}
	}
	chunk::allChunksAvailability = ChunksAvailabilityState();
	chunk::regularChunksAvailability = ChunksAvailabilityState();
	chunk::allChunksReplicationState = ChunksReplicationState();
	chunk::regularChunksReplicationState = ChunksReplicationState();

	uint32_t disableChunksDel = cfg_getuint32("DISABLE_CHUNKS_DEL", 0);
	ReplicationsDelayInit = cfg_getuint32("REPLICATIONS_DELAY_INIT",300);
	ReplicationsDelayDisconnect = cfg_getuint32("REPLICATIONS_DELAY_DISCONNECT",3600);
	if (disableChunksDel) {
		MaxDelHardLimit = MaxDelSoftLimit = 0;
	} else {
		MaxDelSoftLimit = cfg_getuint32("CHUNKS_SOFT_DEL_LIMIT",10);
		if (cfg_isdefined("CHUNKS_HARD_DEL_LIMIT")) {
			MaxDelHardLimit = cfg_getuint32("CHUNKS_HARD_DEL_LIMIT",25);
			if (MaxDelHardLimit<MaxDelSoftLimit) {
				MaxDelSoftLimit = MaxDelHardLimit;
				lzfs_pretty_syslog(LOG_WARNING, "%s: CHUNKS_SOFT_DEL_LIMIT is greater than "
					"CHUNKS_HARD_DEL_LIMIT - using CHUNKS_HARD_DEL_LIMIT for both",
					cfg_filename().c_str());
			}
		} else {
			MaxDelHardLimit = 3 * MaxDelSoftLimit;
		}
		if (MaxDelSoftLimit == 0) {
			throw InitializeException(cfg_filename() + ": CHUNKS_SOFT_DEL_LIMIT is zero");
		}
	}
	TmpMaxDelFrac = MaxDelSoftLimit;
	TmpMaxDel = MaxDelSoftLimit;
	MaxWriteRepl = cfg_getuint32("CHUNKS_WRITE_REP_LIMIT",2);
	MaxReadRepl = cfg_getuint32("CHUNKS_READ_REP_LIMIT",10);
	if (MaxReadRepl==0) {
		throw InitializeException(cfg_filename() + ": CHUNKS_READ_REP_LIMIT is zero");
	}
	if (MaxWriteRepl==0) {
		throw InitializeException(cfg_filename() + ": CHUNKS_WRITE_REP_LIMIT is zero");
	}

	ChunksLoopPeriod  = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_PERIOD", 1000, MINCHUNKSLOOPPERIOD, MAXCHUNKSLOOPPERIOD);
	uint32_t repl = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MAX_CPU", 60, MINCHUNKSLOOPCPU, MAXCHUNKSLOOPCPU);
	ChunksLoopTimeout = repl * ChunksLoopPeriod / 100;

	uint32_t looptime;
	if (cfg_isdefined("CHUNKS_LOOP_TIME")) {
		lzfs_pretty_syslog(LOG_WARNING,
				"%s: defining loop time by CHUNKS_LOOP_TIME option is "
				"deprecated - use CHUNKS_LOOP_MAX_CPS and CHUNKS_LOOP_MIN_TIME",
				cfg_filename().c_str());
		looptime = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_TIME", 300, MINLOOPTIME, MAXLOOPTIME);
		uint64_t scaled_looptime = std::max((uint64_t)1000 * looptime / ChunksLoopPeriod, (uint64_t)1);
		HashSteps = 1 + ((HASHSIZE) / scaled_looptime);
		HashCPS   = 0xFFFFFFFF;
	} else {
		looptime = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MIN_TIME", 300, MINLOOPTIME, MAXLOOPTIME);
		HashCPS = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MAX_CPS", 100000, MINCPS, MAXCPS);
		uint64_t scaled_looptime = std::max((uint64_t)1000 * looptime / ChunksLoopPeriod, (uint64_t)1);
		HashSteps = 1 + ((HASHSIZE) / scaled_looptime);
		HashCPS   = (uint64_t)ChunksLoopPeriod * HashCPS / 1000;
	}
	double endangeredChunksPriority = cfg_ranged_get("ENDANGERED_CHUNKS_PRIORITY", 0.0, 0.0, 1.0);
	gEndangeredChunksServingLimit = HashSteps * endangeredChunksPriority;
	gEndangeredChunksMaxCapacity = cfg_get("ENDANGERED_CHUNKS_MAX_CAPACITY", static_cast<uint64_t>(1024*1024UL));
	AcceptableDifference = cfg_ranged_get("ACCEPTABLE_DIFFERENCE", 0.1, 0.001, 10.0);
	RebalancingBetweenLabels = cfg_getuint32("CHUNKS_REBALANCING_BETWEEN_LABELS", 0) == 1;
	jobshpos = 0;
	main_reloadregister(chunk_reload);
	main_destructregister(chunk_clean_zombie_servers);
	metadataserver::registerFunctionCalledOnPromotion(chunk_become_master);
	main_canexitregister(chunk_canexit);
	main_eachloopregister(chunk_clean_zombie_servers_a_bit);
	if (metadataserver::isMaster()) {
		chunk_become_master();
	}
#endif
	return 1;
}

