/*
 * aligner_sw_driver.cpp
 *
 * Routines that drive the alignment process given a collection of seed hits.
 * This is generally done in a few stages: extendSeeds visits the set of
 * seed-hit BW elements in some order; for each element visited it resolves its
 * reference offset; once the reference offset is known, bounds for a dynamic
 * programming subproblem are established; if these bounds are distinct from
 * the bounds we've already tried, we solve the dynamic programming subproblem
 * and report the hit; if the AlnSinkWrap indicates that we can stop, we
 * return, otherwise we continue on to the next BW element.
 */

#include <iostream>
#include "aligner_cache.h"
#include "aligner_sw_driver.h"
#include "pe.h"
#include "dp_framer.h"

using namespace std;

/**
 * Given seed results, set up all of our state for resolving and keeping
 * track of reference offsets for hits.
 */
void SwDriver::setUpSaRangeState(
	SeedResults& sh,             // seed hits to extend into full alignments
	const Ebwt& ebwt,            // BWT
	const BitPairReference& ref, // Reference strings
	size_t maxrows,              // max rows to consider per position
	AlignmentCacheIface& ca,     // alignment cache for seed hits
	RandomSource& rnd,           // pseudo-random generator
	WalkMetrics& wlm)            // group walk left metrics
{
	const size_t nonz = sh.nonzeroOffsets();
	gws_.clear();     gws_.resize(nonz);
	satups_.clear();  satups_.resize(nonz);
	satups2_.clear(); satups2_.resize(nonz);
	sacomb_.clear();  sacomb_.resize(nonz);
	for(size_t i = 0; i < nonz; i++) {
		bool fw = true;
		uint32_t offidx = 0, rdoff = 0, seedlen = 0;
		QVal qv = sh.hitsByRank(i, offidx, rdoff, fw, seedlen);
		assert(qv.repOk(ca.current()));
		satups_[i].clear();
		satups2_[i].clear();
		sacomb_[i].clear();
		ca.queryQval(qv, satups_[i]);
		EList<SATuple, 16> *satups = &satups_[i];
		// Whittle down the rows in satups_ according to 'maxrows'
		if(maxrows != 0xffffffff && SATuple::randomNarrow(
			satups_[i],
			satups2_[i],
			rnd,
			maxrows))
		{
			satups_[i] = satups2_[i];
		}
		sacomb_[i].resize(satups->size());
		for(size_t j = 0 ; j < sacomb_[i].size(); j++) {
			sacomb_[i][j].init((*satups)[j]);
		}
		gws_[i].initQval(
			ebwt,    // forward Bowtie index
			ref,     // reference sequences
			qv,      // QVal describing BW ranges for this seed hit
			*satups, // SA tuples: ref hit, salist range
			sacomb_[i], // Combiner for resolvers
			ca,      // current cache
			rnd,     // pseudo-random generator
			true,    // use results list?
			wlm);    // metrics
		assert(gws_[i].initialized());
	}
}

/**
 * Given a collection of SeedHits for a single read, extend seed alignments
 * into full alignments.  Where possible, try to avoid redundant offset lookups
 * and dynamic programming wherever possible.  Optionally report alignments to
 * a AlnSinkWrap object as they are discovered.
 *
 * If 'reportImmediately' is true, returns true iff a call to msink->report()
 * returned true (indicating that the reporting policy is satisfied and we can
 * stop).  Otherwise, returns false.
 */
