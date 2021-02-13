/*
  Honey, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2021 The Stockfish developers (see AUTHORS file)

  Honey is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Honey is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>   // For std::memset
#include <iostream>
#include <sstream>

#include <fstream>
#include <unistd.h> //for sleep //MichaelB7
#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"
#include "polybook.h" // Cerebellum

namespace Search {

  LimitsType Limits;
  //from Shashin
  bool tal,capablanca,petrosian;

  bool adaptive;
  bool ctempt;
  bool defensive;
  bool dpa;
  bool profound=false;
  int benchKnps;
  int dct;

  int defensive_v = 0;
  int mpv;
  int proValue;
  int profound_v;
}

namespace Tablebases {

  int Cardinality;
  bool RootInTB;
  bool UseRule50;
  Depth ProbeDepth;
}

namespace TB = Tablebases;

using std::string;
using Eval::evaluate;
using namespace Search;

namespace {

  // Different node types, used as a template parameter
  enum NodeType { NonPV, PV };
#ifndef Noir
  constexpr uint64_t TtHitAverageWindow     = 4096;
  constexpr uint64_t TtHitAverageResolution = 1024;
#else
  constexpr uint64_t ttHitAverageWindow     = 4096;
  constexpr uint64_t ttHitAverageResolution = 1024;
#endif
  // Futility margin
  Value futility_margin(Depth d, bool improving) {
    return Value(234 * (d - improving));
  }
  // Reductions lookup table, initialized at startup
  int Reductions[MAX_MOVES]; // [depth or moveNumber]

  Depth reduction(bool i, Depth d, int mn) {
    int r = Reductions[d] * Reductions[mn];
    return (r + 503) / 1024 + (!i && r > 915);
  }

  constexpr int futility_move_count(bool improving, Depth depth) {
    return (3 + depth * depth) / (2 - improving);
  }

  // History and stats update bonus, based on depth
  int stat_bonus(Depth d) {
    return d > 14 ? 29 : 8 * d * d + 224 * d - 215;
  }
#ifndef Noir
  // Add a small random component to draw evaluations to avoid 3-fold blindness
  Value value_draw(Thread* thisThread) {
    return VALUE_DRAW + Value(2 * (thisThread->nodes & 1) - 1);
  }

#endif


  // Skill structure is used to implement strength limit
  struct Skill {
    explicit Skill(int l) : level(l) {}
    bool enabled() const { return level < 40; }
    bool time_to_pick(Depth depth) const { return depth == 1 + level; }
    Move pick_best(size_t multiPV);

    int level;
    Move best = MOVE_NONE;
  };

bool  fide, minOutput, uci_sleep;
bool limitStrength = false;
int  tactical, uci_elo = 0;

  // Breadcrumbs are used to mark nodes as being searched by a given thread
  struct Breadcrumb {
    std::atomic<Thread*> thread;
    std::atomic<Key> key;
  };
  std::array<Breadcrumb, 1024> breadcrumbs;

  // ThreadHolding structure keeps track of which thread left breadcrumbs at the given
  // node for potential reductions. A free node will be marked upon entering the moves
  // loop by the constructor, and unmarked upon leaving that loop by the destructor.
  struct ThreadHolding {
    explicit ThreadHolding(Thread* thisThread, Key posKey, int ply) {
       location = ply < 8 ? &breadcrumbs[posKey & (breadcrumbs.size() - 1)] : nullptr;
       otherThread = false;
       owning = false;
       if (location)
       {
          // See if another already marked this location, if not, mark it ourselves
          Thread* tmp = (*location).thread.load(std::memory_order_relaxed);
          if (tmp == nullptr)
          {
              (*location).thread.store(thisThread, std::memory_order_relaxed);
              (*location).key.store(posKey, std::memory_order_relaxed);
              owning = true;
          }
          else if (   tmp != thisThread
                   && (*location).key.load(std::memory_order_relaxed) == posKey)
              otherThread = true;
       }
    }

    ~ThreadHolding() {
       if (owning) // Free the marked location
           (*location).thread.store(nullptr, std::memory_order_relaxed);
    }

    bool marked() { return otherThread; }

    private:
    Breadcrumb* location;
    bool otherThread, owning;
  };

  template <NodeType NT>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode);

  template <NodeType NT>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth = 0);

  Value value_to_tt(Value v, int ply);
  Value value_from_tt(Value v, int ply, int r50c);
  void update_pv(Move* pv, Move move, Move* childPv);
  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
  void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus, int depth);
  void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, Square prevSq,
                        Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth);

  // perft() is our utility to verify move generation. All the leaf nodes up
  // to the given depth are generated and counted, and the sum is returned.
  template<bool Root>
  uint64_t perft(Position& pos, Depth depth) {

    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::kCacheLineSize);

    uint64_t cnt, nodes = 0;
    const bool leaf = (depth == 2);

    for (const auto& m : MoveList<LEGAL>(pos))
    {
        if (Root && depth <= 1)
            cnt = 1, nodes++;
        else
        {
            pos.do_move(m, st);
            cnt = leaf ? MoveList<LEGAL>(pos).size() : perft<false>(pos, depth - 1);
            nodes += cnt;
            pos.undo_move(m);
        }
        if (Root)
            sync_cout << UCI::move(m, pos.is_chess960()) << ": " << cnt << sync_endl;
    }
    return nodes;
  }

} // namespace


/// Search::init() is called at startup to initialize various lookup tables

void Search::init() {

  for (int i = 1; i < MAX_MOVES; ++i)
      Reductions[i] = int((21.3 + 2 * std::log(Threads.size())) * std::log(i + 0.25 * std::log(i)));
}


/// Search::clear() resets search state to its initial value

void Search::clear() {

  Threads.main()->wait_for_search_finished();

  Time.availableNodes = 0;
  TT.clear();
  Threads.clear();
  Tablebases::init(Options["SyzygyPath"]); // Free mapped files
}


/// MainThread::search() is started when the program receives the UCI 'go'
/// command. It searches from the root position and outputs the "bestmove".

void MainThread::search() {

  if (Limits.perft)
  {
      nodes = perft<true>(rootPos, Limits.perft);
      sync_cout << "\nNodes searched: " << nodes << "\n" << sync_endl;
      return;
  }

    PRNG rng(now());

    adaptive            = Options["Adaptive_Play"];
    defensive           = Options["Defensive"];
    fide                = Options["FIDE_Ratings"];
    minOutput           = Options["Minimal Output"];
    tactical            = Options["Tactical"];
    uci_elo             = (Options["UCI_Elo"]);
    uci_sleep           = Options["Slow Play"];
    mpv                 = Options["MultiPV"];

    proValue            = Options["Pro Value"];
    profound            = Options["Pro Analysis"];
    dpa                 = Options["Deep Pro Analysis"];


  Color us = rootPos.side_to_move();
  Time.init(Limits, us, rootPos.game_ply());
  TT.new_search();

  Eval::NNUE::verify();

  //from Shashin
  tal=Options["Tal"];
  capablanca=Options["Capablanca"];
  petrosian=Options["Petrosian"];

#ifndef Noir
  if (rootMoves.empty())
  {
      rootMoves.emplace_back(MOVE_NONE);
      sync_cout << "info depth 0 score "
                << UCI::value(rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW)
                << sync_endl;
  }
#else
   if (rootMoves.empty())
  {
      rootMoves.emplace_back(MOVE_NONE);
      sync_cout << "info depth 0 score "
                << UCI::value(rootPos.checkers() ? -VALUE_MATE, -VALUE_MATE : VALUE_DRAW, VALUE_DRAW)
                << sync_endl;
  }
#endif
  else
  {

      Move bookMove = MOVE_NONE;

      if (bool(Options["Use_Book_1"]) && !Limits.infinite && !Limits.mate)
          bookMove = polybook1.probe(rootPos);
      if (bool(Options["Use_Book_2"]) && !bookMove && !Limits.infinite && !Limits.mate)
          bookMove = polybook2.probe(rootPos);
      if (bool(Options["Use_Book_3"]) && !bookMove && !Limits.infinite && !Limits.mate)
          bookMove = polybook3.probe(rootPos);
      if (bool(Options["Use_Book_4"]) && !bookMove && !Limits.infinite && !Limits.mate)
          bookMove = polybook4.probe(rootPos);

      if (bookMove && std::count(rootMoves.begin(), rootMoves.end(), bookMove))
      {
          for (Thread* th : Threads)
              std::swap(th->rootMoves[0], *std::find(th->rootMoves.begin(), th->rootMoves.end(), bookMove));
      }
      else
      {
         if (Options["NPS_Level"])
         {
             benchKnps = 1000 * (Options["Bench_KNPS"]);
             sync_cout << "Bknps: " << benchKnps  << sync_endl;// for debug
             int nps = 64 * pow(1.1486, (Options["NPS_Level"] - 1 ));
             Limits.nodes = nps;
             Limits.nodes *= Time.optimum()/1000 + 1 ;
             std::this_thread::sleep_for (std::chrono::milliseconds(Time.optimum()) * double(1 - Limits.nodes/benchKnps));
         }
         else if (Options["UCI_LimitStrength"] && Options["Engine_Level"] == "None")
         {
             uci_elo = (Options["UCI_Elo"]);
             limitStrength = true;
             adaptive=true;
             goto skipLevels;
         }
         if (Options["Engine_Level"] == "None")
         {
             limitStrength = false;
             goto skipLevels;
         }
         else
             {
              limitStrength = true;
              adaptive = true;
             }

         if (Options["Engine_Level"] == "World_Champion")
             uci_elo = 2900;
         else if (Options["Engine_Level"] == "Super_GM")
             uci_elo = 2800;
         else if (Options["Engine_Level"] == "GM")
             uci_elo = 2650;
         else if (Options["Engine_Level"] == "Deep_Thought")
             uci_elo = 2500;
         else if (Options["Engine_Level"] == "SIM")
             uci_elo = 2400;
		 else if (Options["Engine_Level"] == "IM")
             uci_elo = 2300;
         else if (Options["Engine_Level"] == "Cray_Blitz")
             uci_elo = 2200;
         else if (Options["Engine_Level"] == "Master")
             uci_elo = 2100;
         else if (Options["Engine_Level"] == "Expert")
             uci_elo = 1950;
         else if (Options["Engine_Level"] == "Class_A")
             uci_elo = 1800;
         else if (Options["Engine_Level"] == "Class_B")
             uci_elo = 1650;
         else if (Options["Engine_Level"] == "Class_C")
             uci_elo = 1500;
         else if (Options["Engine_Level"] == "Class_D")
             uci_elo = 1350;
         else if (Options["Engine_Level"] == "Boris")
             uci_elo = 1200;
         else if (Options["Engine_Level"] == "Novice")
             uci_elo = 1000;
skipLevels:

         if (limitStrength)
         {  //note varietry strength is capped around ~2150-2200 due to its robustness
             benchKnps = 1000 * (Options["Bench_KNPS"]);
             int random = (rand() % 20 - 10);
             uci_elo = uci_elo + random;
             //sync_cout << "Elo " << uci_elo << sync_endl;// for debug

             int ccrlELo = uci_elo;
             if (fide)
                 uci_elo = (((uci_elo * 10) / 7) - 1200);  //shallow adj was only required to get CCRL rating correct
             uci_elo += 200; //  to offset Elo loss with adaptive
             uci_elo = std::min(uci_elo, 3200);
             int NodesToSearch  =  pow(1.00382, (uci_elo - 900)) * 48;
             //sync_cout << "Nodes To Search: " << NodesToSearch << sync_endl;//for debug
             Limits.nodes = NodesToSearch;
             Limits.nodes = (Limits.nodes * Time.optimum()/1000);
             Limits.nodes = std::clamp(Limits.nodes, (int64_t) 48,Limits.nodes);
             int sleepTime = Time.optimum() * double(1 - Limits.nodes/benchKnps);
             //int sleepValue = Time.optimum() * double(1 - Limits.nodes/benchKnps);
             //sync_cout << "Sleep time: " << sleepValue << sync_endl;//for debug
             //sync_cout << "Limit Nodes: " <<  Limits.nodes << sync_endl;//for debug

             if (uci_sleep)

                 {
                   sync_cout <<  " info game slowed down to avoid instant move: " << sleepTime << " milliseconds\n" << sync_endl;// for debug
                   sync_cout << sync_endl;
                   std::this_thread::sleep_for (std::chrono::milliseconds(Time.optimum()) * double(1 - Limits.nodes/benchKnps));
                 }
             uci_elo =  ccrlELo;
         }

      if (mpv > 1)
          profound = false;

      if (!tactical && profound)

      {

          if (proValue && !dpa)
               profound_v = proValue * (std::max(Time.optimum(),Limits.movetime));
          else if (dpa)
              {
                profound_v = proValue * 900000;
                //std::cerr << "\nPro Analysis value2: " << profound_v << "\n" << sync_endl; //debug
              }
          //std::cerr << "\nPro Analysis value: " << profound_v << "\n" << sync_endl; //debug
        }

      Threads.start_searching(); // start non-main threads
      Thread::search();          // main thread start searching
      }
  }

  // When we reach the maximum depth, we can arrive here without a raise of
  // Threads.stop. However, if we are pondering or in an infinite search,
  // the UCI protocol states that we shouldn't print the best move before the
  // GUI sends a "stop" or "ponderhit" command. We therefore simply wait here
  // until the GUI sends one of those commands.

  while (!Threads.stop && (ponder || Limits.infinite))
  {} // Busy wait for a stop or a ponder reset

  // Stop the threads if not already stopped (also raise the stop if
  // "ponderhit" just reset Threads.ponder).
  Threads.stop = true;

  // Wait until all threads have finished
  Threads.wait_for_search_finished();

  // When playing in 'nodes as time' mode, subtract the searched nodes from
  // the available ones before exiting.
  if (Limits.npmsec)
      Time.availableNodes += Limits.inc[us] - Threads.nodes_searched();

  Thread* bestThread = this;

  if (   int(Options["MultiPV"]) == 1
      && !Limits.depth
      && !(Skill(Options["Skill Level"]).enabled() || int(Options["UCI_LimitStrength"]) || limitStrength)
      && rootMoves[0].pv[0] != MOVE_NONE)
      bestThread = Threads.get_best_thread();

  bestPreviousScore = bestThread->rootMoves[0].score;

  // Send again PV info if we have a new best thread

  if (bestThread != this || Skill(Options["Skill Level"]).enabled())
      sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE) << sync_endl;


  if  (adaptive )
  {

      size_t i = 0;
      if ( bestPreviousScore >= -PawnValueMg && bestPreviousScore <= PawnValueMg * 5 )
      {
          while (i+1 < rootMoves.size() && bestThread->rootMoves[i+1].score > bestPreviousScore)
          ++i;
          bestPreviousScore = bestThread->rootMoves[i].score;
          sync_cout  << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE) << sync_endl;
          sync_cout << "bestmove " << UCI::move(bestThread->rootMoves[i].pv[0], rootPos.is_chess960());
      }
      else if ( bestPreviousScore > PawnValueMg * 4  && bestPreviousScore <  PawnValueMg * 10 )
      {
          while (i+1 < rootMoves.size() && bestThread->rootMoves[i+1].score < bestPreviousScore
                && bestPreviousScore + PawnValueMg/2  > bestThread->rootMoves[i+1].score)
          {
              ++i;
              break;
          }
          bestPreviousScore = bestThread->rootMoves[i].score;
          while (i+1 < rootMoves.size() && bestThread->rootMoves[i+1].score > bestPreviousScore
                && bestPreviousScore + PawnValueMg/2  < bestThread->rootMoves[i+1].score)
          {
              ++i;
              bestPreviousScore = bestThread->rootMoves[i-1].score;

          }
          bestPreviousScore = bestThread->rootMoves[i].score;
          sync_cout  << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE) << sync_endl;
          sync_cout << "bestmove " << UCI::move(bestThread->rootMoves[i].pv[0], rootPos.is_chess960());
      }
      else
      {
          sync_cout << "bestmove " << UCI::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());
          if (bestThread->rootMoves[0].pv.size() > 1 || bestThread->rootMoves[0].extract_ponder_from_tt(rootPos))
              std::cout << " ponder " << UCI::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());
      }
  }
  else
  {
  sync_cout << "bestmove " << UCI::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());

  if (bestThread->rootMoves[0].pv.size() > 1 || bestThread->rootMoves[0].extract_ponder_from_tt(rootPos))
      std::cout << " ponder " << UCI::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());
  }
  std::cout << sync_endl;
}
//from Shashin
inline int getShashinValue(Value score) {
  if ((int)score < -SHASHIN_TAL_THRESHOLD) {
	  return SHASHIN_POSITION_PETROSIAN;
  }
  if (((int)score >= -SHASHIN_TAL_THRESHOLD)
		  && ((int)score <= -SHASHIN_CAPABLANCA_THRESHOLD)) {
	  return SHASHIN_POSITION_CAPABLANCA_PETROSIAN;
  }
  if (((int)score < SHASHIN_CAPABLANCA_THRESHOLD)) {
	  return SHASHIN_POSITION_CAPABLANCA;
  }
  if (((int)score >= SHASHIN_CAPABLANCA_THRESHOLD)
		  && ((int)score <= SHASHIN_TAL_THRESHOLD)) {
	  return SHASHIN_POSITION_TAL_CAPABLANCA;
  }
  if ((int)score > SHASHIN_TAL_THRESHOLD) {
	  return SHASHIN_POSITION_TAL;
  }
  return SHASHIN_POSITION_TAL_CAPABLANCA_PETROSIAN;
}

inline int getShashinQuiescentCapablanca(Value score,int refScore) {
  return abs(score) > refScore ? 0 : 1;
}

inline uint8_t getInitialShashinValue() {
  if (!tal && !capablanca && !petrosian)
    return SHASHIN_POSITION_DEFAULT;
  if (tal && capablanca && !petrosian)
    return SHASHIN_POSITION_TAL_CAPABLANCA;
  if (tal && !capablanca && !petrosian)
    return SHASHIN_POSITION_TAL;
  if (!tal && capablanca && !petrosian)
    return SHASHIN_POSITION_CAPABLANCA;
  if (!tal && capablanca && petrosian)
    return SHASHIN_POSITION_CAPABLANCA_PETROSIAN;
  if (!tal && !capablanca && petrosian)
    return SHASHIN_POSITION_PETROSIAN;
  if (tal && capablanca && petrosian)
    return SHASHIN_POSITION_TAL_CAPABLANCA_PETROSIAN;
  return SHASHIN_POSITION_TAL_PETROSIAN;
}

inline int getInitialShashinQuiescent(){
  if ((!tal && !capablanca && !petrosian)
      ||
      (!tal && capablanca && !petrosian))
	  return 1;
  return 0;
}

inline void initShashinValues (Position& pos)
{
  pos.this_thread()->shashinValue = getInitialShashinValue ();
  pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore = getInitialShashinQuiescent ();
  pos.this_thread()->shashinQuiescentCapablancaMaxScore = getInitialShashinQuiescent ();
}
inline void updateShashinValues (Position& pos,Value score)
{
  pos.this_thread()->shashinValue = getShashinValue (score);
  pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore = getShashinQuiescentCapablanca (score, SHASHIN_MIDDLE_HIGH_SCORE);
  pos.this_thread()->shashinQuiescentCapablancaMaxScore = getShashinQuiescentCapablanca (score, SHASHIN_MAX_SCORE);
}
inline void revertShashinValues (Position& pos,int lastShashinValue, int lastShashinQuiescentCapablancaMiddleHighScore, int lastShashinQuiescentCapablancaMaxScore)
{
  pos.this_thread()->shashinValue = lastShashinValue;
  pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore = lastShashinQuiescentCapablancaMiddleHighScore;
  pos.this_thread()->shashinQuiescentCapablancaMaxScore = lastShashinQuiescentCapablancaMaxScore;
}


/// Thread::search() is the main iterative deepening loop. It calls search()
/// repeatedly with increasing depth until the allocated thinking time has been
/// consumed, the user stops the search, or the maximum search depth is reached.

void Thread::search() {

  // To allow access to (ss-7) up to (ss+2), the stack must be oversized.
  // The former is needed to allow update_continuation_histories(ss-1, ...),
  // which accesses its argument at ss-6, also near the root.
  // The latter is needed for statScores and killer initialization.
  Stack stack[MAX_PLY+10], *ss = stack+7;
  Move  pv[MAX_PLY+1];
  Value bestValue, alpha, beta, delta;
  Move  lastBestMove = MOVE_NONE;
  Depth lastBestMoveDepth = 0;
  MainThread* mainThread = (this == Threads.main() ? Threads.main() : nullptr);
  double timeReduction = 1, totBestMoveChanges = 0;
  Color us = rootPos.side_to_move();
  ctempt= Options["Contempt"];
  if (Options["Defensive"])
      defensive_v = -34;  //about 16 cp



  int iterIdx = 0;

  std::memset(ss-7, 0, 10 * sizeof(Stack));
  for (int i = 7; i > 0; i--)
      (ss-i)->continuationHistory = &this->continuationHistory[0][0][NO_PIECE][0]; // Use as a sentinel

  ss->pv = pv;

  bestValue = delta = alpha = -VALUE_INFINITE;
  beta = VALUE_INFINITE;

  if (mainThread)
  {
      if (mainThread->bestPreviousScore == VALUE_INFINITE)
          for (int i = 0; i < 4; ++i)
              mainThread->iterValue[i] = VALUE_ZERO;
      else
          for (int i = 0; i < 4; ++i)
              mainThread->iterValue[i] = mainThread->bestPreviousScore;
  }

  std::copy(&lowPlyHistory[2][0], &lowPlyHistory.back().back() + 1, &lowPlyHistory[0][0]);
  std::fill(&lowPlyHistory[MAX_LPH - 2][0], &lowPlyHistory.back().back() + 1, 0);

  size_t multiPV = size_t(Options["MultiPV"]);

  // Pick integer skill levels, but non-deterministically round up or down
  // such that the average integer skill corresponds to the input floating point one.
  // UCI_Elo is converted to a suitable fractional skill level, using anchoring
  // to CCRL Elo (goldfish 1.13 = 2000) and a fit through Ordo derived Elo
  // for match (TC 60+0.6) results spanning a wide range of k values.
  PRNG rng(now());
  double floatLevel = Options["UCI_LimitStrength"] ?
                      std::clamp(std::pow((Options["UCI_Elo"] -  946.6) / 71.7, 1 / 0.806), 0.0, 40.0) :
                        double(Options["Skill Level"]);
  int intLevel = int(floatLevel) +
                 ((floatLevel - int(floatLevel)) * 1024 > rng.rand<unsigned>() % 1024  ? 1 : 0);
  Skill skill(intLevel);

  // When playing with strength handicap enable MultiPV search that we will
  // use behind the scenes to retrieve a set of possible moves.
  if (skill.enabled())
      multiPV = std::max(multiPV, (size_t)4);

  multiPV = std::min(multiPV, rootMoves.size());
#ifndef Noir
  ttHitAverage = TtHitAverageWindow * TtHitAverageResolution / 2;
#else
  ttHitAverage = ttHitAverageWindow * ttHitAverageResolution / 2;
#endif
  int ct = int(ctempt) * (int(Options["Contempt_Value"]) * PawnValueEg / 100); // From centipawns
  // In analysis mode, adjust contempt in accordance with user preference
  if (Limits.infinite || Options["AnalyseMode"])
      ct =  Options["Analysis_Contempt"] == "Off"  ? 0
          : Options["Analysis_Contempt"] == "Both" ? ct
          : Options["Analysis_Contempt"] == "White" && us == BLACK ? -ct
          : Options["Analysis_Contempt"] == "Black" && us == WHITE ? -ct
          : ct;

  // Evaluation score is from the white point of view
  contempt = (us == WHITE ?  make_score(ct, ct / 2)
                          : -make_score(ct, ct / 2));

  //from Shashin
  initShashinValues (rootPos);
  int searchAgainCounter = 0;

  // Iterative deepening loop until requested to stop or the target depth is reached
  while (   ++rootDepth < MAX_PLY
         && !Threads.stop
         && !(Limits.depth && mainThread && rootDepth > Limits.depth))
  {
      // Age out PV variability metric
      if (mainThread)
          totBestMoveChanges /= 2;

      // Save the last iteration's scores before first PV line is searched and
      // all the move scores except the (new) PV are set to -VALUE_INFINITE.
      for (RootMove& rm : rootMoves)
          rm.previousScore = rm.score;

      size_t pvFirst = 0;
      pvLast = 0;

      if (!Threads.increaseDepth)
         searchAgainCounter++;

      // MultiPV loop. We perform a full root search for each PV line

  profound_test = false;
   //std::cerr << "\nPro Analysis value test 2: " << profound_v << "\n" << sync_endl;//debug
    size_t multiPVStore = multiPV;
    if ( profound && !tactical && multiPV == 1 && profound_v )
    {

        if (Threads.nodes_searched() <= (uint64_t)profound_v)
          {
            profound_test = true;
            multiPV = std::min(8, int(rootMoves.size()));
          }

         else if (Threads.nodes_searched() > (uint64_t)profound_v)
          {
            profound_test = false;
            multiPV = multiPVStore;//Options["MultiPV"];
          }
      }

      for (pvIdx = 0; pvIdx < multiPV && !Threads.stop; ++pvIdx)
      {
          if (pvIdx == pvLast)
          {
              pvFirst = pvLast;
              for (pvLast++; pvLast < rootMoves.size(); pvLast++)
                  if (rootMoves[pvLast].tbRank != rootMoves[pvFirst].tbRank)
                      break;
          }

          // Reset UCI info selDepth for each depth and each PV line
          selDepth = 0;

          // Reset aspiration window starting size
#if defined (Sullivan) ||(Blau)
          if (rootDepth >= 5)
#else
          if (rootDepth >= 4)
#endif
          {
              Value prev = rootMoves[pvIdx].previousScore;

#if defined (Sullivan) || (Blau)
              delta = Value(18 + abs(prev) / 64);
#else
              delta = Value(17);
#endif
              alpha = std::max(prev - delta,-VALUE_INFINITE);
              beta  = std::min(prev + delta, VALUE_INFINITE);
              updateShashinValues(rootPos,prev);//from ShashChess

              // Adjust contempt based on root move's bestPreviousScore (dynamic contempt)
              dct = ctempt * (ct + (113 - ct / 2) * prev / (abs(prev) + 147) + defensive_v);

              contempt = (us == WHITE ?  make_score(dct, dct / 2)
                                      : -make_score(dct, dct / 2));
          }
#ifndef Noir  //Noir starts at line 2490
          // Start with a small aspiration window and, in the case of a fail
          // high/low, re-search with a bigger window until we don't fail
          // high/low anymore.
          failedHighCnt = 0;
          while (true)
          {
              Depth adjustedDepth = std::max(1, rootDepth - failedHighCnt - searchAgainCounter);
              bestValue = ::search<PV>(rootPos, ss, alpha, beta, adjustedDepth, false);

              // Bring the best move to the front. It is critical that sorting
              // is done with a stable algorithm because all the values but the
              // first and eventually the new best one are set to -VALUE_INFINITE
              // and we want to keep the same order for all the moves except the
              // new PV that goes to the front. Note that in case of MultiPV
              // search the already searched PV lines are preserved.
              std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

              // If search has been stopped, we break immediately. Sorting is
              // safe because RootMoves is still valid, although it refers to
              // the previous iteration.
              if (Threads.stop)
                  break;

// When failing high/low give some update (without cluttering
              // the UI) before a re-search.
              if (   mainThread
                  && multiPV == 1
                  && (bestValue <= alpha || bestValue >= beta)
                  && Time.elapsed() > 3000)
                  sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;

              // In case of failing low/high increase aspiration window and
              // re-search, otherwise exit the loop.
              if (bestValue <= alpha)
              {
                  beta = (alpha + beta) / 2;
                  alpha = std::max(bestValue - delta, -VALUE_INFINITE);

                  failedHighCnt = 0;
                  if (mainThread)
                      mainThread->stopOnPonderhit = false;
              }
              else if (bestValue >= beta)
              {
                  beta = std::min(bestValue + delta, VALUE_INFINITE);
                  ++failedHighCnt;
              }
#if defined (Stockfish)
              else
                  break;
#else
              else
              {
                  ++rootMoves[pvIdx].best_move_count;
                  break;
              }
#endif


              delta += delta / 4 + 5;

              assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
          }

          // Sort the PV lines searched so far and update the GUI
          std::stable_sort(rootMoves.begin() + pvFirst, rootMoves.begin() + pvIdx + 1);

          if (    mainThread
              && (Threads.stop || pvIdx + 1 == multiPV || Time.elapsed() > 3000))
              sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;
              //int dctcp = dct * 100/PawnValueEg; //for debug
              //sync_cout << "Contempt MG: " << dctcp  << sync_endl;// for debug
              //sync_cout << "Contempt EG: " << dctcp/2  << "\n" << sync_endl;// for debug
      }

      if (!Threads.stop)
          completedDepth = rootDepth;

      if (rootMoves[0].pv[0] != lastBestMove) {
         lastBestMove = rootMoves[0].pv[0];
         lastBestMoveDepth = rootDepth;
      }

      // Have we found a "mate in x"?
      if (   Limits.mate
          && bestValue >= VALUE_MATE_IN_MAX_PLY
          && VALUE_MATE - bestValue <= 2 * Limits.mate)
          Threads.stop = true;

      if (!mainThread)
          continue;

      if (Options["FastPlay"])
        {   if ( Time.elapsed() > Time.optimum() / 256
          && ( abs(bestValue) > 12300 ||  abs(bestValue) >= VALUE_MATE_IN_MAX_PLY ))
          Threads.stop = true;
        }

      // If skill level is enabled and time is up, pick a sub-optimal best move
      if (skill.enabled() && skill.time_to_pick(rootDepth))
          skill.pick_best(multiPV);

      // Do we have time for the next iteration? Can we stop searching now?
      if (    Limits.use_time_management()
          && !Threads.stop
          && !mainThread->stopOnPonderhit)
      {
          double fallingEval = (318 + 6 * (mainThread->bestPreviousScore - bestValue)
                                    + 6 * (mainThread->iterValue[iterIdx] - bestValue)) / 825.0;
          fallingEval = std::clamp(fallingEval, 0.5, 1.5);

          // If the bestMove is stable over several iterations, reduce time accordingly
          timeReduction = lastBestMoveDepth + 9 < completedDepth ? 1.92 : 0.95;
          double reduction = (1.47 + mainThread->previousTimeReduction) / (2.32 * timeReduction);

          // Use part of the gained time from a previous stable move for the current move
          for (Thread* th : Threads)
          {
              totBestMoveChanges += th->bestMoveChanges;
              th->bestMoveChanges = 0;
          }
          double bestMoveInstability = 1 + 2 * totBestMoveChanges / Threads.size();

          double totalTime = Time.optimum() * fallingEval * reduction * bestMoveInstability;

          // Cap used time in case of a single legal move for a better viewer experience in tournaments
          // yielding correct scores and sufficiently fast moves.
          if (rootMoves.size() == 1)
              totalTime = std::min(500.0, totalTime);

          // Stop the search if we have exceeded the totalTime
          if (Time.elapsed() > totalTime)
          {
              // If we are allowed to ponder do not stop the search now but
              // keep pondering until the GUI sends "ponderhit" or "stop".
              if (mainThread->ponder)
                  mainThread->stopOnPonderhit = true;
              else
                  Threads.stop = true;
          }
          else if (   Threads.increaseDepth
                   && !mainThread->ponder
                   && Time.elapsed() > totalTime * 0.58)
                   Threads.increaseDepth = false;
          else
                   Threads.increaseDepth = true;
      }

      mainThread->iterValue[iterIdx] = bestValue;
      iterIdx = (iterIdx + 1) & 3;
  }

  if (!mainThread)
      return;

  mainThread->previousTimeReduction = timeReduction;

  // If skill level is enabled, swap best PV line with the sub-optimal one
  if (skill.enabled())
      std::swap(rootMoves[0], *std::find(rootMoves.begin(), rootMoves.end(),
                skill.best ? skill.best : skill.pick_best(multiPV)));
}


namespace {

  // search<>() is the main search function for both PV and non-PV nodes

  template <NodeType NT>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {

    constexpr bool PvNode = NT == PV;
    const bool rootNode = PvNode && ss->ply == 0;
    const Depth maxNextDepth = rootNode ? depth : depth + 1;

    int lastShashinValue, lastShashinQuiescentCapablancaMiddleHighScore, lastShashinQuiescentCapablancaMaxScore;//from Shashin


    // Check if we have an upcoming move which draws by repetition, or
    // if the opponent had an alternative move earlier to this position.
    if (   pos.rule50_count() >= 3
        && alpha < VALUE_DRAW
        && !rootNode
        && pos.has_game_cycle(ss->ply))
    {
        alpha = value_draw(pos.this_thread());
        if (alpha >= beta)
            return alpha;
    }

    // Dive into quiescence search when the depth reaches zero
    if (depth <= 0)
        return qsearch<NT>(pos, ss, alpha, beta);

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    Move pv[MAX_PLY+1], capturesSearched[32], quietsSearched[64];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::kCacheLineSize);

    TTEntry* tte;
    Key posKey;
    Move ttMove, move, excludedMove, bestMove;
    Depth extension, newDepth;
    Value bestValue, value, ttValue, eval, maxValue, probCutBeta;
    bool formerPv, givesCheck, improving, didLMR, priorCapture;
    bool captureOrPromotion, doFullDepthSearch, moveCountPruning,
         ttCapture, singularQuietLMR;
    Piece movedPiece;
    int moveCount, captureCount, quietCount;

    // Step 1. Initialize node
    Thread* thisThread = pos.this_thread();
    ss->inCheck = pos.checkers();
    priorCapture = pos.captured_piece();
    Color us = pos.side_to_move();
    moveCount = captureCount = quietCount = ss->moveCount = 0;
    bestValue = -VALUE_INFINITE;
    maxValue = VALUE_INFINITE;

    // Check for the available remaining time
    if (thisThread == Threads.main())
        static_cast<MainThread*>(thisThread)->check_time();

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && thisThread->selDepth < ss->ply + 1)
        thisThread->selDepth = ss->ply + 1;

    if (!rootNode)
    {
        // Step 2. Check for aborted search and immediate draw
        if (   Threads.stop.load(std::memory_order_relaxed)
            || pos.is_draw(ss->ply)
            || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos)
                                                        : value_draw(pos.this_thread());

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply+1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Same logic but with reversed
        // signs applies also in the opposite condition of being mated instead of giving
        // mate. In this case return a fail-high score.
        alpha = std::max(mated_in(ss->ply), alpha);
        beta = std::min(mate_in(ss->ply+1), beta);
        if (alpha >= beta)
            return alpha;
    }

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    (ss+1)->ply = ss->ply + 1;
    (ss+1)->ttPv = false;
    (ss+1)->excludedMove = bestMove = MOVE_NONE;
    (ss+2)->killers[0] = (ss+2)->killers[1] = MOVE_NONE;
    Square prevSq = to_sq((ss-1)->currentMove);

    // Initialize statScore to zero for the grandchildren of the current position.
    // So statScore is shared between all grandchildren and only the first grandchild
    // starts with statScore = 0. Later grandchildren start with the last calculated
    // statScore of the previous grandchild. This influences the reduction rules in
    // LMR which are based on the statScore of parent position.
    if (!rootNode)
        (ss+2)->statScore = 0;

    // Step 4. Transposition table lookup. We don't want the score of a partial
    // search to overwrite a previous full search TT value, so we use a different
    // position key in case of an excluded move.
    excludedMove = ss->excludedMove;
    posKey = excludedMove == MOVE_NONE ? pos.key() : pos.key() ^ make_key(excludedMove);
    tte = TT.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttMove =  rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0]
            : ss->ttHit    ? tte->move() : MOVE_NONE;
    if (!excludedMove)
        ss->ttPv = PvNode || (ss->ttHit && tte->is_pv());
    formerPv = ss->ttPv && !PvNode;

    // Update low ply history for previous move if we are near root and position is or has been in PV
    if (   ss->ttPv
        && depth > 12
        && ss->ply - 1 < MAX_LPH
        && !priorCapture
        && is_ok((ss-1)->currentMove))
        thisThread->lowPlyHistory[ss->ply - 1][from_to((ss-1)->currentMove)] << stat_bonus(depth - 5);

    // thisThread->ttHitAverage can be used to approximate the running average of ttHit
    thisThread->ttHitAverage =   (TtHitAverageWindow - 1) * thisThread->ttHitAverage / TtHitAverageWindow
                                + TtHitAverageResolution * ss->ttHit;

    // At non-PV nodes we check for an early TT cutoff
    if (  !PvNode
        && ss->ttHit
        && (pos.this_thread()->shashinQuiescentCapablancaMaxScore)
        && tte->depth() >= depth
        && ttValue != VALUE_NONE // Possible in case of TT access race
        && (ttValue >= beta ? (tte->bound() & BOUND_LOWER)
                            : (tte->bound() & BOUND_UPPER)))
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit
        if (ttMove)
        {
            if (ttValue >= beta)
            {
                // Bonus for a quiet ttMove that fails high
                if (!pos.capture_or_promotion(ttMove))
                    update_quiet_stats(pos, ss, ttMove, stat_bonus(depth), depth);

                // Extra penalty for early quiet moves of the previous ply
                if ((ss-1)->moveCount <= 2 && !priorCapture)
                    update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + 1));
            }
            // Penalty for a quiet ttMove that fails low
            else if (!pos.capture_or_promotion(ttMove))
            {
                int penalty = -stat_bonus(depth);
                thisThread->mainHistory[us][from_to(ttMove)] << penalty;
                update_continuation_histories(ss, pos.moved_piece(ttMove), to_sq(ttMove), penalty);
            }
        }

        // Partial workaround for the graph history interaction problem
        // For high rule50 counts don't produce transposition table cutoffs.
        if (pos.rule50_count() < 90)
            return ttValue;
    }

    // Step 5. Tablebases probe
    if (!rootNode && TB::Cardinality)
    {
        int piecesCount = pos.count<ALL_PIECES>();

        if (    piecesCount <= TB::Cardinality
            && (piecesCount <  TB::Cardinality || depth >= TB::ProbeDepth)
            &&  pos.rule50_count() == 0
            && !pos.can_castle(ANY_CASTLING))
        {
            TB::ProbeState err;
            TB::WDLScore wdl = Tablebases::probe_wdl(pos, &err);

            // Force check of time on the next occasion
            if (thisThread == Threads.main())
                static_cast<MainThread*>(thisThread)->callsCnt = 0;

            if (err != TB::ProbeState::FAIL)
            {
                thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);

                int drawScore = TB::UseRule50 ? 1 : 0;

                // use the range VALUE_MATE_IN_MAX_PLY to VALUE_TB_WIN_IN_MAX_PLY to score
                value =  wdl < -drawScore ? VALUE_MATED_IN_MAX_PLY + ss->ply + 1
                       : wdl >  drawScore ? VALUE_MATE_IN_MAX_PLY - ss->ply - 1
                                          : VALUE_DRAW + 2 * wdl * drawScore;

                Bound b =  wdl < -drawScore ? BOUND_UPPER
                         : wdl >  drawScore ? BOUND_LOWER : BOUND_EXACT;

                if (    b == BOUND_EXACT
                    || (b == BOUND_LOWER ? value >= beta : value <= alpha))
                {
                    tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, b,
                              std::min(MAX_PLY - 1, depth + 6),
                              MOVE_NONE, VALUE_NONE);

                    return value;
                }

                if (PvNode)
                {
                    if (b == BOUND_LOWER)
                        bestValue = value, alpha = std::max(alpha, bestValue);
                    else
                        maxValue = value;
                }
            }
        }
    }

    CapturePieceToHistory& captureHistory = thisThread->captureHistory;

    // Step 6. Static evaluation of the position
    if (ss->inCheck)
    {
        // Skip early pruning when in check
        ss->staticEval = eval = VALUE_NONE;
        updateShashinValues(pos,ss->staticEval);//from ShashChess
        improving = false;
        goto moves_loop;
    }
    else if (ss->ttHit)
    {
        // Never assume anything about values stored in TT
        ss->staticEval = eval = tte->eval();
        if (eval == VALUE_NONE)
            ss->staticEval = eval = evaluate(pos);

        // Randomize draw evaluation
        if (eval == VALUE_DRAW)
            eval = value_draw(thisThread);

        // Can ttValue be used as a better position evaluation?
        if (    ttValue != VALUE_NONE
            && (tte->bound() & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttValue;
    }
    else
    {
        // In case of null move search use previous static eval with a different sign
        // and addition of two tempos
        if ((ss-1)->currentMove != MOVE_NULL)
            ss->staticEval = eval = evaluate(pos);
        else
            ss->staticEval = eval = -(ss-1)->staticEval + 2 * Tempo;

        // Save static evaluation into transposition table
        tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_NONE, MOVE_NONE, eval);
    }

    updateShashinValues(pos,eval);//from ShashChess
    // Use static evaluation difference to improve quiet move ordering
    if (is_ok((ss-1)->currentMove) && !(ss-1)->inCheck && !priorCapture)
    {
        int bonus = std::clamp(-depth * 4 * int((ss-1)->staticEval + ss->staticEval - 2 * Tempo), -1000, 1000);
        thisThread->mainHistory[~us][from_to((ss-1)->currentMove)] << bonus;
    }

    // Set up improving flag that is used in various pruning heuristics
    // We define position as improving if static evaluation of position is better
    // Than the previous static evaluation at our turn
    // In case of us being in check at our previous move we look at move prior to it
    improving =  (ss-2)->staticEval == VALUE_NONE
               ? ss->staticEval > (ss-4)->staticEval || (ss-4)->staticEval == VALUE_NONE
               : ss->staticEval > (ss-2)->staticEval;

    // Step 7. Futility pruning: child node (~50 Elo)
    if (   !PvNode
#if defined (Sullivan) ||(Blau)
        &&  depth < 7
#else
        &&  depth < 9
#endif
        &&  !(pos.this_thread()->profound_test)
        &&  eval - futility_margin(depth, improving) >= beta
        &&  eval < VALUE_KNOWN_WIN) // Do not return unproven wins
        return eval;

    // Step 8.. Null move search with verification search (~40 Elo)
    if (   !PvNode
#if defined (Sullivan) ||(Blau)
        && depth > 5
#endif
        && (ss-1)->currentMove != MOVE_NULL
        && (ss-1)->statScore < 22977
        &&  eval >= beta
        &&  eval >= ss->staticEval
        &&  ss->staticEval >= beta - 30 * depth - 28 * improving + 84 * ss->ttPv + 168
        && !excludedMove
#ifdef Stockfish
        &&  pos.non_pawn_material(us)
#else
        &&  thisThread->selDepth + 3 > thisThread->rootDepth
        &&  pos.non_pawn_material(us) > BishopValueMg
#endif
        && (ss->ply >= thisThread->nmpMinPly || us != thisThread->nmpColor))
    {
        assert(eval - beta >= 0);

        // Null move dynamic reduction based on depth and value
        Depth R = (1015 + 85 * depth) / 256 + std::min(int(eval - beta) / 256, 3);

        ss->currentMove = MOVE_NULL;
        ss->continuationHistory = &thisThread->continuationHistory[0][0][NO_PIECE][0];

        //begin from Shashin
        lastShashinValue=pos.this_thread()->shashinValue,
        lastShashinQuiescentCapablancaMiddleHighScore=pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore,
	    lastShashinQuiescentCapablancaMaxScore=pos.this_thread()->shashinQuiescentCapablancaMaxScore;
        //end from Shashin

        pos.do_null_move(st);

        updateShashinValues(pos,(ss+1)->staticEval); //from Shashin

        Value nullValue = -search<NonPV>(pos, ss+1, -beta, -beta+1, depth-R, !cutNode);

        pos.undo_null_move();

        revertShashinValues(pos,lastShashinValue, lastShashinQuiescentCapablancaMiddleHighScore, lastShashinQuiescentCapablancaMaxScore);//from Shashin
        if (nullValue >= beta)
        {
            // Do not return unproven mate or TB scores
            if (nullValue >= VALUE_TB_WIN_IN_MAX_PLY)
                nullValue = beta;

            if (thisThread->nmpMinPly || (abs(beta) < VALUE_KNOWN_WIN && depth < 14))
                return nullValue;

            assert(!thisThread->nmpMinPly); // Recursive verification is not allowed

            // Do verification search at high depths, with null move pruning disabled
            // for us, until ply exceeds nmpMinPly.
            thisThread->nmpMinPly = ss->ply + 3 * (depth-R) / 4;
            thisThread->nmpColor = us;

            Value v = search<NonPV>(pos, ss, beta-1, beta, depth-R, false);

            thisThread->nmpMinPly = 0;

            if (v >= beta)
                return nullValue;
        }
    }

    probCutBeta = beta + 194 - 49 * improving;

    // Step 9. ProbCut (~10 Elo)
    // If we have a good enough capture and a reduced search returns a value
    // much above beta, we can (almost) safely prune the previous move.
    if (   !PvNode
        &&  depth > 4
        &&  abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
        // if value from transposition table is lower than probCutBeta, don't attempt probCut
        // there and in further interactions with transposition table cutoff depth is set to depth - 3
        // because probCut search has depth set to depth - 4 but we also do a move before it
        // so effective depth is equal to depth - 3
        && !(   ss->ttHit
             && tte->depth() >= depth - 3
             && ttValue != VALUE_NONE
             && ttValue < probCutBeta))
    {
        // if ttMove is a capture and value from transposition table is good enough produce probCut
        // cutoff without digging into actual probCut search
        if (   ss->ttHit
            && tte->depth() >= depth - 3
            && ttValue != VALUE_NONE
            && ttValue >= probCutBeta
            && ttMove
            && pos.capture_or_promotion(ttMove))
            return probCutBeta;

        assert(probCutBeta < VALUE_INFINITE);
        MovePicker mp(pos, ttMove, probCutBeta - ss->staticEval, &captureHistory);
        int probCutCount = 0;
        bool ttPv = ss->ttPv;
        ss->ttPv = false;

        while (   (move = mp.next_move()) != MOVE_NONE
               && probCutCount < 2 + 2 * cutNode)
            if (move != excludedMove && pos.legal(move))
            {
                assert(pos.capture_or_promotion(move));
                assert(depth >= 5);

                captureOrPromotion = true;
                probCutCount++;

                ss->currentMove = move;
                ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                          [captureOrPromotion]
                                                                          [pos.moved_piece(move)]
                                                                          [to_sq(move)];
                //begin from Shashin
                lastShashinValue=pos.this_thread()->shashinValue,
                lastShashinQuiescentCapablancaMiddleHighScore=pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore,
		            lastShashinQuiescentCapablancaMaxScore=pos.this_thread()->shashinQuiescentCapablancaMaxScore;
                //end from Shashin
                pos.do_move(move, st);
                updateShashinValues(pos,(ss+1)->staticEval); //from Shashin
                // Perform a preliminary qsearch to verify that the move holds
                value = -qsearch<NonPV>(pos, ss+1, -probCutBeta, -probCutBeta+1);

                // If the qsearch held, perform the regular search
                if (value >= probCutBeta)
                    value = -search<NonPV>(pos, ss+1, -probCutBeta, -probCutBeta+1, depth - 4, !cutNode);

                pos.undo_move(move);
                revertShashinValues(pos,lastShashinValue, lastShashinQuiescentCapablancaMiddleHighScore, lastShashinQuiescentCapablancaMaxScore);//from Shashin

                if (value >= probCutBeta)
                {
                    // if transposition table doesn't have equal or more deep info write probCut data into it
                    if ( !(ss->ttHit
                       && tte->depth() >= depth - 3
                       && ttValue != VALUE_NONE))
                        tte->save(posKey, value_to_tt(value, ss->ply), ttPv,
                            BOUND_LOWER,
                            depth - 3, move, ss->staticEval);
                    return value;
                }
            }
         ss->ttPv = ttPv;
    }

    // Step 10. If the position is not in TT, decrease depth by 2
    if (   PvNode
#if defined (Sullivan) ||(Blau)
        && depth >=7
#else
        && depth >=6
#endif
        && !ttMove)
        depth -= 2;

moves_loop: // When in check, search starts from here

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr                   , (ss-4)->continuationHistory,
                                          nullptr                   , (ss-6)->continuationHistory };

    Move countermove = thisThread->counterMoves[pos.piece_on(prevSq)][prevSq];

    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &thisThread->lowPlyHistory,
                                      &captureHistory,
                                      contHist,
                                      countermove,
                                      ss->killers,
                                      ss->ply);

    value = bestValue;
    singularQuietLMR = moveCountPruning = false;
    ttCapture = ttMove && pos.capture_or_promotion(ttMove);

    // Mark this node as being searched
    ThreadHolding th(thisThread, posKey, ss->ply);

    // Step 11. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move(moveCountPruning)) != MOVE_NONE)
    {
      assert(is_ok(move));

      if (move == excludedMove)
          continue;

      // At root obey the "searchmoves" option and skip moves not listed in Root
      // Move List. As a consequence any illegal move is also skipped. In MultiPV
      // mode we also skip PV moves which have been already searched and those
      // of lower "TB rank" if we are in a TB root position.
      if (rootNode && !std::count(thisThread->rootMoves.begin() + thisThread->pvIdx,
                                  thisThread->rootMoves.begin() + thisThread->pvLast, move))
          continue;
#ifdef Stockfish
      // Check for legality
      if (!rootNode && !pos.legal(move))
          continue;
#endif
      ss->moveCount = ++moveCount;

      if (!minOutput && rootNode && thisThread == Threads.main() && Time.elapsed() > 5000)
          sync_cout << "info depth " << depth
                    << " currmove " << UCI::move(move, pos.is_chess960())
                    << " currmovenumber " << moveCount + thisThread->pvIdx << sync_endl;
      if (PvNode)
          (ss+1)->pv = nullptr;

      extension = 0;
      captureOrPromotion = pos.capture_or_promotion(move);
      movedPiece = pos.moved_piece(move);
      givesCheck = pos.gives_check(move);

      // Calculate new depth for this move
      newDepth = depth - 1;

      // Step 13. Pruning at shallow depth (~200 Elo)
      if (  !rootNode
          && pos.non_pawn_material(us)
          && bestValue > VALUE_TB_LOSS_IN_MAX_PLY)
      {
          // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold
          moveCountPruning = moveCount >= futility_move_count(improving, depth);

          // Reduced depth of the next LMR search
          int lmrDepth = std::max(newDepth - reduction(improving, depth, moveCount), 0);

          if (   captureOrPromotion
              || givesCheck)
          {
              // Capture history based pruning when the move doesn't give check
           	    //from Shashin begin
	    if(pos.this_thread()->shashinQuiescentCapablancaMaxScore)
            {
              if (   !givesCheck
                  && lmrDepth < 1
                  && captureHistory[movedPiece][to_sq(move)][type_of(pos.piece_on(to_sq(move)))] < 0)
                  continue;

              // SEE based pruning
              if (!pos.see_ge(move, Value(-218) * depth)) // (~25 Elo)
                  continue;
            }
           }
          else
          {
              // Countermoves based pruning (~20 Elo)
              if (   lmrDepth < 4 + ((ss-1)->statScore > 0 || (ss-1)->moveCount == 1)
                  && (*contHist[0])[movedPiece][to_sq(move)] < CounterMovePruneThreshold
                  && (*contHist[1])[movedPiece][to_sq(move)] < CounterMovePruneThreshold)
                  continue;

              // Futility pruning: parent node (~5 Elo)
              if (   lmrDepth < ((pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore)? 7:3) //lmrDepth patch
                  && !ss->inCheck
                  && ss->staticEval + 254 + 159 * lmrDepth <= alpha
                  &&  (*contHist[0])[movedPiece][to_sq(move)]
                    + (*contHist[1])[movedPiece][to_sq(move)]
                    + (*contHist[3])[movedPiece][to_sq(move)]
                    + (*contHist[5])[movedPiece][to_sq(move)] / 2 < 26394)
                  continue;

              // Prune moves with negative SEE (~20 Elo)
              if (!pos.see_ge(move, Value(-(30 - std::min(lmrDepth, 18)) * lmrDepth * lmrDepth)))
                  continue;
          }
      }

      // Step 13. Extensions (~75 Elo)

      // Singular extension search (~70 Elo). If all moves but one fail low on a
      // search of (alpha-s, beta-s), and just one fails high on (alpha, beta),
      // then that move is singular and should be extended. To verify this we do
      // a reduced search on all the other moves but the ttMove and if the
      // result is lower than ttValue minus a margin, then we will extend the ttMove.
      if (    depth >= 7
          &&  move == ttMove
          && !rootNode
          && !excludedMove // Avoid recursive singular search
       /* &&  ttValue != VALUE_NONE Already implicit in the next condition */
          &&  abs(ttValue) < VALUE_KNOWN_WIN
          && (tte->bound() & BOUND_LOWER)
          &&  tte->depth() >= depth - 3)
      {
          Value singularBeta = ttValue - ((formerPv + 4) * depth) / 2;
          Depth singularDepth = (depth - 1 + 3 * formerPv) / 2;
          ss->excludedMove = move;
          value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
          ss->excludedMove = MOVE_NONE;

          if (value < singularBeta)
          {
              extension = 1;
              singularQuietLMR = !ttCapture;
          }

          // Multi-cut pruning
          // Our ttMove is assumed to fail high, and now we failed high also on a reduced
          // search without the ttMove. So we assume this expected Cut-node is not singular,
          // that multiple moves fail high, and we can prune the whole subtree by returning
          // a soft bound.
          else if (singularBeta >= beta)
              return singularBeta;

          // If the eval of ttMove is greater than beta we try also if there is another
          // move that pushes it over beta, if so also produce a cutoff.
          else if (ttValue >= beta)
          {
              ss->excludedMove = move;
              value = search<NonPV>(pos, ss, beta - 1, beta, (depth + 3) / 2, cutNode);
              ss->excludedMove = MOVE_NONE;

              if (value >= beta)
                  return beta;
          }
      }

      // Check extension (~2 Elo)
      else if (    givesCheck
               && (pos.is_discovered_check_on_king(~us, move) || pos.see_ge(move)))
          extension = 1;

