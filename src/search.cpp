/*
 * Stormphrax, a UCI chess engine
 * Copyright (C) 2023 Ciekce
 *
 * Stormphrax is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Stormphrax is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Stormphrax. If not, see <https://www.gnu.org/licenses/>.
 */

#include "search.h"

#include <iostream>
#include <algorithm>
#include <cmath>
#include <cassert>

#include "uci.h"
#include "movegen.h"
#include "limit/trivial.h"
#include "opts.h"
#include "3rdparty/fathom/tbprobe.h"

namespace stormphrax::search
{
	using namespace stormphrax::tunable;

	namespace
	{
		constexpr f64 MinReportDelay = 1.0;

		// values from viridithas
		//TODO tune for stormphrax
		constexpr f64 LmrBase = 0.77;
		constexpr f64 LmrDivisor = 2.36;

		[[nodiscard]] auto generateLmrTable()
		{
			std::array<std::array<i32, 256>, 256> dst{};

			// neither can be 0
			for (i32 depth = 1; depth < 256; ++depth)
			{
				for (i32 moves = 1; moves < 256; ++moves)
				{
					dst[depth][moves] = static_cast<i32>(LmrBase
						+ std::log(static_cast<f64>(depth)) * std::log(static_cast<f64>(moves)) / LmrDivisor);
				}
			}

			return dst;
		}

		const auto LmrTable = generateLmrTable();

		inline auto drawScore(usize nodes)
		{
			return 2 - static_cast<Score>(nodes % 4);
		}
	}

	Searcher::Searcher(std::optional<usize> hashSize)
		: m_table{hashSize ? *hashSize : DefaultHashSize}
	{
		auto &thread = m_threads.emplace_back();

		thread.id = m_nextThreadId++;
		thread.thread = std::thread{[this, &thread]
		{
			run(thread);
		}};
	}

	auto Searcher::newGame() -> void
	{
		m_table.clear();

		for (auto &thread : m_threads)
		{
			std::fill(thread.stack.begin(), thread.stack.end(), SearchStackEntry{});
			thread.history.clear();
		}
	}

	auto Searcher::startSearch(const Position &pos, i32 maxDepth,
		std::unique_ptr<limit::ISearchLimiter> limiter) -> void
	{
		if (!m_limiter && !limiter)
		{
			std::cerr << "missing limiter" << std::endl;
			return;
		}

		const auto &boards = pos.boards();

		// probe syzygy tb for a move
		if (g_opts.syzygyEnabled
			&& boards.occupancy().popcount() <= std::min(g_opts.syzygyProbeLimit, static_cast<i32>(TB_LARGEST)))
		{
			const auto epSq = pos.enPassant();
			const auto result = tb_probe_root(
				boards.whiteOccupancy(),
				boards.blackOccupancy(),
				boards.kings(),
				boards.queens(),
				boards.rooks(),
				boards.bishops(),
				boards.knights(),
				boards.pawns(),
				pos.halfmove(), 0,
				epSq == Square::None ? 0 : static_cast<i32>(epSq),
				pos.toMove() == Color::White,
				nullptr
			);

			if (result != TB_RESULT_FAILED)
			{
				static constexpr auto PromoPieces = std::array {
					PieceType::None,
					PieceType::Queen,
					PieceType::Rook,
					PieceType::Bishop,
					PieceType::Knight
				};

				const auto wdl = TB_GET_WDL(result);

				const auto score = wdl == TB_WIN ? ScoreTbWin
					: wdl == TB_LOSS ? -ScoreTbWin
					: 0; // draw

				const auto src = static_cast<Square>(TB_GET_FROM(result));
				const auto dst = static_cast<Square>(TB_GET_TO  (result));

				const auto promo = PromoPieces[TB_GET_PROMOTES(result)];
				const bool ep = TB_GET_EP(result);

				const auto move = ep ? Move::enPassant(src, dst)
					: promo != PieceType::None ? Move::promotion(src, dst, promo)
					: Move::standard(src, dst);

				auto &thread = m_threads[0];

				thread.rootPv.moves[0] = move;
				thread.rootPv.length = 1;

				report(thread, 1, 0.0, score, -ScoreMax, ScoreMax, true);
				std::cout << "bestmove " << uci::moveToString(move) << std::endl;

				return;
			}
		}

		m_resetBarrier.arriveAndWait();

		if (limiter)
			m_limiter = std::move(limiter);

		const auto contempt = g_opts.contempt;

		m_contempt[static_cast<i32>(pos.  toMove())] =  contempt;
		m_contempt[static_cast<i32>(pos.opponent())] = -contempt;

		for (auto &thread : m_threads)
		{
			thread.maxDepth = maxDepth;
			thread.search = SearchData{};
			thread.pos = pos;

			thread.nnueState.reset(thread.pos.boards());
		}

		m_stop.store(false, std::memory_order::seq_cst);
		m_runningThreads.store(static_cast<i32>(m_threads.size()));

		m_searching.store(true, std::memory_order::relaxed);

		m_idleBarrier.arriveAndWait();
	}