bool SwDriver::extendSeeds(
	const Read& rd,              // read to align
	bool mate1,                  // true iff rd is mate #1
	bool color,                  // true -> read is colorspace
	SeedResults& sh,             // seed hits to extend into full alignments
	const Ebwt& ebwt,            // BWT
	const BitPairReference& ref, // Reference strings
	SwAligner& swa,              // dynamic programming aligner
	const Scoring& sc,           // scoring scheme
	int seedmms,                 // # mismatches allowed in seed
	int seedlen,                 // length of seed
	int seedival,                // interval between seeds
	TAlScore minsc,              // minimum score for anchor
	TAlScore floorsc,            // local-alignment floor for anchor score
	int nceil,                   // maximum # Ns permitted in reference portion
	float posmin,                // minimum number of positions to examine
	float posfrac,               // max number of additional poss to examine
	float rowmin,                // minimum number of extensions to try
	float rowmult,               // number of extensions to try per pos
	size_t maxhalf,  	         // max width in either direction for DP tables
	AlignmentCacheIface& ca,     // alignment cache for seed hits
	RandomSource& rnd,           // pseudo-random source
	WalkMetrics& wlm,            // group walk left metrics
	SwMetrics& swmSeed,          // DP metrics for seed-extend
	AlnSinkWrap* msink,          // AlnSink wrapper for multiseed-style aligner
	bool reportImmediately,      // whether to report hits immediately to msink
	EList<SwCounterSink*>* swCounterSinks, // send counter updates to these
	EList<SwActionSink*>* swActionSinks)   // send action-list updates to these
{
	assert(!reportImmediately || msink != NULL);
	assert(!reportImmediately || msink->empty());
	assert(!reportImmediately || !msink->maxed());

	// Calculate the largest possible number of read and reference gaps
	const size_t rdlen = rd.length();
	int readGaps = sc.maxReadGaps(minsc, rdlen);
	int refGaps  = sc.maxRefGaps(minsc, rdlen);
	
	size_t maxrows = (size_t)(rowmult + 0.5f);

	DynProgFramer dpframe(!gReportOverhangs);

	// Initialize a set of GroupWalks, one for each seed.  Also, intialize the
	// accompanying lists of reference seed hits (satups*) and the combiners
	// that link the reference-scanning results to the BW walking results
	// (sacomb_).
	setUpSaRangeState(
		sh,      // seed hits to extend into full alignments
		ebwt,    // BWT
		ref,     // Reference strings
		maxrows, // max rows to consider per position
		ca,      // alignment cache for seed hits
		rnd,     // pseudo-random generator
		wlm);    // group walk left metrics

	// Iterate twice through levels seed hits from the lowest ranked
	// level to the highest ranked.  On the first iteration, look for
	// entries for which the offset is already known and try SWs.  On
	// the second iteration, resolve entries for which the offset is
	// unknown and try SWs.
	const size_t nonz = sh.nonzeroOffsets();
	float possf = posmin + posfrac * (nonz-posmin) + 0.5f;
	possf = max(possf, 1.0f);
	possf = min(possf, (float)nonz);
	const size_t poss = (size_t)possf;
	size_t rows = rdlen + (color ? 1 : 0);
	for(size_t i = 0; i < poss; i++) {
		bool fw = true;
		uint32_t offidx = 0, rdoff = 0, seedlen = 0;
		// TODO: Right now we take a QVal and then investigate it until it's
		// exhausted.  We might instead keep a few different GroupWalkers
		// initialized with separate QVals and investigate them in tandem.
		QVal qv = sh.hitsByRank(i, offidx, rdoff, fw, seedlen);
		assert(qv.repOk(ca.current()));
		int advances = 0;
		ASSERT_ONLY(WalkResult lastwr);
		if(!fw) {
			// 'rdoff' and 'offidx' are with respect to the 5' end of
			// the read.  Here we convert rdoff to be with respect to
			// the upstream (3') end of ther read.
			rdoff = (uint32_t)(rdlen - rdoff - seedlen);
		}
		while(!gws_[i].done()) {
			// Resolve next element offset
			WalkResult wr;
			gws_[i].advanceQvalPos(wr, wlm);
			assert(wr.elt != lastwr.elt);
			ASSERT_ONLY(lastwr = wr);
			advances++;
			assert_neq(0xffffffff, wr.toff);
			Coord c(0, (TRefOff)wr.toff - rdoff, fw);
			if(!redSeed1_.insert(c)) {
				// Already tried to find an alignment at these coordinates
				swmSeed.rshit++;
				continue;
			}
			uint32_t tidx = 0, toff = 0, tlen = 0;
			ebwt.joinedToTextOff(
				wr.elt.len,
				wr.toff,
				tidx,
				toff,
				tlen);
			tlen += (color ? 1 : 0);
			if(tidx == 0xffffffff) {
				// The seed hit straddled a reference boundary so the seed hit
				// isn't valid
				continue;
			}
			// Now that we have a seed hit, there are many issues to solve
			// before we have a completely framed dynamic programming problem.
			// They include:
			//
			// 1. Setting reference offsets on either side of the seed hit,
			//    accounting for where the seed occurs in the read
			// 2. Adjusting the width of the banded dynamic programming problem
			//    and adjusting reference bounds to allow for gaps in the
			//    alignment
			// 3. Accounting for the edges of the reference, which can impact
			//    the width of the DP problem and reference bounds.
			// 4. Perhaps filtering the problem down to a smaller problem based
			//    on what DPs we've already solved for this read
			//
			// We do #1 here, since it is simple and we have all the seed-hit
			// information here.  #2 and #3 are handled in the DynProgFramer.
			
			// Find offset of alignment's upstream base assuming net gaps=0
			// between beginning of read and beginning of seed hit
			int64_t refoff = (int64_t)toff - rdoff;
			size_t width = 0, trimup = 0, trimdn = 0;
			int64_t refl = 0, refr = 0;
			bool found = dpframe.frameSeedExtension(
				refoff,   // ref offset implied by seed hit assuming no gaps
				rows,     // length of read sequence used in DP table (so len
				          // of +1 nucleotide sequence for colorspace reads)
				tlen,     // length of reference
				readGaps, // max # of read gaps permitted in opp mate alignment
				refGaps,  // max # of ref gaps permitted in opp mate alignment
				maxhalf,  // max width in either direction
				width,    // out: calculated width stored here
				trimup,   // out: number of bases trimmed from upstream end
				trimdn,   // out: number of bases trimmed from downstream end
				refl,     // out: ref pos of upper LHS of parallelogram
				refr,     // out: ref pos of lower RHS of parallelogram
				st_,      // out: legal starting columns stored here
				en_);     // out: legal ending columns stored here
			if(!found) {
				continue;
			}
			assert_eq(width, st_.size());
			assert_eq(st_.size(), en_.size());
			// Given the boundaries defined by refl and refr, initilize the
			// SwAligner with the dynamic programming problem that aligns the
			// read to this reference stretch.
			swa.init(
				rd,        // read to align
				0,         // off of first char in 'rd' to consider
				rdlen,     // off of last char (excl) in 'rd' to consider
				fw,        // whether to align forward or revcomp read
				color,     // colorspace?
				tidx,      // reference aligned against
				refl,      // off of first character in ref to consider
				refr+1,    // off of last char (excl) in ref to consider
				ref,       // Reference strings
				tlen,      // length of reference sequence
				width,     // # bands to do (width of parallelogram)
				&st_,      // mask indicating which columns we can start in
				&en_,      // mask indicating which columns we can end in
				sc,        // scoring scheme
				minsc,     // minimum score for valid alignments
				floorsc,   // local-alignment floor score
				nceil);    // max # Ns
			// Now fill the dynamic programming matrix and return true iff
			// there is at least one valid alignment
			found = swa.align(rnd);
			swa.mergeAlignCounters(
				swmSeed.sws,
				swmSeed.swcups,
				swmSeed.swrows,
				swmSeed.swskiprows,
				swmSeed.swsucc,
				swmSeed.swfail);
			swa.resetAlignCounters();
			if(!found) {
				continue; // Look for more anchor alignments
			}
			while(true) {
				res_.reset();
				assert(res_.empty());
				if(swa.done()) {
					break;
				}
				swa.nextAlignment(res_, rnd);
				swa.mergeBacktraceCounters(swmSeed.swbts);
				swa.resetBacktraceCounters();
				found = !res_.empty();
				if(!found) {
					break;
				}
				// User specified that alignments overhanging ends of reference
				// should be excluded...
				assert(gReportOverhangs || res_.alres.within(tidx, 0, fw, tlen));
				// Is this alignment redundant with one we've seen previously?
				if(redAnchor_.overlap(res_.alres)) {
					// Redundant with an alignment we found already
					continue;
				}
				redAnchor_.add(res_.alres);
				// Annotate the AlnRes object with some key parameters
				// that were used to obtain the alignment.
				res_.alres.setParams(
					seedmms,   // # mismatches allowed in seed
					seedlen,   // length of seed
					seedival,  // interval between seeds
					minsc,     // minimum score for valid alignment
					floorsc);  // local-alignment floor score
				
				if(reportImmediately) {
					assert(msink != NULL);
					assert(res_.repOk());
					// Check that alignment accurately reflects the
					// reference characters aligned to
					assert(res_.alres.matchesRef(rd, ref));
					// Report an unpaired alignment
					assert(!msink->maxed());
					if(msink->report(
						0,
						mate1 ? &res_.alres : NULL,
						mate1 ? NULL : &res_.alres))
					{
						// Short-circuited because a limit, e.g. -k, -m or
						// -M, was exceeded
						return true;
					}
				}
			}

			// At this point we know that we aren't bailing, and will continue to resolve seed hits.  

		} // while(!gws_[i].done())
	}
	return false;
}