#ifndef Stockfish
      // Passed pawn extension
      else if (   move == ss->killers[0]
               && pos.advanced_pawn_push(move)
               && pos.pawn_passed(us, to_sq(move)))
          extension = 1;

      // Last captures extension
      else if (   PieceValue[EG][pos.captured_piece()] > PawnValueEg
               && pos.non_pawn_material() <= 2 * RookValueMg)
          extension = 1;
#endif

      // Castling extension
      if (   type_of(move) == CASTLING
          && popcount(pos.pieces(us) & ~pos.pieces(PAWN) & (to_sq(move) & KingSide ? KingSide : QueenSide)) <= 2)
          extension = 1;

      // Add extension to new depth
      newDepth += extension;

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

#ifndef Stockfish
      // Check for legality just before making the move
      if (!rootNode && !pos.legal(move))
       {
           ss->moveCount = --moveCount;
           continue;
       }
#endif
      // Update the current move (this must be done after singular extension search)
      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                [captureOrPromotion]
                                                                [movedPiece]
                                                                [to_sq(move)];

      // Step 15. Make the move
      //from Shashin begin
      lastShashinValue=pos.this_thread()->shashinValue,
      lastShashinQuiescentCapablancaMiddleHighScore=pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore,
      lastShashinQuiescentCapablancaMaxScore=pos.this_thread()->shashinQuiescentCapablancaMaxScore;
      //from Shashin end
      pos.do_move(move, st, givesCheck);
      updateShashinValues(pos,(ss+1)->staticEval);

      // Step 16. Reduced depth search (LMR, ~200 Elo). If the move fails high it will be
      // re-searched at full depth.
      if (    depth >= 3
        &&  moveCount > 1 + 2 * rootNode + 2 * (PvNode && abs(bestValue) < 2)
#ifndef Stockfish
#ifndef Noir
          && (!rootNode || thisThread->best_move_count(move) == 0)
#endif
#endif
          && (  !captureOrPromotion
              || moveCountPruning
              || ss->staticEval + PieceValue[EG][pos.captured_piece()] <= alpha
              || cutNode
              || (!PvNode && !formerPv && captureHistory[movedPiece][to_sq(move)][type_of(pos.captured_piece())] < 4506)
              || thisThread->ttHitAverage < 432 * TtHitAverageResolution * TtHitAverageWindow / 1024))
      {
          Depth r = reduction(improving, depth, moveCount);
          // Decrease reduction at non-check cut nodes for second move at low depths
          if (   cutNode
              && depth <= 10
              && moveCount <= 2
              && !ss->inCheck)
              r--;

          // Decrease reduction if the ttHit running average is large
          if (thisThread->ttHitAverage > 537 * TtHitAverageResolution * TtHitAverageWindow / 1024)
              r--;

          // Increase reduction if other threads are searching this position
          if (th.marked())
              r++;

          // Decrease reduction if position is or has been on the PV (~10 Elo)
          if (ss->ttPv)
              r -= 2;

          // Increase reduction at root and non-PV nodes when the best move does not change frequently
          if ((rootNode || !PvNode) && thisThread->rootDepth > 10 && thisThread->bestMoveChanges <= 2)
              r++;

          // More reductions for late moves if position was not in previous PV
          if (moveCountPruning && !formerPv)
              r++;

          // Decrease reduction if opponent's move count is high (~5 Elo)
          if ((ss-1)->moveCount > 13)
              r--;

          // Decrease reduction if ttMove has been singularly extended (~3 Elo)
          if (singularQuietLMR)
              r -= 1 + formerPv;

          if (captureOrPromotion)
          {
              // Unless giving check, this capture is likely bad
              if (   !givesCheck
                  && ss->staticEval + PieceValue[EG][pos.captured_piece()] + 210 * depth <= alpha)
                  r++;
          }
          else
          {
              // Increase reduction if ttMove is a capture (~5 Elo)
              if (ttCapture)
                  r++;

              // Increase reduction at root if failing high
              r += rootNode ? thisThread->failedHighCnt * thisThread->failedHighCnt * moveCount / 512 : 0;

              // Increase reduction for cut nodes (~10 Elo)
              if (cutNode)
                  r += 2;

              // Decrease reduction for moves that escape a capture. Filter out
              // castling moves, because they are coded as "king captures rook" and
              // hence break make_move(). (~2 Elo)
              else if (    type_of(move) == NORMAL
                       && !pos.see_ge(reverse_move(move)))
                  r -= 2 + ss->ttPv - (type_of(movedPiece) == PAWN);

              ss->statScore =  thisThread->mainHistory[us][from_to(move)]
                             + (*contHist[0])[movedPiece][to_sq(move)]
                             + (*contHist[1])[movedPiece][to_sq(move)]
                             + (*contHist[3])[movedPiece][to_sq(move)]
                             - 5287;

              // Decrease/increase reduction by comparing opponent's stat score (~10 Elo)
              if (ss->statScore >= -105 && (ss-1)->statScore < -103)
                  r--;

              else if ((ss-1)->statScore >= -122 && ss->statScore < -129)
                  r++;

              // Decrease/increase reduction for moves with a good/bad history (~30 Elo)
              // If we are not in check use statScore, if we are in check
              // use sum of main history and first continuation history with an offset
              if (ss->inCheck)
                  r -= (thisThread->mainHistory[us][from_to(move)]
                     + (*contHist[0])[movedPiece][to_sq(move)] - 4333) / 16384;
              else
                  r -= ss->statScore / 14884;
          }

          Depth d = std::clamp(newDepth - r, 1, newDepth);

          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, d, true);

          doFullDepthSearch = value > alpha && d != newDepth;

          didLMR = true;
      }
      else
      {
          doFullDepthSearch = !PvNode || moveCount > 1;

          didLMR = false;
      }

      // Step 17. Full depth search when LMR is skipped or fails high
      if (doFullDepthSearch)
      {
          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, newDepth, !cutNode);

          // If the move passed LMR update its stats
          if (didLMR && !captureOrPromotion)
          {
              int bonus = value > alpha ?  stat_bonus(newDepth)
                                        : -stat_bonus(newDepth);

              update_continuation_histories(ss, movedPiece, to_sq(move), bonus);
          }
      }

      // For PV nodes only, do a full PV search on the first move or after a fail
      // high (in the latter case search only if value < beta), otherwise let the
      // parent node fail low with value <= alpha and try another move.
      if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
      {
          (ss+1)->pv = pv;
          (ss+1)->pv[0] = MOVE_NONE;

          value = -search<PV>(pos, ss+1, -beta, -alpha,
                              std::min(maxNextDepth, newDepth), false);
      }

      // Step 18. Undo move
      pos.undo_move(move);
      revertShashinValues(pos,lastShashinValue, lastShashinQuiescentCapablancaMiddleHighScore, lastShashinQuiescentCapablancaMaxScore);//from Shashin
      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Step 19. Check for a new best move
      // Finished searching the move. If a stop occurred, the return value of
      // the search cannot be trusted, and we return immediately without
      // updating best move, PV and TT.
      if (Threads.stop.load(std::memory_order_relaxed))
      {
	  updateShashinValues(pos,VALUE_ZERO);
	  return VALUE_ZERO;
      }

      if (rootNode)
      {
          RootMove& rm = *std::find(thisThread->rootMoves.begin(),
                                    thisThread->rootMoves.end(), move);

          // PV move or new best move?
          if (moveCount == 1 || value > alpha)
          {
              rm.score = value;
              rm.selDepth = thisThread->selDepth;
              rm.pv.resize(1);

              assert((ss+1)->pv);

              for (Move* m = (ss+1)->pv; *m != MOVE_NONE; ++m)
                  rm.pv.push_back(*m);

              // We record how often the best move has been changed in each
              // iteration. This information is used for time management and LMR
              if (moveCount > 1)
                  ++thisThread->bestMoveChanges;
          }
          else
              // All other moves but the PV are set to the lowest value: this
              // is not a problem when sorting because the sort is stable and the
              // move position in the list is preserved - just the PV is pushed up.
              rm.score = -VALUE_INFINITE;
      }

      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
              bestMove = move;

              if (PvNode && !rootNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha! Always alpha < beta
                  alpha = value;
              else
              {
                  assert(value >= beta); // Fail high
                  ss->statScore = 0;
                  break;
              }
          }
      }

      // If the move is worse than some previously searched move, remember it to update its stats later
      if (move != bestMove)
      {
          if (captureOrPromotion && captureCount < 32)
              capturesSearched[captureCount++] = move;

          else if (!captureOrPromotion && quietCount < 64)
              quietsSearched[quietCount++] = move;
      }
    }

    // The following condition would detect a stop only after move loop has been
    // completed. But in this case bestValue is valid because we have fully
    // searched our subtree, and we can anyhow save the result in TT.
    /*
       if (Threads.stop)
        return VALUE_DRAW;
    */

    // Step 20. Check for mate and stalemate
    // All legal moves have been searched and if there are no legal moves, it
    // must be a mate or a stalemate. If we are in a singular extension search then
    // return a fail low score.

    assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    if (!moveCount)
        bestValue = excludedMove ? alpha
                   :     ss->inCheck ? mated_in(ss->ply) : VALUE_DRAW;

    // If there is a move which produces search value greater than alpha we update stats of searched moves
    else if (bestMove)
        update_all_stats(pos, ss, bestMove, bestValue, beta, prevSq,
                         quietsSearched, quietCount, capturesSearched, captureCount, depth);

    // Bonus for prior countermove that caused the fail low
    else if (   (depth >= 3 || PvNode)
             && !priorCapture)
        update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, stat_bonus(depth));

    if (PvNode)
        bestValue = std::min(bestValue, maxValue);

    // If a good move is not found and the previous position was ttPv, then the previous
    // opponent move is probably good and the new position is added to the search tree.
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || ((ss-1)->ttPv && depth > 3);
    // Otherwise, a counter move has been found and if the position is the last leaf
    // in the search tree, remove the position from the search tree.
    else if (depth > 3)
        ss->ttPv = ss->ttPv && (ss+1)->ttPv;

    // Write gathered information in transposition table
    if (!excludedMove && !(rootNode && thisThread->pvIdx))
        tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                  bestValue >= beta ? BOUND_LOWER :
                  PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
                  depth, bestMove, ss->staticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // qsearch() is the quiescence search function, which is called by the main search
  // function with zero depth, or recursively with further decreasing depth per call.
  template <NodeType NT>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {

    constexpr bool PvNode = NT == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(depth <= 0);

    Move pv[MAX_PLY+1];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::kCacheLineSize);

    TTEntry* tte;
    Key posKey;
    Move ttMove, move, bestMove;
    Depth ttDepth;
    Value bestValue, value, ttValue, futilityValue, futilityBase, oldAlpha;
    bool pvHit, givesCheck, captureOrPromotion;
    int moveCount;

    if (PvNode)
    {
        oldAlpha = alpha; // To flag BOUND_EXACT when eval above alpha and no available moves
        (ss+1)->pv = pv;
        ss->pv[0] = MOVE_NONE;
    }

    Thread* thisThread = pos.this_thread();
    (ss+1)->ply = ss->ply + 1;
    bestMove = MOVE_NONE;
    ss->inCheck = pos.checkers();
    moveCount = 0;

    // Check for an immediate draw or maximum ply reached
    if (   pos.is_draw(ss->ply)
        || ss->ply >= MAX_PLY)
        return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos) : VALUE_DRAW;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Decide whether or not to include checks: this fixes also the type of
    // TT entry depth that we are going to use. Note that in qsearch we use
    // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
    ttDepth = ss->inCheck || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS
                                                  : DEPTH_QS_NO_CHECKS;
    // Transposition table lookup
    posKey = pos.key();
    tte = TT.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttMove = ss->ttHit ? tte->move() : MOVE_NONE;
    pvHit = ss->ttHit && tte->is_pv();

    if (  !PvNode
        && ss->ttHit
	&& (pos.this_thread()->shashinQuiescentCapablancaMaxScore)
        && tte->depth() >= ttDepth
        && ttValue != VALUE_NONE // Only in case of TT access race
        && (ttValue >= beta ? (tte->bound() & BOUND_LOWER)
                            : (tte->bound() & BOUND_UPPER)))
        return ttValue;

    // Evaluate the position statically
    if (ss->inCheck)
    {
        ss->staticEval = VALUE_NONE;
        updateShashinValues(pos,ss->staticEval);//from ShashChess
        bestValue = futilityBase = -VALUE_INFINITE;
    }
    else
    {
        if (ss->ttHit)
        {

	  // Never assume anything on values stored in TT
	  if ((ss->staticEval = bestValue = tte->eval()) == VALUE_NONE)
	    //from ShashChess begin
	    {
	      ss->staticEval = bestValue = evaluate(pos);
	      updateShashinValues(pos,ss->staticEval);//from ShashChess
	    }
	    //from ShashChess end

	  // Can ttValue be used as a better position evaluation?
	  if (ttValue != VALUE_NONE
	      && (tte->bound() & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
	  {
	      bestValue = ttValue;
	      updateShashinValues(pos,ttValue);//from ShashChess
	  }

        }
        else
            // In case of null move search use previous static eval with a different sign
            // and addition of two tempos
            {
              ss->staticEval = bestValue =
              (ss-1)->currentMove != MOVE_NULL ? evaluate(pos)
                                             : -(ss-1)->staticEval + 2 * Tempo;
              updateShashinValues(pos,ss->staticEval);//from ShashChess
            }
        // Stand pat. Return immediately if static value is at least beta
        if (bestValue >= beta)
        {
            // Save gathered info in transposition table
            if (!ss->ttHit)
                tte->save(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER,
                          DEPTH_NONE, MOVE_NONE, ss->staticEval);

            return bestValue;
        }

        if (PvNode && bestValue > alpha)
            alpha = bestValue;

        futilityBase = bestValue + 155;
    }

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr                   , (ss-4)->continuationHistory,
                                          nullptr                   , (ss-6)->continuationHistory };

    // Initialize a MovePicker object for the current position, and prepare
    // to search the moves. Because the depth is <= 0 here, only captures,
    // queen and checking knight promotions, and other checks(only if depth >= DEPTH_QS_CHECKS)
    // will be generated.
    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &thisThread->captureHistory,
                                      contHist,
                                      to_sq((ss-1)->currentMove));

    // Loop through the moves until no moves remain or a beta cutoff occurs
    while ((move = mp.next_move()) != MOVE_NONE)
    {
      assert(is_ok(move));

      givesCheck = pos.gives_check(move);
      captureOrPromotion = pos.capture_or_promotion(move);

      moveCount++;

      // Futility pruning
      if (    bestValue > VALUE_TB_LOSS_IN_MAX_PLY
          && !givesCheck
          &&  futilityBase > -VALUE_KNOWN_WIN
          && !pos.advanced_pawn_push(move))
      {
          assert(type_of(move) != EN_PASSANT); // Due to !pos.advanced_pawn_push

          // moveCount pruning
          if (moveCount > 2)
              continue;

          futilityValue = futilityBase + PieceValue[EG][pos.piece_on(to_sq(move))];

          if (futilityValue <= alpha)
          {
              bestValue = std::max(bestValue, futilityValue);
              continue;
          }

          if (futilityBase <= alpha && !pos.see_ge(move, VALUE_ZERO + 1))
          {
              bestValue = std::max(bestValue, futilityBase);
              continue;
          }
      }

      // Do not search moves with negative SEE values
      if (    bestValue > VALUE_TB_LOSS_IN_MAX_PLY
          && !pos.see_ge(move))
          continue;

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      // Check for legality just before making the move
      if (!pos.legal(move))
      {
          moveCount--;
          continue;
      }

      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                [captureOrPromotion]
                                                                [pos.moved_piece(move)]
                                                                [to_sq(move)];

      // CounterMove based pruning
      if (  !captureOrPromotion
          && bestValue > VALUE_TB_LOSS_IN_MAX_PLY
          && (*contHist[0])[pos.moved_piece(move)][to_sq(move)] < CounterMovePruneThreshold
          && (*contHist[1])[pos.moved_piece(move)][to_sq(move)] < CounterMovePruneThreshold)
          continue;

      //from Shashin begin
      int lastShashinValue=pos.this_thread()->shashinValue,
      lastShashinQuiescentCapablancaMiddleHighScore=pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore,
      lastShashinQuiescentCapablancaMaxScore=pos.this_thread()->shashinQuiescentCapablancaMaxScore;
      //from Shashin end
      // Make and search the move
      pos.do_move(move, st, givesCheck);
      //from Shashin begin
       updateShashinValues(pos,(ss+1)->staticEval);
      //From Shashin end
      value = -qsearch<NT>(pos, ss+1, -beta, -alpha, depth - 1);
      pos.undo_move(move);
      //from Shashin begin
      revertShashinValues(pos,lastShashinValue, lastShashinQuiescentCapablancaMiddleHighScore, lastShashinQuiescentCapablancaMaxScore);
      //from Shashin end
      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Check for a new best move
      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
              bestMove = move;

              if (PvNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha here!
                  alpha = value;
              else
                  break; // Fail high
          }
       }
    }

    // All legal moves have been searched. A special case: If we're in check
    // and no legal moves were found, it is checkmate.
    if (ss->inCheck && bestValue == -VALUE_INFINITE)
    {
        assert(!MoveList<LEGAL>(pos).size());

        return mated_in(ss->ply); // Plies to mate from the root
    }

    // Save gathered info in transposition table
    tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit,
              bestValue >= beta ? BOUND_LOWER :
              PvNode && bestValue > oldAlpha  ? BOUND_EXACT : BOUND_UPPER,
              ttDepth, bestMove, ss->staticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // value_to_tt() adjusts a mate or TB score from "plies to mate from the root" to
  // "plies to mate from the current position". Standard scores are unchanged.
  // The function is called before storing a value in the transposition table.

  Value value_to_tt(Value v, int ply) {

    assert(v != VALUE_NONE);

    return  v >= VALUE_TB_WIN_IN_MAX_PLY  ? v + ply
          : v <= VALUE_TB_LOSS_IN_MAX_PLY ? v - ply : v;
  }


  // value_from_tt() is the inverse of value_to_tt(): it adjusts a mate or TB score
  // from the transposition table (which refers to the plies to mate/be mated from
  // current position) to "plies to mate/be mated (TB win/loss) from the root". However,
  // for mate scores, to avoid potentially false mate scores related to the 50 moves rule
  // and the graph history interaction, we return an optimal TB score instead.

  Value value_from_tt(Value v, int ply, int r50c) {
    return   v == VALUE_NONE ? VALUE_NONE
          :  v <= VALUE_MATED_IN_MAX_PLY ? v + ply
          : (v >= VALUE_MATE_IN_MAX_PLY) && (VALUE_MATE - v > 99 - r50c) ? VALUE_MATE_IN_MAX_PLY - 1
          :  v >= VALUE_MATE_IN_MAX_PLY ? v - ply : v;
    }



  // update_pv() adds current move and appends child pv[]

  void update_pv(Move* pv, Move move, Move* childPv) {

    for (*pv++ = move; childPv && *childPv != MOVE_NONE; )
        *pv++ = *childPv++;
    *pv = MOVE_NONE;
  }


  // update_all_stats() updates stats at the end of search() when a bestMove is found

  void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, Square prevSq,
                        Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth) {

    int bonus1, bonus2;
    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    CapturePieceToHistory& captureHistory = thisThread->captureHistory;
    Piece moved_piece = pos.moved_piece(bestMove);
    PieceType captured = type_of(pos.piece_on(to_sq(bestMove)));

    bonus1 = stat_bonus(depth + 1);
    bonus2 = bestValue > beta + PawnValueMg ? bonus1               // larger bonus
                                            : stat_bonus(depth);   // smaller bonus

    if (!pos.capture_or_promotion(bestMove))
    {
        // Increase stats for the best move in case it was a quiet move
        update_quiet_stats(pos, ss, bestMove, bonus2, depth);

        // Decrease stats for all non-best quiet moves
        for (int i = 0; i < quietCount; ++i)
        {
            thisThread->mainHistory[us][from_to(quietsSearched[i])] << -bonus2;
            update_continuation_histories(ss, pos.moved_piece(quietsSearched[i]), to_sq(quietsSearched[i]), -bonus2);
        }
    }
    else
        // Increase stats for the best move in case it was a capture move
        captureHistory[moved_piece][to_sq(bestMove)][captured] << bonus1;

    // Extra penalty for a quiet early move that was not a TT move or
    // main killer move in previous ply when it gets refuted.
    if (   ((ss-1)->moveCount == 1 + (ss-1)->ttHit || ((ss-1)->currentMove == (ss-1)->killers[0]))
        && !pos.captured_piece())
            update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -bonus1);

    // Decrease stats for all non-best capture moves
    for (int i = 0; i < captureCount; ++i)
    {
        moved_piece = pos.moved_piece(capturesSearched[i]);
        captured = type_of(pos.piece_on(to_sq(capturesSearched[i])));
        captureHistory[moved_piece][to_sq(capturesSearched[i])][captured] << -bonus1;
    }
  }


  // update_continuation_histories() updates histories of the move pairs formed
  // by moves at ply -1, -2, -4, and -6 with current move.

  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {

    for (int i : {1, 2, 4, 6})
    {
        // Only update first 2 continuation histories if we are in check
        if (ss->inCheck && i > 2)
            break;
        if (is_ok((ss-i)->currentMove))
            (*(ss-i)->continuationHistory)[pc][to] << bonus;
    }
  }


  // update_quiet_stats() updates move sorting heuristics

  void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus, int depth) {

    // Update killers
    if (ss->killers[0] != move)
    {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = move;
    }

    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    thisThread->mainHistory[us][from_to(move)] << bonus;
    update_continuation_histories(ss, pos.moved_piece(move), to_sq(move), bonus);

    // Penalty for reversed move in case of moved piece not being a pawn
    if (type_of(pos.moved_piece(move)) != PAWN)
        thisThread->mainHistory[us][from_to(reverse_move(move))] << -bonus;

    // Update countermove history
    if (is_ok((ss-1)->currentMove))
    {
        Square prevSq = to_sq((ss-1)->currentMove);
        thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] = move;
    }

    // Update low ply history
    if (depth > 11 && ss->ply < MAX_LPH)
        thisThread->lowPlyHistory[ss->ply][from_to(move)] << stat_bonus(depth - 7);
  }

  // When playing with strength handicap, choose best move among a set of RootMoves
  // using a statistical rule dependent on 'level'. Idea by Heinz van Saanen.

  Move Skill::pick_best(size_t multiPV) {

    const RootMoves& rootMoves = Threads.main()->rootMoves;
    static PRNG rng(now()); // PRNG sequence should be non-deterministic

    // RootMoves are already sorted by score in descending order
    Value topScore = rootMoves[0].score;
    int delta = std::min(topScore - rootMoves[multiPV - 1].score, PawnValueMg);
    int weakness = 120 - level;
    int maxScore = -VALUE_INFINITE;

    // Choose best move. For each move score we add two terms, both dependent on
    // weakness. One is deterministic and bigger for weaker levels, and one is
    // random. Then we choose the move with the resulting highest score.
    for (size_t i = 0; i < multiPV; ++i)
    {
        // This is our magic formula
        int push = (  weakness * int(topScore - rootMoves[i].score)
                    + delta * (rng.rand<unsigned>() % weakness)) / 128;

        if (rootMoves[i].score + push >= maxScore)
        {
            maxScore = rootMoves[i].score + push;
            best = rootMoves[i].pv[0];
        }
    }

    return best;
  }

} // namespace