	auto Searcher::stop() -> void
	{
		m_stop.store(true, std::memory_order::relaxed);

		// safe, always runs from uci thread
		if (m_runningThreads.load() > 0)
		{
			std::unique_lock lock{m_stopMutex};
			m_stopSignal.wait(lock, [this]
			{
				return m_runningThreads.load(std::memory_order::seq_cst) == 0;
			});
		}
	}

	auto Searcher::runDatagenSearch(ThreadData &thread) -> std::pair<Score, Score>
	{
		m_stop.store(false, std::memory_order::seq_cst);

		const auto score = searchRoot(thread, false);

		m_table.age();

		const auto whitePovScore = thread.pos.toMove() == Color::Black ? -score : score;
		return {whitePovScore, wdl::normalizeScore(whitePovScore)};
	}

	auto Searcher::runBench(BenchData &data, const Position &pos, i32 depth) -> void
	{
		m_limiter = std::make_unique<limit::InfiniteLimiter>();
		m_contempt = {};

		// this struct is a small boulder the size of a large boulder
		// and overflows the stack if not on the heap
		auto thread = std::make_unique<ThreadData>();

		thread->pos = pos;
		thread->maxDepth = depth;

		thread->nnueState.reset(thread->pos.boards());

		m_stop.store(false, std::memory_order::seq_cst);

		const auto start = util::g_timer.time();

		searchRoot(*thread, false);

		const auto time = util::g_timer.time() - start;

		data.search = thread->search;
		data.time = time;
	}

	auto Searcher::setThreads(u32 threads) -> void
	{
		if (threads != m_threads.size())
		{
			stopThreads();

			m_quit.store(false, std::memory_order::seq_cst);

			m_threads.clear();
			m_threads.shrink_to_fit();
			m_threads.reserve(threads);

			m_nextThreadId = 0;

			m_resetBarrier.reset(threads + 1);
			m_idleBarrier.reset(threads + 1);

			m_searchEndBarrier.reset(threads);

			for (i32 i = 0; i < threads; ++i)
			{
				auto &thread = m_threads.emplace_back();

				thread.id = m_nextThreadId++;
				thread.thread = std::thread{[this, &thread]
				{
					run(thread);
				}};
			}
		}
	}

	auto Searcher::stopThreads() -> void
	{
		m_quit.store(true, std::memory_order::release);
		m_resetBarrier.arriveAndWait();
		m_idleBarrier.arriveAndWait();

		for (auto &thread : m_threads)
		{
			thread.thread.join();
		}
	}

	auto Searcher::run(ThreadData &thread) -> void
	{
		while (true)
		{
			m_resetBarrier.arriveAndWait();
			m_idleBarrier.arriveAndWait();

			if (m_quit.load(std::memory_order::acquire))
				return;

			searchRoot(thread, true);
		}
	}