/**
 * Given a read, perform full dynamic programming against the entire
 * reference.  Optionally report alignments to a AlnSinkWrap object
 * as they are discovered.
 *
 * If 'reportImmediately' is true, returns true iff a call to
 * msink->report() returned true (indicating that the reporting
 * policy is satisfied and we can stop).  Otherwise, returns false.
 */
bool SwDriver::sw(
	const Read& rd,              // read to align
	bool color,                  // true -> read is colorspace
	const BitPairReference& ref, // Reference strings
	SwAligner& swa,              // dynamic programming aligner
	const Scoring& sc,           // scoring scheme
	TAlScore minsc,              // minimum score for valid alignment
	TAlScore floorsc,            // local-alignment floor score
	RandomSource& rnd,           // pseudo-random source
	SwMetrics& swm,              // dynamic programming metrics
	AlnSinkWrap* msink,          // HitSink for multiseed-style aligner
	bool reportImmediately,      // whether to report hits immediately to msink
	EList<SwCounterSink*>* swCounterSinks, // send counter updates to these
	EList<SwActionSink*>* swActionSinks)   // send action-list updates to these
{
	assert(!reportImmediately || msink != NULL);
	return false;
}

/**
 * Given a collection of SeedHits for both mates in a read pair, extend seed
 * alignments into full alignments and then look for the opposite mate using
 * dynamic programming.  Where possible, try to avoid redundant offset lookups.
 * Optionally report alignments to a AlnSinkWrap object as they are discovered.
 *
 * If 'reportImmediately' is true, returns true iff a call to
 * msink->report() returned true (indicating that the reporting
 * policy is satisfied and we can stop).  Otherwise, returns false.
 *
 * REDUNDANT SEED HITS
 *
 * See notes at top of aligner_sw_driver.h.
 *
 * REDUNDANT ALIGNMENTS
 *
 * See notes at top of aligner_sw_driver.h.
 *
 * MIXING PAIRED AND UNPAIRED ALIGNMENTS
 *
 * There are distinct paired-end alignment modes for the cases where (a) the
 * user does or does not want to see unpaired alignments for individual mates
 * when there are no reportable paired-end alignments involving both mates, and
 * (b) the user does or does not want to see discordant paired-end alignments.
 * The modes have implications for this function and for the AlnSinkWrap, since
 * it affects when we're "done."  Also, whether the user has asked us to report
 * discordant alignments affects whether and how much searching for unpaired
 * alignments we must do (i.e. if there are no paired-end alignments, we must
 * at least do -m 1 for both mates).
 *
 * Mode 1: Just concordant paired-end.  Print only concordant paired-end
 * alignments.  As soon as any limits (-k/-m/-M) are reached, stop.
 *
 * Mode 2: Concordant and discordant paired-end.  If -k/-m/-M limits are
 * reached for paired-end alignments, stop.  Otherwise, if no paired-end
 * alignments are found, align both mates in an unpaired -m 1 fashion.  If
 * there is exactly one unpaired alignment for each mate, report the
 * combination as a discordant alignment.
 *
 * Mode 3: Concordant paired-end if possible, otherwise unpaired.  If -k/-M
 * limit is reached for paired-end alignmnts, stop.  If -m limit is reached for
 * paired-end alignments or no paired-end alignments are found, align both
 * mates in an unpaired fashion.  All the same settings governing validity and
 * reportability in paired-end mode apply here too (-k/-m/-M/etc).
 *
 * Mode 4: Concordant or discordant paired-end if possible, otherwise unpaired.
 * If -k/-M limit is reached for paired-end alignmnts, stop.  If -m limit is
 * reached for paired-end alignments or no paired-end alignments are found,
 * align both mates in an unpaired fashion.  If the -m limit was reached, there
 * is no need to search for a discordant alignment, and unapired alignment can
 * proceed as in Mode 3.  If no paired-end alignments were found, then unpaired
 * alignment proceeds as in Mode 3 but with this caveat: alignment must be at
 * least as thorough as dictated by -m 1 up until the point where
 *
 * Print paired-end alignments when there are reportable paired-end
 * alignments, otherwise report reportable unpaired alignments.  If -k limit is
 * reached for paired-end alignments, stop.  If -m/-M limit is reached for
 * paired-end alignments, stop searching for paired-end alignments and look
 * only for unpaired alignments.  If searching only for unpaired alignments,
 * respect -k/-m/-M limits separately for both mates.
 *
 * The return value from the AlnSinkWrap's report member function must be
 * specific enough to distinguish between:
 *
 * 1. Stop searching for paired-end alignments
 * 2. Stop searching for alignments for unpaired alignments for mate #1
 * 3. Stop searching for alignments for unpaired alignments for mate #2
 * 4. Stop searching for any alignments
 *
 * Note that in Mode 2, options affecting validity and reportability of
 * alignments apply .  E.g. if -m 1 is specified
 *
 * WORKFLOW
 *
 * Our general approach to finding paired and unpaired alignments here
 * is as follows:
 *
 * - For mate in mate1, mate2:
 *   - For each seed hit in mate:
 *     - Try to extend it into a full alignment; if we can't, continue
 *       to the next seed hit
 *     - Look for alignment for opposite mate; if we can't find one,
 *     - 
 *     - 
 *
 */