/// MainThread::check_time() is used to print debug info and, more importantly,
/// to detect when we are out of available time and thus stop the search.

void MainThread::check_time() {

  if (--callsCnt > 0)
      return;

  // When using nodes, ensure checking rate is not lower than 0.1% of nodes
  callsCnt = Limits.nodes ? std::min(1024, int(Limits.nodes / 1024)) : 1024;

  static TimePoint tick = now();

  TimePoint elapsed = Time.elapsed();
  TimePoint tock = Limits.startTime + elapsed;
  Thread* bestThread = this;
  if (elapsed > 500)
  {
    if (elapsed <= 120050)
    {
      if (tock - tick >= 10000 && minOutput)
      {
        tick = tock;
        sync_cout  << "\n" << "info " << elapsed/1000 << " seconds"  << sync_endl;
        sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE) << sync_endl;
        //dbg_print();
      }
    }
    else if (elapsed <= 600100)
    {
      if (tock - tick >= 60000 && minOutput)
      {
        tick = tock;
        sync_cout  << "\n" << "info " << elapsed/60000 << " minutes" << sync_endl;
        sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE)  << sync_endl;
        //dbg_print();
      }
    }
    else
    {
      if (tock - tick >= 300000 && minOutput)
      {
        tick = tock;
        sync_cout  << "\n" << "info " << elapsed/60000 << " minutes" << sync_endl ;
        sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE)  << sync_endl;
        //dbg_print();
      }
    }
  }

  // We should not stop pondering until told so by the GUI
  if (ponder)
      return;

  if (   (Limits.use_time_management() && (elapsed > Time.maximum() - 10 || stopOnPonderhit))
      || (Limits.movetime && elapsed >= Limits.movetime)
      || (Limits.nodes && Threads.nodes_searched() >= (uint64_t)Limits.nodes))
      Threads.stop = true;
}