	auto Searcher::searchRoot(ThreadData &thread, bool mainSearchThread) -> Score
	{
		auto &searchData = thread.search;

		const bool reportAndUpdate = mainSearchThread && thread.isMainThread();

		thread.rootPv.moves[0] = NullMove;
		thread.rootPv.length = 0;

		auto score = -ScoreMax;
		Move best{};

		const auto startTime = reportAndUpdate ? util::g_timer.time() : 0.0;
		const auto startDepth = 1 + static_cast<i32>(thread.id) % 16;

		i32 depthCompleted{};

		bool hitSoftTimeout = false;

		for (i32 depth = startDepth;
			depth <= thread.maxDepth
				&& !(hitSoftTimeout = shouldStop(searchData, true));
			++depth)
		{
			searchData.depth = depth;
			searchData.seldepth = 0;

			const auto prevBest = best;

			bool reportThisIter = reportAndUpdate;

			if (depth < minAspDepth())
			{
				const auto newScore = search(thread, thread.rootPv, depth, 0, 0, -ScoreMax, ScoreMax, false);

				depthCompleted = depth;

				if (depth > 1 && m_stop.load(std::memory_order::relaxed) || thread.rootPv.length == 0)
					break;

				score = newScore;
				best = thread.rootPv.moves[0];
			}
			else
			{
				auto aspDepth = depth;

				auto delta = initialAspWindow();

				auto alpha = std::max(score - delta, -ScoreMax);
				auto beta  = std::min(score + delta,  ScoreMax);

				while (!shouldStop(searchData, false))
				{
					aspDepth = std::max(aspDepth, depth - maxAspReduction());

					const auto newScore = search(thread, thread.rootPv, aspDepth, 0, 0, alpha, beta, false);

					const bool stop = m_stop.load(std::memory_order::relaxed);
					if (stop || thread.rootPv.length == 0)
					{
						reportThisIter &= !stop;
						break;
					}

					score = newScore;

					if (reportAndUpdate && (score <= alpha || score >= beta))
					{
						const auto time = util::g_timer.time() - startTime;
						if (time > MinReportDelay)
							report(thread, thread.search.depth, time, score, alpha, beta);
					}

					delta += delta / 2;

					if (delta > maxAspWindow())
						delta = ScoreMax;

					if (score >= beta)
					{
						beta = std::min(beta + delta, ScoreMax);
						--aspDepth;
					}
					else if (score <= alpha)
					{
						beta = (alpha + beta) / 2;
						alpha = std::max(alpha - delta, -ScoreMax);
						aspDepth = depth;
					}
					else
					{
						best = thread.rootPv.moves[0];
						depthCompleted = depth;
						break;
					}
				}
			}

			if (reportAndUpdate)
				m_limiter->update(thread.search, best, thread.search.nodes);

			if (reportThisIter && depth < thread.maxDepth)
			{
				if (!best)
					best = thread.rootPv.moves[0];

				if (best)
					report(thread, searchData.depth, util::g_timer.time() - startTime, score, -ScoreMax, ScoreMax);
				else
				{
					std::cout << "info string no legal moves" << std::endl;
					break;
				}
			}
		}

		if (reportAndUpdate)
		{
			if (mainSearchThread)
				m_searchMutex.lock();

			if (best)
			{
				if (!hitSoftTimeout || !m_limiter->stopped())
					report(thread, depthCompleted, util::g_timer.time() - startTime, score, -ScoreMax, ScoreMax);
				std::cout << "bestmove " << uci::moveToString(best) << std::endl;
			}
			else std::cout << "info string no legal moves" << std::endl;
		}

		if (mainSearchThread)
		{
			--m_runningThreads;
			m_stopSignal.notify_all();

			if (reportAndUpdate)
			{
				m_stop.store(true, std::memory_order::seq_cst);
				m_searchEndBarrier.arriveAndWait();

				m_searching.store(false, std::memory_order::relaxed);

				m_table.age();

				m_searchMutex.unlock();
			}
			else m_searchEndBarrier.arriveAndWait();
		}

		return score;
	}