bool SwDriver::extendSeedsPaired(
	const Read& rd,              // mate to align as anchor
	const Read& ord,             // mate to align as opposite
	bool anchor1,                // true iff anchor mate is mate1
	bool color,                  // true -> reads are colorspace
	SeedResults& sh,             // seed hits for anchor
	const Ebwt& ebwt,            // BWT
	const BitPairReference& ref, // Reference strings
	SwAligner& swa,              // dynamic programming aligner for anchor
	SwAligner& oswa,             // dynamic programming aligner for opposite
	const Scoring& sc,           // scoring scheme
	const PairedEndPolicy& pepol,// paired-end policy
	int seedmms,                 // # mismatches allowed in seed
	int seedlen,                 // length of seed
	int seedival,                // interval between seeds
	TAlScore minsc,              // minimum score for valid anchor aln
	TAlScore ominsc,             // minimum score for valid opposite aln
	TAlScore floorsc,            // local-alignment score floor for anchor
	TAlScore ofloorsc,           // local-alignment score floor for opposite
	int nceil,                   // max # Ns permitted in ref for anchor
	int onceil,                  // max # Ns permitted in ref for opposite
	bool nofw,                   // don't align forward read
	bool norc,                   // don't align revcomp read
	float posmin,                // minimum number of positions to examine
	float posfrac,               // max number of additional poss to examine
	float rowmin,                // minimum number of extensions
	float rowmult,               // number of extensions to try per pos
	size_t maxhalf,              // max width in either direction for DP tables
	AlignmentCacheIface& ca,     // alignment cache for seed hits
	RandomSource& rnd,           // pseudo-random source
	WalkMetrics& wlm,            // group walk left metrics
	SwMetrics& swmSeed,          // DP metrics for seed-extend
	SwMetrics& swmMate,          // DP metrics for mate finidng
	AlnSinkWrap* msink,          // AlnSink wrapper for multiseed-style aligner
	bool swMateImmediately,      // whether to look for mate immediately
	bool reportImmediately,      // whether to report hits immediately to msink
	bool discord,                // look for discordant alignments?
	bool mixed,                  // look for unpaired as well as paired alns?
	EList<SwCounterSink*>* swCounterSinks, // send counter updates to these
	EList<SwActionSink*>* swActionSinks)   // send action-list updates to these
{
	assert(!reportImmediately || msink != NULL);
	assert(!reportImmediately || !msink->maxed());
	assert(!msink->state().doneWithMate(anchor1));

	const size_t rdlen  = rd.length();
	const size_t ordlen = ord.length();

	// Calculate the largest possible number of read and reference gaps
	int readGaps  = sc.maxReadGaps(minsc,  rdlen);
	int refGaps   = sc.maxRefGaps (minsc,  rdlen);
	int oreadGaps = sc.maxReadGaps(ominsc, ordlen);
	int orefGaps  = sc.maxRefGaps (ominsc, ordlen);

	size_t maxrows = (size_t)(rowmult + 0.5f);

	const size_t rows   = rdlen  + (color ? 1 : 0);
	const size_t orows  = ordlen + (color ? 1 : 0);
	
	DynProgFramer dpframe(!gReportOverhangs);

	// Initialize a set of GroupWalks, one for each seed.  Also, intialize the
	// accompanying lists of reference seed hits (satups*) and the combiners
	// that link the reference-scanning results to the BW walking results
	// (sacomb_).
	setUpSaRangeState(
		sh,      // seed hits to extend into full alignments
		ebwt,    // BWT
		ref,     // Reference strings
		maxrows, // max rows to consider per position
		ca,      // alignment cache for seed hits
		rnd,     // pseudo-random generator
		wlm);    // group walk left metrics

	// Iterate twice through levels seed hits from the lowest ranked
	// level to the highest ranked.  On the first iteration, look for
	// entries for which the offset is already known and try SWs.  On
	// the second iteration, resolve entries for which the offset is
	// unknown and try SWs.
	const size_t nonz = sh.nonzeroOffsets();
	float possf = posmin + posfrac * (nonz-posmin) + 0.5f;
	possf = max(possf, 1.0f);
	possf = min(possf, (float)nonz);
	const size_t poss = (size_t)possf;
	for(size_t i = 0; i < poss; i++) {
		bool fw = true;
		uint32_t offidx = 0, rdoff = 0, seedlen = 0;
		// TODO: Right now we take a QVal and then investigate it until it's
		// exhausted.  We might instead keep a few different GroupWalkers
		// initialized with separate QVals and investigate them in tandem.
		QVal qv = sh.hitsByRank(i, offidx, rdoff, fw, seedlen);
		assert(qv.repOk(ca.current()));
		if(!fw) {
			// 'rdoff' and 'offidx' are with respect to the 5' end of
			// the read.  Here we convert rdoff to be with respect to
			// the upstream (3') end of ther read.
			rdoff = (uint32_t)(rdlen - rdoff - seedlen);
		}
		assert(!norc ||  fw);
		assert(!nofw || !fw);
		int advances = 0;
		ASSERT_ONLY(WalkResult lastwr);
		while(!gws_[i].done()) {
			// Resolve the next anchor seed hit
			assert(!msink->state().done());
			assert(!msink->state().doneWithMate(anchor1));
			WalkResult wr;
			gws_[i].advanceQvalPos(wr, wlm);
			assert(wr.elt != lastwr.elt);
			ASSERT_ONLY(lastwr = wr);
			advances++;
			assert_neq(0xffffffff, wr.toff);
			Coord c(0, (TRefOff)wr.toff - rdoff, fw);
			ESet<Coord>& redSeedAnchor = anchor1 ? redSeed1_ : redSeed2_;
			if(!redSeedAnchor.insert(c)) {
				// Already tried to find an alignment at these coordinates
				swmSeed.rshit++;
				continue;
			}
			uint32_t tidx = 0, toff = 0, tlen = 0;
			ebwt.joinedToTextOff(
				wr.elt.len,
				wr.toff,
				tidx,
				toff,
				tlen);
			tlen += (color ? 1 : 0);
			if(tidx == 0xffffffff) {
				// The seed hit straddled a reference boundary so the seed hit
				// isn't valid
				continue;
			}
			// Now that we have a seed hit, there are many issues to solve
			// before we have a completely framed dynamic programming problem.
			// They include:
			//
			// 1. Setting reference offsets on either side of the seed hit,
			//    accounting for where the seed occurs in the read
			// 2. Adjusting the width of the banded dynamic programming problem
			//    and adjusting reference bounds to allow for gaps in the
			//    alignment
			// 3. Accounting for the edges of the reference, which can impact
			//    the width of the DP problem and reference bounds.
			// 4. Perhaps filtering the problem down to a smaller problem based
			//    on what DPs we've already solved for this read
			//
			// We do #1 here, since it is simple and we have all the seed-hit
			// information here.  #2 and #3 are handled in the DynProgFramer.
			
			// Find offset of alignment's upstream base assuming net gaps=0
			// between beginning of read and beginning of seed hit
			int64_t refoff = (int64_t)toff - rdoff;
			size_t width = 0, trimup = 0, trimdn = 0;
			int64_t refl = 0, refr = 0;
			bool found = dpframe.frameSeedExtension(
				refoff,   // ref offset implied by seed hit assuming no gaps
				rows,     // length of read sequence used in DP table (so len
		                  // of +1 nucleotide sequence for colorspace reads)
				tlen,     // length of reference
				readGaps, // max # of read gaps permitted in opp mate alignment
				refGaps,  // max # of ref gaps permitted in opp mate alignment
				maxhalf,  // max width in either direction
				width,    // out: calculated width stored here
				trimup,   // out: number of bases trimmed from upstream end
				trimdn,   // out: number of bases trimmed from downstream end
				refl,     // out: ref pos of upper LHS of parallelogram
				refr,     // out: ref pos of lower RHS of parallelogram
				st_,      // out: legal starting columns stored here
				en_);     // out: legal ending columns stored here
			if(!found) {
				continue;
			}
			assert_eq(width, st_.size());
			assert_eq(st_.size(), en_.size());
			res_.reset();
			assert(res_.empty());
			assert_neq(0xffffffff, tidx);
			// Given the boundaries defined by refl and refr, initilize the
			// SwAligner with the dynamic programming problem that aligns the
			// read to this reference stretch.
			swa.init(
				rd,        // read to align
				0,         // off of first char in 'rd' to consider
				rdlen,     // off of last char (excl) in 'rd' to consider
				fw,        // whether to align forward or revcomp read
				color,     // colorspace?
				tidx,      // reference aligned against
				refl,      // off of first character in ref to consider
				refr+1,    // off of last char (excl) in ref to consider
				ref,       // Reference strings
				tlen,      // length of reference sequence
				width,     // # bands to do (width of parallelogram)
				&st_,      // mask indicating which columns we can start in
				&en_,      // mask indicating which columns we can end in
				sc,        // scoring scheme
				minsc,     // minimum score for valid alignments
				floorsc,   // local-alignment floor score
				nceil);    // max # Ns
			// Now fill the dynamic programming matrix and return true iff
			// there is at least one valid alignment
			found = swa.align(rnd);
			swa.mergeAlignCounters(
				swmSeed.sws,
				swmSeed.swcups,
				swmSeed.swrows,
				swmSeed.swskiprows,
				swmSeed.swsucc,
				swmSeed.swfail);
			swa.resetAlignCounters();
			if(!found) {
				continue; // Look for more anchor alignments
			}
			// For each anchor alignment we pull out of the dynamic programming
			// problem...
			while(true) {
				res_.reset();
				assert(res_.empty());
				if(swa.done()) {
					break;
				}
				swa.nextAlignment(res_, rnd);
				swa.mergeBacktraceCounters(swmSeed.swbts);
				swa.resetBacktraceCounters();
				found = !res_.empty();
				if(!found) {
					// Could not extend the seed hit into a full alignment for
					// the anchor mate
					break;
				}

				// User specified that alignments overhanging ends of reference
				// should be excluded...
				assert(gReportOverhangs || res_.alres.within(tidx, 0, fw, tlen));
				// Is this alignment redundant with one we've seen previously?
				if(redAnchor_.overlap(res_.alres)) {
					// Redundant with an alignment we found already
					continue;
				}
				redAnchor_.add(res_.alres);
				// Annotate the AlnRes object with some key parameters
				// that were used to obtain the alignment.
				res_.alres.setParams(
					seedmms,   // # mismatches allowed in seed
					seedlen,   // length of seed
					seedival,  // interval between seeds
					minsc,     // minimum score for valid alignment
					floorsc);  // local-alignment floor score
				
				bool foundMate = false;
				TRefOff off = res_.alres.refoff();
				if( msink->state().doneWithMate(!anchor1) &&
				   !msink->state().doneWithMate( anchor1))
				{
					// We're done with the opposite mate but not with the
					// anchor mate; don't try to mate up the anchor.
					swMateImmediately = false;
				}
				if(found && swMateImmediately) {
					assert(!msink->state().doneWithMate(!anchor1));
					bool oleft = false, ofw = false;
					int64_t oll = 0, olr = 0, orl = 0, orr = 0;
					assert(!msink->state().done());
					if(!msink->state().doneConcordant()) {
						foundMate = pepol.otherMate(
							anchor1,             // anchor mate is mate #1?
							fw,                  // anchor aligned to Watson?
							off,                 // offset of anchor mate
							orows + oreadGaps,   // max # columns spanned by alignment
							tlen,                // reference length
							anchor1 ? rd.length()  : ord.length(), // mate #1 length
							anchor1 ? ord.length() : rd.length(),  // mate #2 length
							oleft,               // out: look left for opposite mate?
							oll,
							olr,
							orl,
							orr,
							ofw);
					} else {
						// We're no longer interested in finding additional
						// concordant paired-end alignments so we just report this
						// mate's alignment as an unpaired alignment (below)
					}
					size_t owidth = 0, otrimup = 0, otrimdn = 0;
					int64_t orefl = 0, orefr = 0;
					if(foundMate) {
						foundMate = dpframe.frameFindMate(
							!oleft,      // true iff anchor alignment is to the left
							oll,         // leftmost Watson off for LHS of opp aln
							olr,         // rightmost Watson off for LHS of opp aln
							orl,         // leftmost Watson off for RHS of opp aln
							orr,         // rightmost Watson off for RHS of opp aln
							orows,       // length of opposite mate
							tlen,        // length of reference sequence aligned to
							oreadGaps,   // max # of read gaps in opp mate aln
							orefGaps,    // max # of ref gaps in opp mate aln
							maxhalf,     // max width in either direction
							owidth,      // out: calculated width stored here
							otrimup,     // out: # bases trimmed from upstream end
							otrimdn,     // out: # bases trimmed from downstream end
							orefl,       // out: ref pos of upper LHS of parallelogram
							orefr,       // out: ref pos of lower RHS of parallelogram
							ost_,        // out: legal starting columns stored here
							oen_);       // out: legal ending columns stored here
						assert_eq(orefr - orefl + 1, (int64_t)(owidth + orows - 1));
					}
					if(foundMate) {
						ores_.reset();
						assert(ores_.empty());
						// Given the boundaries defined by refi and reff, initilize
						// the SwAligner with the dynamic programming problem that
						// aligns the read to this reference stretch.
						oswa.init(
							ord,       // read to align
							0,         // off of first char in rd to consider
							ordlen,    // off of last char (excl) in rd to consider
							ofw,       // whether to align forward or revcomp read
							color,     // colorspace?
							tidx,      // reference aligned against
							orefl,     // off of first character in rf to consider
							orefr+1,   // off of last char (excl) in rf to consider
							ref,       // Reference strings
							tlen,      // length of reference sequence
							owidth,    // # bands to do (width of parallelogram)
							&ost_,     // mask of which cols we can start in
							&oen_,     // mask of which cols we can end in
							sc,        // scoring scheme
							ominsc,    // minimum score for valid alignments
							ofloorsc,  // local-alignment floor score
							onceil);   // max # Ns
						// Now fill the dynamic programming matrix and return true
						// iff there is at least one valid alignment
						foundMate = oswa.align(rnd);
						oswa.mergeAlignCounters(
							swmMate.sws,
							swmMate.swcups,
							swmMate.swrows,
							swmMate.swskiprows,
							swmMate.swsucc,
							swmMate.swfail);
						oswa.resetAlignCounters();
					}
					do {
						ores_.reset();
						assert(ores_.empty());
						if(oswa.done()) {
							foundMate = false;
						} else {
							oswa.nextAlignment(ores_, rnd);
							oswa.mergeBacktraceCounters(swmMate.swbts);
							oswa.resetBacktraceCounters();
							foundMate = !ores_.empty();
						}
						if(foundMate) {
							// Redundant with one we've seen previously?
							if(!redAnchor_.overlap(ores_.alres)) {
								redAnchor_.add(ores_.alres);
							}
							assert_eq(ofw, ores_.alres.fw());
							// Annotate the AlnRes object with some key parameters
							// that were used to obtain the alignment.
							ores_.alres.setParams(
								seedmms,    // # mismatches allowed in seed
								seedlen,    // length of seed
								seedival,   // interval between seeds
								ominsc,     // minimum score for valid alignment
								ofloorsc);  // local-alignment floor score
							if(!gReportOverhangs &&
							   !ores_.alres.within(tidx, 0, ofw, tlen))
							{
								foundMate = false;
							}
						}
						TRefId refid;
						TRefOff off1, off2;
						TRefOff fragoff;
						size_t len1, len2, fraglen;
						bool fw1, fw2;
						int pairCl;
						if(foundMate) {
							refid = res_.alres.refid();
							assert_eq(refid, ores_.alres.refid());
							off1 = anchor1 ? off : ores_.alres.refoff();
							off2 = anchor1 ? ores_.alres.refoff() : off;
							len1 = anchor1 ?
								res_.alres.refExtent() : ores_.alres.refExtent();
							len2 = anchor1 ?
								ores_.alres.refExtent() : res_.alres.refExtent();
							fw1  = anchor1 ? res_.alres.fw() : ores_.alres.fw();
							fw2  = anchor1 ? ores_.alres.fw() : res_.alres.fw();
							fragoff = min<TRefOff>(off1, off2);
							fraglen = max<TRefOff>(
								off1 - fragoff + len1,
								off2 - fragoff + len2);
							// Check that final mate alignments are consistent with
							// paired-end fragment constraints
							pairCl = pepol.peClassifyPair(
								off1,
								len1,
								fw1,
								off2,
								len2,
								fw2);
							foundMate = pairCl != PE_ALS_DISCORD;
						}
						if(msink->state().doneConcordant()) {
							foundMate = false;
						}
						if(reportImmediately) {
							if(foundMate) {
								// Report pair to the AlnSinkWrap
								assert(!msink->state().doneConcordant());
								assert(msink != NULL);
								assert(res_.repOk());
								assert(ores_.repOk());
								// Check that alignment accurately reflects the
								// reference characters aligned to
								assert(res_.alres.matchesRef(rd, ref));
								assert(ores_.alres.matchesRef(ord, ref));
								// Report an unpaired alignment
								assert(!msink->maxed());
								assert(!msink->state().done());
								if(msink->report(
									0,
									anchor1 ? &res_.alres : &ores_.alres,
									anchor1 ? &ores_.alres : &res_.alres))
								{
									// Short-circuited because a limit, e.g.
									// -k, -m or -M, was exceeded
									return true;
								}
								if(mixed || discord) {
									// Report alignment for mate #1 as an
									// unpaired alignment.
									if(!msink->state().doneUnpaired(true)) {
										const AlnRes& r1 = anchor1 ?
											res_.alres : ores_.alres;
										if(!redMate1_.overlap(r1)) {
											redMate1_.add(r1);
											if(msink->report(0, &r1, NULL)) {
												return true; // Short-circuited
											}
										}
									}
									// Report alignment for mate #2 as an
									// unpaired alignment.
									if(!msink->state().doneUnpaired(false)) {
										const AlnRes& r2 = anchor1 ?
											ores_.alres : res_.alres;
										if(!redMate2_.overlap(r2)) {
											redMate2_.add(r2);
											if(msink->report(0, NULL, &r2)) {
												return true; // Short-circuited
											}
										}
									}
								}
								if(msink->state().doneWithMate(anchor1)) {
									// We're now done with the mate that we're
									// currently using as our anchor.  We're not
									// with the read overall.
									return false;
								}
							} else if(mixed || discord) {
								// Report unpaired hit for anchor
								assert(msink != NULL);
								assert(res_.repOk());
								// Check that alignment accurately reflects the
								// reference characters aligned to
								assert(res_.alres.matchesRef(rd, ref));
								// Report an unpaired alignment
								assert(!msink->maxed());
								assert(!msink->state().done());
								// Report alignment for mate #1 as an
								// unpaired alignment.
								if(!msink->state().doneUnpaired(anchor1)) {
									const AlnRes& r = res_.alres;
									RedundantAlns& red = anchor1 ? redMate1_ : redMate2_;
									const AlnRes* r1 = anchor1 ? &res_.alres : NULL;
									const AlnRes* r2 = anchor1 ? NULL : &res_.alres;
									if(!red.overlap(r)) {
										red.add(r);
										if(msink->report(0, r1, r2)) {
											return true; // Short-circuited
										}
									}
								}
								if(msink->state().doneWithMate(anchor1)) {
									// Done with mate, but not read overall
									return false;
								}
							}
						}
					} while(!ores_.empty());
				} // if(found && swMateImmediately)
				else if(found) {
					assert(!msink->state().doneWithMate(anchor1));
					// We found an anchor alignment but did not attempt to find
					// an alignment for the opposite mate (probably because
					// we're done with it)
					if(reportImmediately && (mixed || discord)) {
						// Report unpaired hit for anchor
						assert(msink != NULL);
						assert(res_.repOk());
						// Check that alignment accurately reflects the
						// reference characters aligned to
						assert(res_.alres.matchesRef(rd, ref));
						// Report an unpaired alignment
						assert(!msink->maxed());
						assert(!msink->state().done());
						// Report alignment for mate #1 as an
						// unpaired alignment.
						if(!msink->state().doneUnpaired(anchor1)) {
							const AlnRes& r = res_.alres;
							RedundantAlns& red = anchor1 ? redMate1_ : redMate2_;
							const AlnRes* r1 = anchor1 ? &res_.alres : NULL;
							const AlnRes* r2 = anchor1 ? NULL : &res_.alres;
							if(!red.overlap(r)) {
								red.add(r);
								if(msink->report(0, r1, r2)) {
									return true; // Short-circuited
								}
							}
						}
						if(msink->state().doneWithMate(anchor1)) {
							// Done with mate, but not read overall
							return false;
						}
					}
				}
			} // while(true)
			
			// At this point we know that we aren't bailing, and will continue to resolve seed hits.  

		} // while(!gw.done())
	
	} // for(size_t i = 0; i < poss; i++)
	return false;
}