/// UCI::pv() formats PV information according to the UCI protocol. UCI requires
/// that all (if any) unsearched PV lines are sent using a previous search score.

string UCI::pv(const Position& pos, Depth depth, Value alpha, Value beta) {

  std::stringstream ss;
  TimePoint elapsed = Time.elapsed() + 1;
  const RootMoves& rootMoves = pos.this_thread()->rootMoves;
  size_t pvIdx = pos.this_thread()->pvIdx;
  size_t multiPV = std::min((size_t)Options["MultiPV"], rootMoves.size());
  uint64_t nodesSearched = Threads.nodes_searched();
  uint64_t tbHits = Threads.tb_hits() + (TB::RootInTB ? rootMoves.size() : 0);

  for (size_t i = 0; i < multiPV; ++i)
  {
      bool updated = rootMoves[i].score != -VALUE_INFINITE;

      if (depth == 1 && !updated && i > 0)
          continue;

      Depth d = updated ? depth : std::max(1, depth - 1);
      Value v = updated ? rootMoves[i].score : rootMoves[i].previousScore;

      if (v == -VALUE_INFINITE)
          v = VALUE_ZERO;

      bool tb = TB::RootInTB && abs(v) < VALUE_MATE_IN_MAX_PLY;
      v = tb ? rootMoves[i].tbScore : v;

      if (ss.rdbuf()->in_avail()) // Not at first line
          ss << "\n";

      ss << "info"
         << " depth "    << d
         << " seldepth " << rootMoves[i].selDepth
         << " multipv "  << i + 1
         << " score "    << UCI::value(v);

      if (Options["UCI_ShowWDL"])
          ss << UCI::wdl(v, pos.game_ply());

      if (!tb && i == pvIdx)
          ss << (v >= beta ? " lowerbound" : v <= alpha ? " upperbound" : "");

      ss << " nodes "    << nodesSearched
         << " nps "      << nodesSearched * 1000 / elapsed;

      if (elapsed > 1000) // Earlier makes little sense
          ss << " hashfull " << TT.hashfull();

      ss << " tbhits "   << tbHits
         << " time "     << elapsed
         << " pv";

      for (Move m : rootMoves[i].pv)
          ss << " " << UCI::move(m, pos.is_chess960());
  }

  return ss.str();
}