	auto Searcher::search(ThreadData &thread, PvList &pv, i32 depth,
		i32 ply, u32 moveStackIdx, Score alpha, Score beta, bool cutnode) -> Score
	{
		assert(alpha >= -ScoreMax);
		assert(beta  <=  ScoreMax);
		assert(alpha <   beta);
		assert(depth >= 0 && depth <= MaxDepth);
		assert(ply   >= 0 && ply   <= MaxDepth);

		if (depth > 1 && shouldStop(thread.search, false))
			return beta;

		auto &pos = thread.pos;
		const auto &boards = pos.boards();

		if (ply >= MaxDepth)
			return eval::staticEval(pos, thread.nnueState, m_contempt);

		const bool inCheck = pos.isCheck();

		// Check extension
		// If in check, extend. This helps resolve
		// perpetuals and other long checking sequences.
		if (inCheck && depth < MaxDepth)
			++depth;

		// Drop into quiescence search in leaf nodes
		if (depth <= 0)
			return qsearch(thread, ply, moveStackIdx, alpha, beta);

		const auto us = pos.toMove();
		const auto them = oppColor(us);

		const bool root = ply == 0;
		const bool pvNode = root || beta - alpha > 1;

		auto &stack = thread.stack[ply];
		auto &moveStack = thread.moveStack[moveStackIdx];

		if (ply > thread.search.seldepth)
			thread.search.seldepth = ply;

		// Mate distance pruning
		// If we've found a mate score already, and this
		// node has no chance of improving it, don't search it.
		// This gains a fractional amount of elo, but massively
		// (and soundly) reduces the size of the search tree when
		// a forced mate is found
		if (!pvNode)
		{
			const auto mdAlpha = std::max(alpha, -ScoreMate + ply);
			const auto mdBeta = std::min(beta, ScoreMate - ply - 1);

			if (mdAlpha >= mdBeta)
				return mdAlpha;
		}

		ProbedTTableEntry ttEntry{};
		auto ttMove = NullMove;

		if (!stack.excluded)
		{
			if (m_table.probe(ttEntry, pos.key(), depth, ply, alpha, beta) && !pvNode)
				return ttEntry.score;
			else if (ttEntry.move && pos.isPseudolegal(ttEntry.move))
				ttMove = ttEntry.move;

			// Internal iterative reduction (IIR)
			// If we do not have a TT move in this position, then
			//   a) searching this node is likely to take a long time, and
			//   b) there's a good chance that this node sucks anyway.
			// Just reduce the depth and come back later
			if (!inCheck
				&& depth >= minIirDepth()
				&& !stack.excluded
				&& !ttMove
				&& (pvNode || cutnode))
				--depth;
		}

		const bool ttHit = ttEntry.type != EntryType::None;

		const auto pieceCount = boards.occupancy().popcount();

		auto syzygyMin = -ScoreMate;
		auto syzygyMax =  ScoreMate;

		const auto syzygyPieceLimit = std::min(g_opts.syzygyProbeLimit, static_cast<i32>(TB_LARGEST));

		// Probe the Syzygy tablebases for a WDL result
		// if there are few enough pieces left on the board
		if (!root
			&& !stack.excluded
			&& g_opts.syzygyEnabled
			&& pieceCount <= syzygyPieceLimit
			&& (pieceCount < syzygyPieceLimit || depth >= g_opts.syzygyProbeDepth)
			&& pos.halfmove() == 0
			&& pos.castlingRooks() == CastlingRooks{})
		{
			const auto epSq = pos.enPassant();
			const auto wdl = tb_probe_wdl(
				boards.whiteOccupancy(),
				boards.blackOccupancy(),
				boards.kings(),
				boards.queens(),
				boards.rooks(),
				boards.bishops(),
				boards.knights(),
				boards.pawns(),
				0, 0,
				epSq == Square::None ? 0 : static_cast<i32>(epSq),
				us == Color::White
			);

			if (wdl != TB_RESULT_FAILED)
			{
				++thread.search.tbhits;

				Score tbScore{};
				EntryType tbEntryType{};

				if (wdl == TB_WIN)
				{
					tbScore = ScoreTbWin - ply;
					tbEntryType = EntryType::Beta;
				}
				else if (wdl == TB_LOSS)
				{
					tbScore = -ScoreTbWin + ply;
					tbEntryType = EntryType::Alpha;
				}
				else // draw
				{
					tbScore = drawScore(thread.search.nodes);
					tbEntryType = EntryType::Exact;
				}

				// Cut off with the same conditions as TT cutoffs
				if (tbEntryType == EntryType::Exact
					|| tbEntryType == EntryType::Alpha && tbScore <= alpha
					|| tbEntryType == EntryType::Beta && tbScore >= beta)
				{
					// Throw the TB score into the TT
					m_table.put(pos.key(), tbScore, NullMove, depth, ply, tbEntryType);
					return tbScore;
				}

				if (pvNode)
				{
					if (tbEntryType == EntryType::Alpha)
						syzygyMax = tbScore;
					else if (tbEntryType == EntryType::Beta)
					{
						if (tbScore > alpha)
							alpha = tbScore;
						syzygyMin = tbScore;
					}
				}
			}
		}

		// we already have the static eval in a singularity search
		if (!stack.excluded)
		{
			if (!root && !pos.lastMove())
				stack.eval = -thread.stack[ply - 1].eval;
			else stack.eval = inCheck ? 0 : eval::staticEval(pos, thread.nnueState, m_contempt);
		}

		thread.prevMoves[ply] = {};

		const bool improving = !inCheck && ply > 1 && stack.eval > thread.stack[ply - 2].eval;

		if (!pvNode && !inCheck && !stack.excluded)
		{
			// Reverse futility pruning (RFP)
			// If static eval is above beta by some depth-dependent
			// margin, assume that this is a cutnode and just prune it
			if (depth <= maxRfpDepth()
				&& stack.eval >= beta + rfpMargin() * depth / (improving ? 2 : 1))
				return stack.eval;

			// Nullmove pruning (NMP)
			// If static eval is above beta, and zugzwang is unlikely
			// (i.e. the side to move has material other than pawns),
			// give the opponent a free move and do a reduced-depth search.
			// If our score is *still* above beta even after the opponent
			// gets two turns in a row, assume that this is a cutnode and prune.
			// Don't bother trying if the TT suggests that NMP will fail - the TT
			// entry, if it exists, must not be a fail-low entry with a score below beta
			if (depth >= minNmpDepth()
				&& stack.eval >= beta
				&& !(ttHit && ttEntry.type == EntryType::Alpha && ttEntry.score < beta)
				&& pos.lastMove()
				&& !boards.nonPk(us).empty())
			{
				const auto R = std::min(depth,
					nmpReductionBase()
						+ depth / nmpReductionDepthScale()
						+ std::min((stack.eval - beta) / nmpReductionEvalScale(), maxNmpEvalReduction()));

				const auto guard = pos.applyMove<false>(NullMove, nullptr, &m_table);
				const auto score = -search(thread, stack.pv, depth - R,
					ply + 1, moveStackIdx + 1, -beta, -beta + 1, !cutnode);

				if (score >= beta)
					return score > ScoreWin ? beta : score;
			}
		}

		moveStack.quietsTried.clear();
		moveStack.noisiesTried.clear();

		if (ply > 0)
			stack.doubleExtensions = thread.stack[ply - 1].doubleExtensions;

		const i32 minLmrMoves = pvNode ? 3 : 2;

		const auto threats = pos.threats();

		auto bestMove = NullMove;
		auto bestScore = -ScoreMax;

		auto entryType = EntryType::Alpha;

		MoveGenerator generator{pos, stack.killer, moveStack.movegenData,
			ttMove, ply, thread.prevMoves, &thread.history};

		u32 legalMoves = 0;

		while (const auto moveAndHistory = generator.next())
		{
			const auto [move, history] = moveAndHistory;

			if (move == stack.excluded)
				continue;

			const auto prevNodes = thread.search.nodes;

			const bool quietOrLosing = generator.stage() >= MovegenStage::Quiet;
			const auto [noisy, captured] = pos.noisyCapturedPiece(move);

			const auto baseLmr = LmrTable[depth][legalMoves + 1];

			if (!root
				&& quietOrLosing
				&& bestScore > -ScoreWin
				// skip moveloop pruning in PV nodes during datagen
				&& (!pvNode || !thread.datagen))
			{
				if (!inCheck)
				{
					const auto lmrHistory = history / historyLmrDivisor();
					const auto lmrDepth = std::clamp(depth - baseLmr + lmrHistory, 0, depth);

					// Late move pruning (LMP)
					// At low enough depths, only search a certain depth-dependent
					// number of moves. Sane implementations just use depth here
					// instead of LMR depth, but SP is weird and loses elo when I try
					if (!pvNode
						&& depth <= maxLmpDepth()
						&& legalMoves >= lmpMinMovesBase() + lmrDepth * lmrDepth / (improving ? 1 : 2))
						break;

					// Futility pruning (FP)
					// At this point, alpha is so far above static eval that it is
					// unlikely that we can improve it with this move, so just give up
					if (depth <= maxFpDepth()
						&& alpha < ScoreWin
						&& stack.eval + fpMargin() + lmrDepth * fpScale() <= alpha)
						break;
				}

				// SEE pruning
				// If this move loses a depth-dependent amount of material, just don't bother searching it
				if (depth <= maxSeePruningDepth()
					&& !see::see(pos, move, depth * (noisy ? noisySeeThreshold() : quietSeeThreshold())))
					continue;
			}

			const auto movingPiece = boards.pieceAt(move.src());
			assert(movingPiece != Piece::None);

			const auto guard = pos.applyMove(move, &thread.nnueState, &m_table);

			if (!guard)
				continue;

			if (pvNode)
				stack.pv.length = 0;

			++thread.search.nodes;
			++legalMoves;

			i32 extension{};

			// Singular extensions (SE)
			// If the TT entry meets certain requirements (adequate depth, and not a fail-low entry),
			// then do a reduced depth search with the TT move excluded from being searched.
			// If the result of that search plus some depth-dependent margin does not beat the TT
			// score, assume that the TT move is "singular" (the only good move) and extend it
			if (!root
				&& depth >= minSingularityDepth()
				&& move == ttMove
				&& !stack.excluded
				&& ttEntry.depth >= depth - singularityDepthMargin()
				&& ttEntry.type != EntryType::Alpha)
			{
				const auto sBeta = std::max(-ScoreMate, ttEntry.score - singularityDepthScale() * depth);
				const auto sDepth = (depth - 1) / 2;

				stack.excluded = move;
				pos.popMove(&thread.nnueState);

				const auto score = search(thread, stack.pv, sDepth, ply, moveStackIdx + 1, sBeta - 1, sBeta, cutnode);

				stack.excluded = NullMove;
				pos.applyMoveUnchecked(move, &thread.nnueState, &m_table);

				if (score < sBeta)
				{
					if (!pvNode
						&& score < sBeta - doubleExtensionMargin()
						&& stack.doubleExtensions <= doubleExtensionLimit())
					{
						// The returned score is *far* below the TT score - the TT move is
						// probably much better than the other moves, extend it by 2 plies instead.
						// Limit the amount this can happen in a particular branch to avoid explosions
						extension = 2;
						++stack.doubleExtensions;
					}
					else extension = 1;
				}
				// Multicut
				// The TT move is not singular, and in fact the reduced-depth search also returned
				// a score that was at least beta - there are probably multiple moves in this position
				// that will beat beta, so just save the time searching and do a cutoff now
				else if (sBeta >= beta)
					return sBeta;
				else if (ttEntry.score >= beta)
					extension = -2;
			}

			thread.prevMoves[ply] = {movingPiece, move.src(), moveActualDst(move)};

			Score score{};

			if (pos.isDrawn(pvNode))
				score = drawScore(thread.search.nodes);
			else
			{
				const auto newDepth = depth - 1 + extension;

				if (pvNode && legalMoves == 1)
					score = -search(thread, stack.pv, newDepth, ply + 1, moveStackIdx + 1, -beta, -alpha, false);
				else
				{
					i32 reduction{};

					// Late move reductions (LMR)
					// Moves ordered later in the movelist are more likely to be bad,
					// so search them to lower depth according to various heuristics
					if (depth >= minLmrDepth()
						&& legalMoves >= minLmrMoves
						&& generator.stage() >= MovegenStage::Quiet)
					{
						auto lmr = baseLmr;

						// reduce more in non-PV nodes
						lmr += !pvNode;

						// reduce less if this move gives check
						lmr -= pos.isCheck();

						// reduce moves with good history scores less and vice versa
						lmr -= history / historyLmrDivisor();

						reduction = std::clamp(lmr, 0, depth - 2);
					}

					score = -search(thread, stack.pv, newDepth - reduction,
						ply + 1, moveStackIdx + 1, -alpha - 1, -alpha, true);

					if (score > alpha && reduction > 0)
						score = -search(thread, stack.pv, newDepth,
							ply + 1, moveStackIdx + 1, -alpha - 1, -alpha, !cutnode);

					if (score > alpha && score < beta)
						score = -search(thread, stack.pv, newDepth, ply + 1, moveStackIdx + 1, -beta, -alpha, false);
				}
			}

			if (root && thread.isMainThread())
				m_limiter->updateMoveNodes(move, thread.search.nodes - prevNodes);

			if (score > bestScore)
			{
				bestMove = move;
				bestScore = score;

				if (pvNode)
				{
					pv.moves[0] = move;

					assert(stack.pv.length + 1 <= MaxDepth);

					std::copy(stack.pv.moves.begin(), stack.pv.moves.begin() + stack.pv.length, pv.moves.begin() + 1);
					pv.length = stack.pv.length + 1;

					assert(pv.length == 1 || pv.moves[0] != pv.moves[1]);
				}

				if (score > alpha)
				{
					if (score >= beta)
					{
						// Update history on fail-highs
						const auto bonus = historyAdjustment(depth);
						const auto penalty = -bonus;

						const auto currMove = thread.prevMoves[ply];

						// If the fail-high move is a quiet move or losing
						// capture, set it as the killer for this ply and the
						// countermove for the opponent's previous move
						if (quietOrLosing)
						{
							stack.killer = move;
							thread.history.updateCountermove(ply, thread.prevMoves, move);
						}

						if (noisy)
							thread.history.updateNoisyScore(currMove, threats, captured, bonus);
						else
						{
							thread.history.updateQuietScore(currMove, threats, ply, thread.prevMoves, bonus);

							// Penalise quiet moves that did not fail high if the fail-high move is quiet
							for (const auto prevQuiet : moveStack.quietsTried)
							{
								thread.history.updateQuietScore(prevQuiet, threats, ply, thread.prevMoves, penalty);
							}
						}

						// Always penalise noisy moves that did not fail high
						for (const auto [prevNoisy, prevCaptured] : moveStack.noisiesTried)
						{
							thread.history.updateNoisyScore(prevNoisy, threats, prevCaptured, penalty);
						}

						entryType = EntryType::Beta;
						break;
					}

					alpha = score;
					entryType = EntryType::Exact;
				}
			}

			if (noisy)
				moveStack.noisiesTried.push({thread.prevMoves[ply], captured});
			else moveStack.quietsTried.push(thread.prevMoves[ply]);
		}

		if (legalMoves == 0)
		{
			if (stack.excluded)
				return alpha;
			return inCheck ? (-ScoreMate + ply) : 0;
		}

		bestScore = std::clamp(bestScore, syzygyMin, syzygyMax);

		// increase depth for tt if in check
		// https://chess.swehosting.se/test/1456/
		if (!stack.excluded)
			m_table.put(pos.key(), bestScore, bestMove, inCheck ? depth + 1 : depth, ply, entryType);

		return bestScore;
	}