/// RootMove::extract_ponder_from_tt() is called in case we have no ponder move
/// before exiting the search, for instance, in case we stop the search during a
/// fail high at root. We try hard to have a ponder move to return to the GUI,
/// otherwise in case of 'ponder on' we have nothing to think on.

bool RootMove::extract_ponder_from_tt(Position& pos) {

    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::kCacheLineSize);

    bool ttHit;

    assert(pv.size() == 1);

    if (pv[0] == MOVE_NONE)
        return false;

    pos.do_move(pv[0], st);
    TTEntry* tte = TT.probe(pos.key(), ttHit);

    if (ttHit)
    {
        Move m = tte->move(); // Local copy to be SMP safe
        if (MoveList<LEGAL>(pos).contains(m))
            pv.push_back(m);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}

void Tablebases::rank_root_moves(Position& pos, Search::RootMoves& rootMoves) {

    RootInTB = false;
    UseRule50 = bool(Options["Syzygy50MoveRule"]);
    ProbeDepth = int(Options["SyzygyProbeDepth"]);
    Cardinality = int(Options["SyzygyProbeLimit"]);
    bool dtz_available = true;

    // Tables with fewer pieces than SyzygyProbeLimit are searched with
    // ProbeDepth == DEPTH_ZERO
    if (Cardinality > MaxCardinality)
    {
        Cardinality = MaxCardinality;
        ProbeDepth = 0;
    }

    if (Cardinality >= popcount(pos.pieces()) && !pos.can_castle(ANY_CASTLING))
    {
        // Rank moves using DTZ tables
        RootInTB = root_probe(pos, rootMoves);

        if (!RootInTB)
        {
            // DTZ tables are missing; try to rank moves using WDL tables
            dtz_available = false;
            RootInTB = root_probe_wdl(pos, rootMoves);
        }
    }

    if (RootInTB)
    {
        // Sort moves according to TB rank
        std::stable_sort(rootMoves.begin(), rootMoves.end(),
                  [](const RootMove &a, const RootMove &b) { return a.tbRank > b.tbRank; } );

        // Probe during search only if DTZ is not available and we are winning
        if (dtz_available || rootMoves[0].tbScore <= VALUE_DRAW)
            Cardinality = 0;
    }
    else
    {
        // Clean up if root_probe() and root_probe_wdl() have failed
        for (auto& m : rootMoves)
            m.tbRank = 0;
    }
}
#else     // Noir starts , Start with a small aspiration window and, in the case of a fail
          // high/low, re-search with a bigger window until we don't fail
          // high/low anymore.
          while (true)
          {
              bestValue = ::search<PV>(rootPos, ss, alpha, beta, rootDepth - searchAgainCounter, false);

              // Bring the best move to the front. It is critical that sorting
              // is done with a stable algorithm because all the values but the
              // first and eventually the new best one are set to -VALUE_INFINITE
              // and we want to keep the same order for all the moves except the
              // new PV that goes to the front. Note that in case of MultiPV
              // search the already searched PV lines are preserved.
              std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

              // If search has been stopped, we break immediately. Sorting is
              // safe because RootMoves is still valid, although it refers to
              // the previous iteration.
              if (Threads.stop)
                  break;

              // When failing high/low give some update (without cluttering
              // the UI) before a re-search.
              if (   mainThread
                  && multiPV == 1
                  && (bestValue <= alpha || bestValue >= beta)
                  && Time.elapsed() > 3000)
                  sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;

              // In case of failing low/high increase aspiration window and
              // re-search, otherwise exit the loop.
              if (bestValue <= alpha)
              {
                  beta = (alpha + beta) / 2;
                  alpha = std::max(bestValue - delta, -VALUE_INFINITE);

                  if (mainThread)
                      mainThread->stopOnPonderhit = false;
              }
              else if (bestValue >= beta)
                  beta = std::min(bestValue + delta, VALUE_INFINITE);

              else
                  break;

              delta += delta / 4 + 5;

              assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
          }

          // Sort the PV lines searched so far and update the GUI
          std::stable_sort(rootMoves.begin() + pvFirst, rootMoves.begin() + pvIdx + 1);

          if (    mainThread
              && (Threads.stop || pvIdx + 1 == multiPV || Time.elapsed() > 3000))
              sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;
      }

      if (!Threads.stop)
          completedDepth = rootDepth;

      if (rootMoves[0].pv[0] != lastBestMove) {
         lastBestMove = rootMoves[0].pv[0];
         lastBestMoveDepth = rootDepth;
      }

      // Have we found a "mate in x"?
      if (   Limits.mate
          && bestValue >= VALUE_MATE_IN_MAX_PLY
          && VALUE_MATE - bestValue <= 2 * Limits.mate)
          Threads.stop = true;

      if (!mainThread)
          continue;

      // Do we have time for the next iteration? Can we stop searching now?
      if (    Limits.use_time_management()
          && !Threads.stop
          && !mainThread->stopOnPonderhit)
      {
          double fallingEval = (318 + 6 * (mainThread->bestPreviousScore - bestValue)
                                    + 6 * (mainThread->iterValue[iterIdx] - bestValue)) / 825.0;
          fallingEval = std::clamp(fallingEval, 0.5, 1.5);

          // If the bestMove is stable over several iterations, reduce time accordingly
          timeReduction = lastBestMoveDepth + 9 < completedDepth ? 1.92 : 0.95;
          double reduction = (1.47 + mainThread->previousTimeReduction) / (2.32 * timeReduction);

          // Use part of the gained time from a previous stable move for the current move
          for (Thread* th : Threads)
          {
              totBestMoveChanges += th->bestMoveChanges;
              th->bestMoveChanges = 0;
          }
          double bestMoveInstability = 1 + 2 * totBestMoveChanges / Threads.size();

          TimePoint elapsedT = Time.elapsed();
          TimePoint optimumT = Time.optimum();

          // Stop the search if we have only one legal move, or if available time elapsed
          if (   (rootMoves.size() == 1 && (elapsedT > optimumT / 16))
              || elapsedT > optimumT * fallingEval * reduction * bestMoveInstability)
          {
              // If we are allowed to ponder do not stop the search now but
              // keep pondering until the GUI sends "ponderhit" or "stop".
              if (mainThread->ponder)
                  mainThread->stopOnPonderhit = true;
              else
                  Threads.stop = true;
          }
          else if (   Threads.increaseDepth
                   && !mainThread->ponder
                   && elapsedT > optimumT * fallingEval * reduction * bestMoveInstability * 0.58)
                   Threads.increaseDepth = false;
          else
                   Threads.increaseDepth = true;
      }

      mainThread->iterValue[iterIdx] = bestValue;
      iterIdx = (iterIdx + 1) & 3;
  }

  if (!mainThread)
      return;

  mainThread->previousTimeReduction = timeReduction;
}