	auto Searcher::qsearch(ThreadData &thread, i32 ply, u32 moveStackIdx, Score alpha, Score beta) -> Score
	{
		if (shouldStop(thread.search, false))
			return beta;

		auto &pos = thread.pos;

		const auto staticEval = pos.isCheck()
			? -ScoreMate
			: eval::staticEval(pos, thread.nnueState, m_contempt);

		if (staticEval > alpha)
		{
			if (staticEval >= beta)
				return staticEval;

			alpha = staticEval;
		}

		if (ply >= MaxDepth)
			return staticEval;

		const auto us = pos.toMove();

		if (ply > thread.search.seldepth)
			thread.search.seldepth = ply;

		ProbedTTableEntry entry{};
		auto ttMove = NullMove;

		if (m_table.probe(entry, pos.key(), 0, ply, alpha, beta))
			return entry.score;
		else if (entry.move && pos.isPseudolegal(entry.move))
			ttMove = entry.move;

		auto best = NullMove;
		auto bestScore = staticEval;

		auto entryType = EntryType::Alpha;

		QMoveGenerator generator{pos, NullMove, thread.moveStack[moveStackIdx].movegenData, ttMove};

		while (const auto move = generator.next())
		{
			const auto guard = pos.applyMove(move.move, &thread.nnueState, &m_table);

			if (!guard)
				continue;

			++thread.search.nodes;

			const auto score = pos.isDrawn(false)
				? drawScore(thread.search.nodes)
				: -qsearch(thread, ply + 1, moveStackIdx + 1, -beta, -alpha);

			if (score > bestScore)
			{
				bestScore = score;

				if (score > alpha)
				{
					if (score >= beta)
					{
						entryType = EntryType::Beta;
						break;
					}

					alpha = score;
					entryType = EntryType::Exact;
				}
			}
		}

		m_table.put(pos.key(), bestScore, best, 0, ply, entryType);

		return bestScore;
	}

	auto Searcher::report(const ThreadData &mainThread, i32 depth,
		f64 time, Score score, Score alpha, Score beta, bool tbRoot) -> void
	{
		usize nodes = 0;

		// in tb positions at root we have searched 0 nodes
		if (!tbRoot)
		{
			// technically a potential race but it doesn't matter
			for (const auto &thread : m_threads)
			{
				nodes += thread.search.nodes;
			}
		}

		const auto ms = tbRoot ? 0 : static_cast<usize>(time * 1000.0);
		const auto nps = tbRoot ? 0 : static_cast<usize>(static_cast<f64>(nodes) / time);

		std::cout << "info depth " << depth << " seldepth " << mainThread.search.seldepth
			<< " time " << ms << " nodes " << nodes << " nps " << nps << " score ";

		score = std::clamp(score, alpha, beta);

		// mates
		if (std::abs(score) > ScoreTbWin)
		{
			if (score > 0)
				std::cout << "mate " << ((ScoreMate - score + 1) / 2);
			else std::cout << "mate " << (-(ScoreMate + score) / 2);
		}
		else
		{
			// adjust score to 100cp == 50% win probability
			const auto normScore = wdl::normalizeScore(score);
			std::cout << "cp " << normScore;
		}

		if (score == alpha)
			std::cout << " upperbound";
		else if (score == beta)
			std::cout << " lowerbound";

		// wdl display
		if (g_opts.showWdl)
		{
			if (score > ScoreWin)
				std::cout << " wdl 1000 0 0";
			else if (score < -ScoreWin)
				std::cout << " wdl 0 0 1000";
				// tablebase draws at the root
			else if (tbRoot)
				std::cout << " wdl 0 1000 0";
			else
			{
				const auto plyFromStartpos = mainThread.pos.fullmove() * 2
					- (mainThread.pos.toMove() == Color::White ? 1 : 0) - 1;

				const auto [wdlWin, wdlLoss] = wdl::winRateModel(score, plyFromStartpos);
				const auto wdlDraw = 1000 - wdlWin - wdlLoss;

				std::cout << " wdl " << wdlWin << " " << wdlDraw << " " << wdlLoss;
			}
		}

		std::cout << " hashfull " << m_table.full();

		if (g_opts.syzygyEnabled)
		{
			if (tbRoot)
				std::cout << " tbhits 1";
			else
			{
				usize tbhits = 0;

				// technically a potential race but it doesn't matter
				for (const auto &thread : m_threads)
				{
					tbhits += thread.search.tbhits;
				}

				std::cout << " tbhits " << tbhits;
			}
		}

		std::cout << " pv";

		for (u32 i = 0; i < mainThread.rootPv.length; ++i)
		{
			std::cout << ' ' << uci::moveToString(mainThread.rootPv.moves[i]);
		}

		std::cout << std::endl;
	}
}