namespace {

  // search<>() is the main search function for both PV and non-PV nodes

  template <NodeType NT>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {

    constexpr bool PvNode = NT == PV;
    const bool rootNode = PvNode && ss->ply == 0;

    // Dive into quiescence search when the depth reaches zero
    if (depth <= 0)
        return qsearch<NT>(pos, ss, alpha, beta);

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    Move pv[MAX_PLY+1], capturesSearched[32], quietsSearched[64];
    StateInfo st;
    TTEntry* tte;
    Key posKey;
    Move ttMove, move, excludedMove, bestMove;
    Depth extension, newDepth, ttDepth;
    Bound ttBound;
    Value bestValue, value, ttValue, eval, probCutBeta;
    bool formerPv, givesCheck, improving, didLMR, priorCapture, isMate, gameCycle;
    bool captureOrPromotion, doFullDepthSearch, moveCountPruning, ttCapture, singularQuietLMR, kingDanger;
    Piece movedPiece;
    int moveCount, captureCount, quietCount, rootDepth;

    // Step 1. Initialize node
    Thread* thisThread = pos.this_thread();
    ss->inCheck = pos.checkers();
    priorCapture = pos.captured_piece();
    Color us = pos.side_to_move();
    moveCount = captureCount = quietCount = ss->moveCount = 0;
    bestValue = -VALUE_INFINITE;
    gameCycle = kingDanger = false;
    rootDepth = thisThread->rootDepth;

    // Check for the available remaining time
    if (thisThread == Threads.main())
        static_cast<MainThread*>(thisThread)->check_time();

    //thisThread->nodes++;

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && thisThread->selDepth < ss->ply + 1)
        thisThread->selDepth = ss->ply + 1;

    // Transposition table lookup. We don't want the score of a partial
    // search to overwrite a previous full search TT value, so we use a different
    // position key in case of an excluded move.
    excludedMove = ss->excludedMove;
    posKey = excludedMove == MOVE_NONE ? pos.key() : pos.key() ^ make_key(excludedMove);
    tte = TT.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttDepth = tte->depth();
    ttBound = tte->bound();
    ttMove =  rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0]
            : ss->ttHit    ? tte->move() : MOVE_NONE;
    if (!excludedMove)
        ss->ttPv = PvNode || (ss->ttHit && tte->is_pv());
    formerPv = ss->ttPv && !PvNode;

    if (   ss->ttPv
        && depth > 12
        && ss->ply - 1 < MAX_LPH
        && !priorCapture
        && is_ok((ss-1)->currentMove))
        thisThread->lowPlyHistory[ss->ply - 1][from_to((ss-1)->currentMove)] << stat_bonus(depth - 5);

    // thisThread->ttHitAverage can be used to approximate the running average of ttHit
    thisThread->ttHitAverage =   (ttHitAverageWindow - 1) * thisThread->ttHitAverage / ttHitAverageWindow
                                + ttHitAverageResolution * ss->ttHit;

    if (!rootNode)
    {
        // Check if we have an upcoming move which draws by repetition, or
        // if the opponent had an alternative move earlier to this position.
        if (pos.has_game_cycle(ss->ply))
        {
            if (VALUE_DRAW >= beta)
            {
                tte->save(posKey, VALUE_DRAW, ss->ttPv, BOUND_UPPER,
                          depth, MOVE_NONE, VALUE_NONE);

                return VALUE_DRAW;
            }
            gameCycle = true;
            alpha = std::max(alpha, VALUE_DRAW);
        }

        // Step 2. Check for aborted search and immediate draw
        if (pos.is_draw(ss->ply))
            return VALUE_DRAW;

        if (Threads.stop.load(std::memory_order_relaxed) || ss->ply >= MAX_PLY)
            return ss->ply >= MAX_PLY && !ss->inCheck ? evaluate(pos)
                                                      : VALUE_DRAW;

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply+1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Same logic but with reversed
        // signs applies also in the opposite condition of being mated instead of giving
        // mate. In this case return a fail-high score.
        if (alpha >= mate_in(ss->ply+1))
            return alpha;
    }

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    (ss+1)->ply = ss->ply + 1;
    (ss+1)->ttPv = false;
    (ss+1)->excludedMove = bestMove = MOVE_NONE;
    (ss+2)->killers[0] = (ss+2)->killers[1] = MOVE_NONE;
    Square prevSq = to_sq((ss-1)->currentMove);

    // Initialize statScore to zero for the grandchildren of the current position.
    // So statScore is shared between all grandchildren and only the first grandchild
    // starts with statScore = 0. Later grandchildren start with the last calculated
    // statScore of the previous grandchild. This influences the reduction rules in
    // LMR which are based on the statScore of parent position.
    if (!rootNode)
        (ss+2)->statScore = 0;

    // At non-PV nodes we check for an early TT cutoff
    if (  !PvNode
        && ss->ttHit
        && !gameCycle
        && pos.rule50_count() < 88
        && ttDepth >= depth
        && ttValue != VALUE_NONE // Possible in case of TT access race
        && (ttValue != VALUE_DRAW || VALUE_DRAW >= beta)
        && (ttValue >= beta ? (ttBound & BOUND_LOWER)
                            : (ttBound & BOUND_UPPER)))
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit
        if (ttMove)
        {
            if (ttValue >= beta)
            {
                if (!pos.capture_or_promotion(ttMove))
                    update_quiet_stats(pos, ss, ttMove, stat_bonus(depth), depth);

                // Extra penalty for early quiet moves of the previous ply
                if ((ss-1)->moveCount <= 2 && !priorCapture)
                    update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + 1));
            }
            // Penalty for a quiet ttMove that fails low
            else if (!pos.capture_or_promotion(ttMove))
            {
                int penalty = -stat_bonus(depth);
                thisThread->mainHistory[us][from_to(ttMove)] << penalty;
                update_continuation_histories(ss, pos.moved_piece(ttMove), to_sq(ttMove), penalty);
            }
        }

        return ttValue;
    }

    // Step 5. Tablebases probe
    if (!rootNode && TB::Cardinality)
    {
        int piecesCount = popcount(pos.pieces());

        if (    piecesCount <= TB::Cardinality
            && (piecesCount <  TB::Cardinality || depth >= TB::ProbeDepth)
            &&  pos.rule50_count() == 0
            && !pos.can_castle(ANY_CASTLING))
        {
            TB::ProbeState err;
            TB::WDLScore v = Tablebases::probe_wdl(pos, &err);

            // Force check of time on the next occasion
            if (thisThread == Threads.main())
                static_cast<MainThread*>(thisThread)->callsCnt = 0;

            if (err != TB::ProbeState::FAIL)
            {
                thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);

                int drawScore = TB::UseRule50 ? 1 : 0;

                int centiPly = PawnValueEg * ss->ply / 100;

                Value tbValue =    v < -drawScore ? -VALUE_TB_WIN + centiPly + PawnValueEg * popcount(pos.pieces( pos.side_to_move()))
                                 : v >  drawScore ?  VALUE_TB_WIN - centiPly - PawnValueEg * popcount(pos.pieces(~pos.side_to_move()))
                                 : v < 0 ? -2 * Tempo : VALUE_DRAW;

                if (    abs(v) <= drawScore
                    || !ss->ttHit
                    || (v < -drawScore && beta  > tbValue + 19)
                    || (v >  drawScore && alpha < tbValue - 19))
                {
                    tte->save(posKey, tbValue, ss->ttPv, v > drawScore ? BOUND_LOWER : v < -drawScore ? BOUND_UPPER : BOUND_EXACT,
                              depth, MOVE_NONE, VALUE_NONE);

                    return tbValue;
                }
            }
        }
    }

    CapturePieceToHistory& captureHistory = thisThread->captureHistory;

    // Step 6. Static evaluation of the position
    if (ss->inCheck)
    {
        // Skip early pruning when in check
        ss->staticEval = eval = VALUE_NONE;
        improving = false;
    }
    else
    {
    if (ss->ttHit)
    {
        // Never assume anything about values stored in TT
        ss->staticEval = eval = tte->eval();
        if (eval == VALUE_NONE)
            ss->staticEval = eval = evaluate(pos);

        // Can ttValue be used as a better position evaluation?
        if (    ttValue != VALUE_NONE
            && (ttBound & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttValue;
    }
    else
    {
        if ((ss-1)->currentMove != MOVE_NULL)
            ss->staticEval = eval = evaluate(pos);
        else
            ss->staticEval = eval = -(ss-1)->staticEval + 2 * Tempo;
    }

    if (gameCycle)
        ss->staticEval = eval = eval * std::max(0, (100 - pos.rule50_count())) / 100;

    if (!ss->ttHit)
        tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_NONE, MOVE_NONE, eval);

    improving =  (ss-2)->staticEval == VALUE_NONE
               ? ss->staticEval > (ss-4)->staticEval || (ss-4)->staticEval == VALUE_NONE
               : ss->staticEval > (ss-2)->staticEval;

   // Update static history for previous move
   if (is_ok((ss-1)->currentMove) && !(ss-1)->inCheck && !priorCapture)
   {
       int bonus = ss->staticEval > -(ss-1)->staticEval + 2 * Tempo ? -stat_bonus(depth) :
                   ss->staticEval < -(ss-1)->staticEval + 2 * Tempo ? stat_bonus(depth) :
                   0;
       thisThread->staticHistory[~us][from_to((ss-1)->currentMove)] << bonus;
   }

    // Begin early pruning.
    if (   !PvNode
        && !excludedMove
        && !gameCycle
        && !thisThread->nmpGuard
        &&  abs(eval) < 2 * VALUE_KNOWN_WIN)
    {
       if (rootDepth > 10)
           kingDanger = pos.king_danger();

       // Step 8. Futility pruning: child node (~30 Elo)
       if (    depth < 8
           && !kingDanger
           &&  eval - futility_margin(depth, improving) >= beta
           &&  eval < VALUE_KNOWN_WIN) // Do not return unproven wins
           return eval;

       // Step 9. Null move search with verification search (~40 Elo)
       if (   (ss-1)->currentMove != MOVE_NULL
           && (ss-1)->statScore < 22977
           &&  eval >= beta
           &&  eval >= ss->staticEval
           &&  ss->staticEval >= beta - 30 * depth - 28 * improving + 84 * ss->ttPv + 168
           &&  pos.non_pawn_material(us)
           && !kingDanger
           && !(rootDepth > 10 && MoveList<LEGAL>(pos).size() < 6))
       {
           assert(eval - beta >= 0);

           // Null move dynamic reduction based on depth and value
           Depth R = (1015 + 85 * depth) / 256 + std::min(int(eval - beta) / 191, 3);

           if (   depth < 11
               || ttValue >= beta
               || ttDepth < depth-R
               || !(ttBound & BOUND_UPPER))
           {
           ss->currentMove = MOVE_NULL;
           ss->continuationHistory = &thisThread->continuationHistory[0][0][NO_PIECE][0];

           pos.do_null_move(st);

           Value nullValue = -search<NonPV>(pos, ss+1, -beta, -beta+1, depth-R, !cutNode);

           pos.undo_null_move();

           if (nullValue >= beta)
           {
               // Do not return unproven mate or TB scores
               nullValue = std::min(nullValue, VALUE_TB_WIN_IN_MAX_PLY);

               if (abs(beta) < VALUE_KNOWN_WIN && depth < 11 && beta <= qsearch<NonPV>(pos, ss, beta-1, beta))
                   return nullValue;

               // Do verification search at high depths
               thisThread->nmpGuard = true;

               Value v = search<NonPV>(pos, ss, beta-1, beta, depth-R, false);

               thisThread->nmpGuard = false;

               if (v >= beta)
                   return nullValue;
           }
           }
       }

        probCutBeta = beta + 183 - 49 * improving;

       // Step 10. ProbCut (~10 Elo)
       // If we have a good enough capture and a reduced search returns a value
       // much above beta, we can (almost) safely prune the previous move.
       if (    depth > 4
           &&  abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
           // if value from transposition table is lower than probCutBeta, don't attempt probCut
           // there and in further interactions with transposition table cutoff depth is set to depth - 3
           // because probCut search has depth set to depth - 4 but we also do a move before it
           // so effective depth is equal to depth - 3
           && (   !ss->ttHit
                || tte->depth() < depth - 3
                || ttValue == VALUE_NONE
                || ttValue >= probCutBeta))
       {
           // if ttMove is a capture and value from transposition table is good enough produce probCut
           // cutoff without digging into actual probCut search
           if (   ss->ttHit
               && tte->depth() >= depth - 3
               && ttValue != VALUE_NONE
               && ttValue >= probCutBeta
               && ttMove
               && pos.capture_or_promotion(ttMove))
               return probCutBeta;

           assert(probCutBeta < VALUE_INFINITE);
           MovePicker mp(pos, ttMove, probCutBeta - ss->staticEval, &captureHistory);
           int probCutCount = 0;
           bool ttPv = ss->ttPv;
           ss->ttPv = false;

           while (  (move = mp.next_move()) != MOVE_NONE
                  && probCutCount < 2 + 2 * cutNode)
               if (move != excludedMove)
               {
                   assert(pos.capture_or_promotion(move));
                   assert(depth >= 5);

                   captureOrPromotion = true;
                   probCutCount++;

                   ss->currentMove = move;
                   ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                             [captureOrPromotion]
                                                                             [pos.moved_piece(move)]
                                                                             [to_sq(move)];

                   pos.do_move(move, st);

                   // Perform a preliminary qsearch to verify that the move holds
                   value = -qsearch<NonPV>(pos, ss+1, -probCutBeta, -probCutBeta+1);

                   // If the qsearch held perform the regular search
                   if (value >= probCutBeta)
                       value = -search<NonPV>(pos, ss+1, -probCutBeta, -probCutBeta+1, depth - 4, !cutNode);

                   pos.undo_move(move);

                   if (value >= probCutBeta)
                   {
                       value = std::min(value, VALUE_TB_WIN_IN_MAX_PLY);

                       // if transposition table doesn't have equal or more deep info write probCut data into it
                       if (  !ss->ttHit
                           || tte->depth() < depth - 4
                           || ttValue == VALUE_NONE)
                           tte->save(posKey, value_to_tt(value, ss->ply), ttPv,
                                     BOUND_LOWER, depth - 4, move, ss->staticEval);

                       return value;
                   }
               }

           ss->ttPv = ttPv;
       }
    } // End early Pruning

    // Step 11. If the position is not in TT, decrease depth by 2
    if (   PvNode
        && depth >= 6
        && !ttMove)
        depth -= 2;

    } // In check search starts here

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr                   , (ss-4)->continuationHistory,
                                          nullptr                   , (ss-6)->continuationHistory };

    Move countermove = thisThread->counterMoves[pos.piece_on(prevSq)][prevSq];

    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &thisThread->staticHistory,
                                      &thisThread->lowPlyHistory,
                                      &captureHistory,
                                      contHist,
                                      countermove,
                                      ss->killers,
                                      ss->ply);

    value = bestValue;
    singularQuietLMR = moveCountPruning = false;
    ttCapture = ttMove && pos.capture_or_promotion(ttMove);

    // Mark this node as being searched
    ThreadHolding th(thisThread, posKey, ss->ply);

    // Step 12. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move(moveCountPruning)) != MOVE_NONE)
    {
      assert(is_ok(move));

      if (move == excludedMove)
          continue;

      // At root obey the "searchmoves" option and skip moves not listed in Root
      // Move List. As a consequence any illegal move is also skipped. In MultiPV
      // mode we also skip PV moves which have been already searched and those
      // of lower "TB rank" if we are in a TB root position.
      if (rootNode && !std::count(thisThread->rootMoves.begin() + thisThread->pvIdx,
                                  thisThread->rootMoves.begin() + thisThread->pvLast, move))
          continue;

      // Check for legality
      if (!rootNode && !pos.legal(move))
          continue;

      ss->moveCount = ++moveCount;

      if (!minOutput && rootNode && thisThread == Threads.main() && Time.elapsed() > 3000)
          sync_cout << "info depth " << depth
                    << " currmove " << UCI::move(move, pos.is_chess960())
                    << " currmovenumber " << moveCount + thisThread->pvIdx << sync_endl;
      if (PvNode)
          (ss+1)->pv = nullptr;

      extension = 0;
      captureOrPromotion = pos.capture_or_promotion(move);
      movedPiece = pos.moved_piece(move);
      givesCheck = pos.gives_check(move);
      isMate = false;

      if (givesCheck)
      {
          pos.do_move(move, st, givesCheck);
          isMate = MoveList<LEGAL>(pos).size() == 0;
          pos.undo_move(move);
      }

      if (isMate)
      {
          ss->currentMove = move;
          ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                    [captureOrPromotion]
                                                                    [movedPiece]
                                                                    [to_sq(move)];
          value = mate_in(ss->ply+1);

          if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
          {
              (ss+1)->pv = pv;
              (ss+1)->pv[0] = MOVE_NONE;
          }
      }
      else
      {

      // Calculate new depth for this move
      newDepth = depth - 1;

      // Step 13. Pruning at shallow depth (~200 Elo)
      if (  !PvNode
          && pos.non_pawn_material(us)
          && bestValue > VALUE_TB_LOSS_IN_MAX_PLY)
      {
          // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold
          moveCountPruning = moveCount >= futility_move_count(improving, depth);

          // Reduced depth of the next LMR search
          int lmrDepth = std::max(newDepth - reduction(improving, depth, moveCount), 0);

          if (   !captureOrPromotion
              && !givesCheck)
          {
              // Countermoves based pruning (~20 Elo)
              if (   lmrDepth < 4 + ((ss-1)->statScore > 0 || (ss-1)->moveCount == 1)
                  && (*contHist[0])[movedPiece][to_sq(move)] < CounterMovePruneThreshold
                  && (*contHist[1])[movedPiece][to_sq(move)] < CounterMovePruneThreshold)
                  continue;

              // Futility pruning: parent node (~5 Elo)
              if (   lmrDepth < 3
                  &&  bestValue > VALUE_TB_LOSS_IN_MAX_PLY
                  && ss->staticEval + 266 + 170 * lmrDepth <= alpha
                  &&  (*contHist[0])[movedPiece][to_sq(move)]
                    + (*contHist[1])[movedPiece][to_sq(move)]
                    + (*contHist[3])[movedPiece][to_sq(move)]
                    + (*contHist[5])[movedPiece][to_sq(move)] / 2 < 27376)
                  continue;

              // Prune moves with negative SEE (~20 Elo)
              if (!pos.see_ge(move, Value(-(30 - std::min(lmrDepth, 18)) * lmrDepth * lmrDepth)))
                  continue;
          }
          else
          {
              // Capture history based pruning when the move doesn't give check
              if (   !givesCheck
                  && lmrDepth < 1
                  && captureHistory[movedPiece][to_sq(move)][type_of(pos.piece_on(to_sq(move)))] < 0)
                  continue;

              // See based pruning
              if (!pos.see_ge(move, Value(-213) * depth)) // (~25 Elo)
                  continue;
          }
      }

      // Step 14. Extensions (~75 Elo)
      if (   gameCycle
          && (depth < 5 || PvNode))
          extension = 2;

      // Singular extension search (~70 Elo). If all moves but one fail low on a
      // search of (alpha-s, beta-s), and just one fails high on (alpha, beta),
      // then that move is singular and should be extended. To verify this we do
      // a reduced search on all the other moves but the ttMove and if the
      // result is lower than ttValue minus a margin then we will extend the ttMove.
      else if (    depth >= 7
          &&  move == ttMove
          && !gameCycle
          && !rootNode
          && !excludedMove // Avoid recursive singular search
          &&  ttValue != VALUE_NONE
          &&  abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
          && (tte->bound() & BOUND_LOWER)
          &&  tte->depth() >= depth - 3)
      {
          Value singularBeta = std::max(ttValue - ((formerPv + 4) * depth) / 2, VALUE_TB_LOSS_IN_MAX_PLY);
          Depth singularDepth = (depth - 1 + 3 * formerPv) / 2;

          ss->excludedMove = move;
          value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
          ss->excludedMove = MOVE_NONE;

          if (value < singularBeta)
          {
              extension = 1;
              singularQuietLMR = !ttCapture;
          }

          // Multi-cut pruning
          // Our ttMove is assumed to fail high, and now we failed high also on a reduced
          // search without the ttMove. So we assume this expected Cut-node is not singular,
          // that multiple moves fail high, and we can prune the whole subtree by returning
          // a soft bound.
          else if (!PvNode)
          {
            if (singularBeta >= beta)
                return std::min(singularBeta, VALUE_TB_WIN_IN_MAX_PLY);

           // If the eval of ttMove is greater than beta we try also if there is another
           // move that pushes it over beta, if so also produce a cutoff.
            else if (ttValue >= beta)
            {
                ss->excludedMove = move;
                value = search<NonPV>(pos, ss, beta - 1, beta, (depth + 3) / 2, cutNode);
                ss->excludedMove = MOVE_NONE;

                if (value >= beta)
                    return beta;
            }
          }
      }

      // Late irreversible move extension
      if (   move == ttMove
          && pos.rule50_count() > 80
          && (captureOrPromotion || type_of(movedPiece) == PAWN))
          extension = 2;

      // Check extension (~2 Elo)
      else if (!extension)
      {
        if (    givesCheck
            && (pos.is_discovered_check_on_king(~us, move) || pos.see_ge(move)))
            extension = 1;

        // Last captures extension
        else if (   PieceValue[EG][pos.captured_piece()] > PawnValueEg
                 && pos.non_pawn_material() <= 2 * RookValueMg)
            extension = 1;
      }

      // Add extension to new depth
      newDepth += extension;

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      // Update the current move (this must be done after singular extension search)
      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                [captureOrPromotion]
                                                                [movedPiece]
                                                                [to_sq(move)];

      // Step 15. Make the move
      pos.do_move(move, st, givesCheck);

      // Step 16. Reduced depth search (LMR, ~200 Elo). If the move fails high it will be
      // re-searched at full depth.
      if (    depth >= 3
          && !gameCycle
          &&  moveCount > 1 + 2 * rootNode
          &&  thisThread->selDepth > depth
          && (  !captureOrPromotion
              || moveCountPruning
              || ss->staticEval + PieceValue[EG][pos.captured_piece()] <= alpha
              || cutNode
              || thisThread->ttHitAverage < 432 * ttHitAverageResolution * ttHitAverageWindow / 1024))
      {
          Depth r = reduction(improving, depth, moveCount);

          // Decrease reduction if the ttHit running average is large
          if (thisThread->ttHitAverage > 537 * ttHitAverageResolution * ttHitAverageWindow / 1024)
              r--;

          // Increase reduction if other threads are searching this position
          if (th.marked())
              r++;

          // Decrease reduction if position is or has been on the PV (~10 Elo)
          if (ss->ttPv)
              r -= 2;

          // Increase reduction at root and non-PV nodes when the best move does not change frequently
          if ((rootNode || !PvNode) && depth > 10 && thisThread->bestMoveChanges <= 2)
              r++;

          if (rootDepth > 10 && pos.king_danger())
              r -= 1;

          if (moveCountPruning && !formerPv)
              r++;

          // Decrease reduction if opponent's move count is high (~5 Elo)
          if ((ss-1)->moveCount > 13)
              r--;

          // Decrease reduction if ttMove has been singularly extended (~3 Elo)
          if (singularQuietLMR)
              r--;

          if (!PvNode && !captureOrPromotion)
          {
              // Increase reduction if ttMove is a capture (~5 Elo)
              if (ttCapture)
                  r++;

              // Increase reduction for cut nodes (~10 Elo)
              if (cutNode)
                  r += 2;

              // Decrease reduction for moves that escape a capture. Filter out
              // castling moves, because they are coded as "king captures rook" and
              // hence break make_move(). (~2 Elo)
              else if (    type_of(move) == NORMAL
                       && !pos.see_ge(reverse_move(move)))
                  r -= 2 + ss->ttPv - (type_of(movedPiece) == PAWN);

              ss->statScore =  thisThread->mainHistory[us][from_to(move)]
                             + (*contHist[0])[movedPiece][to_sq(move)]
                             + (*contHist[1])[movedPiece][to_sq(move)]
                             + (*contHist[3])[movedPiece][to_sq(move)]
                             - 5287;

              // Decrease/increase reduction by comparing opponent's stat score (~10 Elo)
              if (ss->statScore >= -105 && (ss-1)->statScore < -103)
                  r--;

               else if ((ss-1)->statScore >= -122 && ss->statScore < -129)
                  r++;

              // Decrease/increase reduction for moves with a good/bad history (~30 Elo)
              r -= ss->statScore / 14884;
          }
          else
          {
              // Increase reduction for captures/promotions if late move and at low depth
              if (depth < 8 && moveCount > 2)
                  r++;

              // Unless giving check, this capture is likely bad
              if (   !givesCheck
                  && ss->staticEval + PieceValue[EG][pos.captured_piece()] + 210 * depth <= alpha)
                  r++;
          }

          Depth rr = newDepth / (2 + ss->ply / 3);

          r -= rr;

          Depth d = std::clamp(newDepth - r, 1, newDepth);

          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, d, true);

          doFullDepthSearch = value > alpha && d != newDepth;

          didLMR = true;
      }
      else
      {
          doFullDepthSearch = !PvNode || moveCount > 1;

          didLMR = false;
      }

      // Step 17. Full depth search when LMR is skipped or fails high
      if (doFullDepthSearch)
      {
          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, newDepth, !cutNode);

          if (didLMR && !captureOrPromotion)
          {
              int bonus = value > alpha ?  stat_bonus(newDepth)
                                        : -stat_bonus(newDepth);

              if (move == ss->killers[0])
                  bonus += bonus / 4;

              update_continuation_histories(ss, movedPiece, to_sq(move), bonus);
          }
      }

      // For PV nodes only, do a full PV search on the first move or after a fail
      // high (in the latter case search only if value < beta), otherwise let the
      // parent node fail low with value <= alpha and try another move.
      if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
      {
          (ss+1)->pv = pv;
          (ss+1)->pv[0] = MOVE_NONE;

          value = -search<PV>(pos, ss+1, -beta, -alpha, newDepth, false);
      }

      // Step 18. Undo move
      pos.undo_move(move);
      }

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Step 19. Check for a new best move
      // Finished searching the move. If a stop occurred, the return value of
      // the search cannot be trusted, and we return immediately without
      // updating best move, PV and TT.
      if (Threads.stop.load(std::memory_order_relaxed))
          return VALUE_ZERO;

      if (rootNode)
      {
          RootMove& rm = *std::find(thisThread->rootMoves.begin(),
                                    thisThread->rootMoves.end(), move);

          // PV move or new best move?
          if (moveCount == 1 || value > alpha)
          {
              rm.score = value;
              rm.selDepth = thisThread->selDepth;
              rm.pv.resize(1);

              assert((ss+1)->pv);

              for (Move* m = (ss+1)->pv; *m != MOVE_NONE; ++m)
                  rm.pv.push_back(*m);

              // We record how often the best move has been changed in each
              // iteration. This information is used for time management: when
              // the best move changes frequently, we allocate some more time.
              if (moveCount > 1)
                  ++thisThread->bestMoveChanges;
          }
          else
              // All other moves but the PV are set to the lowest value: this
              // is not a problem when sorting because the sort is stable and the
              // move position in the list is preserved - just the PV is pushed up.
              rm.score = -VALUE_INFINITE;
      }

      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
              bestMove = move;

              if (PvNode && !rootNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha! Always alpha < beta
                  alpha = value;
              else
              {
                  assert(value >= beta); // Fail high
                  ss->statScore = 0;
                  break;
              }
          }
      }

      if (move != bestMove)
      {
          if (captureOrPromotion && captureCount < 32)
              capturesSearched[captureCount++] = move;

          else if (!captureOrPromotion && quietCount < 64)
              quietsSearched[quietCount++] = move;
      }
    }

    // The following condition would detect a stop only after move loop has been
    // completed. But in this case bestValue is valid because we have fully
    // searched our subtree, and we can anyhow save the result in TT.
    /*
       if (Threads.stop)
        return VALUE_DRAW;
    */

    // Step 20. Check for mate and stalemate
    // All legal moves have been searched and if there are no legal moves, it
    // must be a mate or a stalemate. If we are in a singular extension search then
    // return a fail low score.

    assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    if (!moveCount)
        bestValue = excludedMove ? alpha
                   :     ss->inCheck ? mated_in(ss->ply) : VALUE_DRAW;

    else if (bestMove)
        update_all_stats(pos, ss, bestMove, bestValue, beta, prevSq,
                         quietsSearched, quietCount, capturesSearched, captureCount, depth);

    // Bonus for prior countermove that caused the fail low
    else if (   (depth >= 3 || PvNode)
             && !priorCapture)
        update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, stat_bonus(depth));

    // If no good move is found and the previous position was ttPv, then the previous
    // opponent move is probably good and the new position is added to the search tree.
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || ((ss-1)->ttPv && depth > 3);

    // Otherwise, a counter move has been found and if the position is the last leaf
    // in the search tree, remove the position from the search tree.
    else if (depth > 3)
        ss->ttPv = ss->ttPv && (ss+1)->ttPv;

    if (!excludedMove && !(rootNode && thisThread->pvIdx))
        tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                  bestValue >= beta ? BOUND_LOWER :
                  PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
                  depth, bestMove, ss->staticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // qsearch() is the quiescence search function, which is called by the main search
  // function with zero depth, or recursively with further decreasing depth per call.
  template <NodeType NT>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {

    constexpr bool PvNode = NT == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(depth <= 0);

    Move pv[MAX_PLY+1];
    StateInfo st;
    TTEntry* tte;
    Key posKey;
    Move ttMove, move, bestMove;
    Depth ttDepth;
    Value bestValue, value, ttValue, futilityValue, futilityBase, oldAlpha;
    bool pvHit, givesCheck, captureOrPromotion, gameCycle;
    int moveCount;

    if (PvNode)
    {
        oldAlpha = alpha; // To flag BOUND_EXACT when eval above alpha and no available moves
        (ss+1)->pv = pv;
        ss->pv[0] = MOVE_NONE;
    }

    Thread* thisThread = pos.this_thread();
    (ss+1)->ply = ss->ply + 1;
    bestMove = MOVE_NONE;
    ss->inCheck = pos.checkers();
    moveCount = 0;
    gameCycle = false;

    if (pos.has_game_cycle(ss->ply))
    {
       if (VALUE_DRAW >= beta)
           return VALUE_DRAW;

       alpha = std::max(alpha, VALUE_DRAW);
       gameCycle = true;
    }

    if (pos.is_draw(ss->ply))
        return VALUE_DRAW;

    // Check for an immediate draw or maximum ply reached
    if (ss->ply >= MAX_PLY)
        return !ss->inCheck ? evaluate(pos) : VALUE_DRAW;

    if (alpha >= mate_in(ss->ply+1))
        return alpha;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Decide whether or not to include checks: this fixes also the type of
    // TT entry depth that we are going to use. Note that in qsearch we use
    // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
    ttDepth = ss->inCheck || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS
                                                  : DEPTH_QS_NO_CHECKS;
    // Transposition table lookup
    posKey = pos.key();
    tte = TT.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttMove = ss->ttHit ? tte->move() : MOVE_NONE;
    pvHit = ss->ttHit && tte->is_pv();

    if (  !PvNode
        && ss->ttHit
        && !gameCycle
        && pos.rule50_count() < 88
        && tte->depth() >= ttDepth
        && ttValue != VALUE_NONE // Only in case of TT access race
        && (ttValue != VALUE_DRAW || VALUE_DRAW >= beta)
        && (ttValue >= beta ? (tte->bound() & BOUND_LOWER)
                            : (tte->bound() & BOUND_UPPER)))
        return ttValue;

    // Evaluate the position statically
    if (ss->inCheck)
    {
        ss->staticEval = VALUE_NONE;
        bestValue = futilityBase = -VALUE_INFINITE;
    }
    else
    {
        if (ss->ttHit)
        {
            // Never assume anything about values stored in TT
            if ((ss->staticEval = bestValue = tte->eval()) == VALUE_NONE)
                ss->staticEval = bestValue = evaluate(pos);

            // Can ttValue be used as a better position evaluation?
            if (    ttValue != VALUE_NONE
                && (tte->bound() & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttValue;
        }
        else
            ss->staticEval = bestValue =
            (ss-1)->currentMove != MOVE_NULL ? evaluate(pos)
                                             : -(ss-1)->staticEval + 2 * Tempo;

        if (gameCycle)
            ss->staticEval = bestValue = bestValue * std::max(0, (100 - pos.rule50_count())) / 100;

        // Stand pat. Return immediately if static value is at least beta
        if (bestValue >= beta)
        {
            if (!ss->ttHit)
                tte->save(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER,
                          DEPTH_NONE, MOVE_NONE, ss->staticEval);

            return bestValue;
        }

        if (PvNode && bestValue > alpha)
            alpha = bestValue;

        futilityBase = bestValue + 155;
    }

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr                   , (ss-4)->continuationHistory,
                                          nullptr                   , (ss-6)->continuationHistory };

    // Initialize a MovePicker object for the current position, and prepare
    // to search the moves. Because the depth is <= 0 here, only captures,
    // queen and checking knight promotions, and other checks(only if depth >= DEPTH_QS_CHECKS)
    // will be generated.
    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &thisThread->staticHistory,
                                      &thisThread->captureHistory,
                                      contHist,
                                      to_sq((ss-1)->currentMove));

    // Loop through the moves until no moves remain or a beta cutoff occurs
    while ((move = mp.next_move()) != MOVE_NONE)
    {
      assert(is_ok(move));

      givesCheck = pos.gives_check(move);
      captureOrPromotion = pos.capture_or_promotion(move);

      moveCount++;

      if (!PvNode)
      {
         // Futility pruning
         if (   !ss->inCheck
             && !givesCheck
             &&  futilityBase > -VALUE_KNOWN_WIN
             && !pos.advanced_pawn_push(move))
         {
             assert(type_of(move) != ENPASSANT); // Due to !pos.advanced_pawn_push

             // moveCount pruning
             if (moveCount > 2)
                 continue;

             futilityValue = futilityBase + PieceValue[EG][pos.piece_on(to_sq(move))];

             if (futilityValue <= alpha)
             {
                 bestValue = std::max(bestValue, futilityValue);
                 continue;
             }

             if (futilityBase <= alpha && !pos.see_ge(move, VALUE_ZERO + 1))
             {
                 bestValue = std::max(bestValue, futilityBase);
                 continue;
             }
         }

         // Do not search moves with negative SEE values
         if (    bestValue > VALUE_TB_LOSS_IN_MAX_PLY
             && !(givesCheck && pos.is_discovered_check_on_king(~pos.side_to_move(), move))
             && !pos.see_ge(move))
             continue;
      }

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                [captureOrPromotion]
                                                                [pos.moved_piece(move)]
                                                                [to_sq(move)];

      // CounterMove based pruning
      if (  !captureOrPromotion
          && moveCount
          && (*contHist[0])[pos.moved_piece(move)][to_sq(move)] < CounterMovePruneThreshold
          && (*contHist[1])[pos.moved_piece(move)][to_sq(move)] < CounterMovePruneThreshold)
          continue;

      // Make and search the move
      pos.do_move(move, st, givesCheck);
      value = -qsearch<NT>(pos, ss+1, -beta, -alpha, depth - 1);
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Check for a new best move
      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
              bestMove = move;

              if (PvNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha here!
                  alpha = value;
              else
                  break; // Fail high
          }
       }
    }

    // All legal moves have been searched. A special case: if we're in check
    // and no legal moves were found, it is checkmate.
    if (ss->inCheck && bestValue == -VALUE_INFINITE)
        return mated_in(ss->ply); // Plies to mate from the root

    tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit,
              bestValue >= beta ? BOUND_LOWER :
              PvNode && bestValue > oldAlpha  ? BOUND_EXACT : BOUND_UPPER,
              ttDepth, bestMove, ss->staticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // value_to_tt() adjusts a mate or TB score from "plies to mate from the root" to
  // "plies to mate from the current position". Standard scores are unchanged.
  // The function is called before storing a value in the transposition table.

  Value value_to_tt(Value v, int ply) {

    assert(v != VALUE_NONE);

    return  v >= VALUE_TB_WIN_IN_MAX_PLY  ? v + ply
          : v <= VALUE_TB_LOSS_IN_MAX_PLY ? v - ply : v;
  }


  // value_from_tt() is the inverse of value_to_tt(): it adjusts a mate or TB score
  // from the transposition table (which refers to the plies to mate/be mated from
  // current position) to "plies to mate/be mated (TB win/loss) from the root". However,
  // for mate scores, to avoid potentially false mate scores related to the 50 moves rule
  // and the graph history interaction, we return an optimal TB score instead.

  Value value_from_tt(Value v, int ply, int r50c) {

    /*return  v == VALUE_NONE             ? VALUE_NONE
          : v >= VALUE_MATE_IN_MAX_PLY  ? v - ply
          : v <= VALUE_MATED_IN_MAX_PLY ? v + ply : v; */

    if (v == VALUE_NONE)
        return VALUE_NONE;

    if (v >= VALUE_TB_WIN_IN_MAX_PLY)  // TB win or better
    {
        if (v >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - v > 99 - r50c)
            return VALUE_MATE_IN_MAX_PLY - 1; // do not return a potentially false mate score

        return v - ply;
    }

    if (v <= VALUE_TB_LOSS_IN_MAX_PLY) // TB loss or worse
    {
        if (v <= VALUE_MATED_IN_MAX_PLY && VALUE_MATE + v > 99 - r50c)
            return VALUE_MATED_IN_MAX_PLY + 1; // do not return a potentially false mate score

        return v + ply;
    }

    return v;

  }


  // update_pv() adds current move and appends child pv[]

  void update_pv(Move* pv, Move move, Move* childPv) {

    for (*pv++ = move; childPv && *childPv != MOVE_NONE; )
        *pv++ = *childPv++;
    *pv = MOVE_NONE;
  }


  // update_all_stats() updates stats at the end of search() when a bestMove is found

  void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, Square prevSq,
                        Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth) {

    int bonus1, bonus2;
    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    CapturePieceToHistory& captureHistory = thisThread->captureHistory;
    Piece moved_piece = pos.moved_piece(bestMove);
    PieceType captured = type_of(pos.piece_on(to_sq(bestMove)));

    bonus1 = stat_bonus(depth + 1);
    bonus2 = bestValue > beta + PawnValueMg ? bonus1               // larger bonus
                                            : stat_bonus(depth);   // smaller bonus

    if (!pos.capture_or_promotion(bestMove))
    {
        update_quiet_stats(pos, ss, bestMove, bonus2, depth);

        // Decrease all the non-best quiet moves
        for (int i = 0; i < quietCount; ++i)
        {
            thisThread->mainHistory[us][from_to(quietsSearched[i])] << -bonus2;
            update_continuation_histories(ss, pos.moved_piece(quietsSearched[i]), to_sq(quietsSearched[i]), -bonus2);
        }
    }
    else
        captureHistory[moved_piece][to_sq(bestMove)][captured] << bonus1;

    // Extra penalty for a quiet early move that was not a TT move or main killer move in previous ply when it gets refuted
    if (   ((ss-1)->moveCount == 1 + (ss-1)->ttHit || ((ss-1)->currentMove == (ss-1)->killers[0]))
        && !pos.captured_piece())
            update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -bonus1);

    // Decrease all the non-best capture moves
    for (int i = 0; i < captureCount; ++i)
    {
        moved_piece = pos.moved_piece(capturesSearched[i]);
        captured = type_of(pos.piece_on(to_sq(capturesSearched[i])));
        captureHistory[moved_piece][to_sq(capturesSearched[i])][captured] << -bonus1;
    }
  }


  // update_continuation_histories() updates histories of the move pairs formed
  // by moves at ply -1, -2, -4, and -6 with current move.

  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {

    for (int i : {1, 2, 4, 6})
    {
        if (ss->inCheck && i > 2)
            break;
        if (is_ok((ss-i)->currentMove))
            (*(ss-i)->continuationHistory)[pc][to] << bonus;
    }
  }


  // update_quiet_stats() updates move sorting heuristics

  void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus, int depth) {

    if (ss->killers[0] != move)
    {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = move;
    }

    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    thisThread->mainHistory[us][from_to(move)] << bonus;
    update_continuation_histories(ss, pos.moved_piece(move), to_sq(move), bonus);

    if (type_of(pos.moved_piece(move)) != PAWN)
        thisThread->mainHistory[us][from_to(reverse_move(move))] << -bonus;

    if (is_ok((ss-1)->currentMove))
    {
        Square prevSq = to_sq((ss-1)->currentMove);
        thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] = move;
    }

    if (depth > 11 && ss->ply < MAX_LPH)
        thisThread->lowPlyHistory[ss->ply][from_to(move)] << stat_bonus(depth - 7);
  }

} // namespace


/// MainThread::check_time() is used to print debug info and, more importantly,
/// to detect when we are out of available time and thus stop the search.

void MainThread::check_time() {

  if (--callsCnt > 0)
      return;

  // When using nodes, ensure checking rate is not lower than 0.1% of nodes
  callsCnt = Limits.nodes ? std::min(1024, int(Limits.nodes / 1024)) : 1024;

  static TimePoint tick = now();

  TimePoint elapsed = Time.elapsed();
  TimePoint tock = Limits.startTime + elapsed;
  Thread* bestThread = this;
  if (elapsed > 500)
  {
    if (elapsed <= 120050)
    {
      if (tock - tick >= 10000 && minOutput)
      {
        tick = tock;
        sync_cout  << "\n" << "info " << elapsed/1000 << " seconds"  << sync_endl;
        sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE) << sync_endl;
        //dbg_print();
      }
    }
    else if (elapsed <= 600100)
    {
      if (tock - tick >= 60000 && minOutput)
      {
        tick = tock;
        sync_cout  << "\n" << "info " << elapsed/60000 << " minutes" << sync_endl;
        sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE)  << sync_endl;
        //dbg_print();
      }
    }
    else
    {
      if (tock - tick >= 300000 && minOutput)
      {
        tick = tock;
        sync_cout  << "\n" << "info " << elapsed/60000 << " minutes" << sync_endl ;
        sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE)  << sync_endl;
        //dbg_print();
      }
    }
  }
  // We should not stop pondering until told so by the GUI
  if (ponder)
      return;

  if (   (Limits.use_time_management() && (elapsed > Time.maximum() - 10 || stopOnPonderhit))
      || (Limits.movetime && elapsed >= Limits.movetime)
      || (Limits.nodes && Threads.nodes_searched() >= (uint64_t)Limits.nodes))
      Threads.stop = true;
}


/// UCI::pv() formats PV information according to the UCI protocol. UCI requires
/// that all (if any) unsearched PV lines are sent using a previous search score.

string UCI::pv(const Position& pos, Depth depth, Value alpha, Value beta) {

  std::stringstream ss;
  TimePoint elapsed = Time.elapsed() + 1;
  const RootMoves& rootMoves = pos.this_thread()->rootMoves;
  size_t pvIdx = pos.this_thread()->pvIdx;
  size_t multiPV = std::min((size_t)Options["MultiPV"], rootMoves.size());
  uint64_t nodesSearched = Threads.nodes_searched();
  uint64_t tbHits = Threads.tb_hits() + (TB::RootInTB ? rootMoves.size() : 0);

  for (size_t i = 0; i < multiPV; ++i)
  {
      bool updated = rootMoves[i].score != -VALUE_INFINITE;

      if (depth == 1 && !updated && i > 0)
          continue;

      Depth d = updated ? depth : std::max(1, depth - 1);
      Value v = updated ? rootMoves[i].score : rootMoves[i].previousScore;
      Value v2 = rootMoves[i].previousScore;

      if (v == -VALUE_INFINITE)
          v = VALUE_ZERO;

      bool tb = TB::RootInTB && abs(v) < VALUE_TB_WIN - 6 * PawnValueEg;

      v = tb ? rootMoves[i].tbScore : v;

      if (ss.rdbuf()->in_avail()) // Not at first line
          ss << "\n";

      ss << "info"
         << " depth "    << d
         << " seldepth " << rootMoves[i].selDepth
         << " multipv "  << i + 1
         << " score "    << UCI::value(v, v2);

      if (Options["UCI_ShowWDL"])
          ss << UCI::wdl(v, pos.game_ply());

      if (!tb && i == pvIdx)
          ss << (v >= beta ? " lowerbound" : v <= alpha ? " upperbound" : "");

      ss << " nodes "    << nodesSearched
         << " nps "      << nodesSearched * 1000 / elapsed;

      if (elapsed > 1000) // Earlier makes little sense
          ss << " hashfull " << TT.hashfull();

      ss << " tbhits "   << tbHits
         << " time "     << elapsed
         << " pv";

      for (Move m : rootMoves[i].pv)
          ss << " " << UCI::move(m, pos.is_chess960());
  }

  return ss.str();
}


/// RootMove::extract_ponder_from_tt() is called in case we have no ponder move
/// before exiting the search, for instance, in case we stop the search during a
/// fail high at root. We try hard to have a ponder move to return to the GUI,
/// otherwise in case of 'ponder on' we have nothing to think on.

bool RootMove::extract_ponder_from_tt(Position& pos) {

    StateInfo st;
    bool ttHit;

    assert(pv.size() == 1);

    if (pv[0] == MOVE_NONE)
        return false;

    pos.do_move(pv[0], st);
    TTEntry* tte = TT.probe(pos.key(), ttHit);

    if (ttHit)
    {
        Move m = tte->move(); // Local copy to be SMP safe
        if (MoveList<LEGAL>(pos).contains(m))
            pv.push_back(m);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}

void Tablebases::rank_root_moves(Position& pos, Search::RootMoves& rootMoves) {

    RootInTB = false;
    UseRule50 = bool(Options["Syzygy50MoveRule"]);
    ProbeDepth = int(Options["SyzygyProbeDepth"]);
    Cardinality = int(Options["SyzygyProbeLimit"]);

    // Tables with fewer pieces than SyzygyProbeLimit are searched with
    // ProbeDepth == DEPTH_ZERO
    if (Cardinality > MaxCardinality)
    {
        Cardinality = MaxCardinality;
        ProbeDepth = 0;
    }

    if (Cardinality >= popcount(pos.pieces()) && !pos.can_castle(ANY_CASTLING))
    {
        // Rank moves using DTZ tables
        RootInTB = root_probe(pos, rootMoves);

        if (!RootInTB)
        {
            // DTZ tables are missing; try to rank moves using WDL tables
            RootInTB = root_probe_wdl(pos, rootMoves);
        }
    }

    if (RootInTB)
    {
        // Sort moves according to TB rank
        std::stable_sort(rootMoves.begin(), rootMoves.end(),
                  [](const RootMove &a, const RootMove &b) { return a.tbRank > b.tbRank; } );
    }
    else
    {
        // Clean up if root_probe() and root_probe_wdl() have failed
        for (auto& m : rootMoves)
            m.tbRank = 0;
    }
}
#endif  //Noir - not Noir starts at row 779, Noir starts at row 2490