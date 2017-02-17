///////////////////////////////////////////////////////////////////////////////
//                   SOFTWARE COPYRIGHT NOTICE AGREEMENT                     //
//       This software and its documentation are copyright (2014) by the     //
//   Broad Institute.  All rights are reserved.  This software is supplied   //
//   without any warranty or guaranteed support whatsoever. The Broad        //
//   Institute is not responsible for its use, misuse, or functionality.     //
///////////////////////////////////////////////////////////////////////////////

// MakeDepend: library OMP
// MakeDepend: cflags OMP_FLAGS

#include "Basevector.h"
#include "CoreTools.h"
#include "FetchReads.h"
#include "Intvector.h"
#include "IteratorRange.h"
#include "PairsManager.h"
#include "ParallelVecUtilities.h"
#include "Qualvector.h"
#include "TokenizeString.h"
#include "efasta/EfastaTools.h"
#include "feudal/HashSet.h"
#include "graph/DigraphTemplate.h"
#include "kmers/KMerHasher.h"
#include "paths/HyperBasevector.h"
#include "paths/ReadsToPathsCoreX.h"
#include "paths/RemodelGapTools.h"
#include "paths/Unipath.h"
//#include "paths/long/EvalAssembly.h"
#include "paths/long/Heuristics.h"
#include "paths/long/LoadCorrectCore.h"
#include "paths/long/Logging.h"
#include "paths/long/LongHyper.h"
#include "paths/long/LongProtoTools.h"
#include "paths/long/MakeKmerStuff.h"
#include "paths/long/ReadPath.h"
#include "paths/long/RefTrace.h"
#include "paths/long/SupportedHyperBasevector.h"
#include "paths/long/large/GapToyTools.h"
#include "random/Random.h"
#include <fstream>
#include <ctime>

PerfStatLogger PerfStatLogger::gInst;

namespace
{

class Dotter
{
public:
    Dotter( HyperBasevector const& hbv, unsigned adjacencyDepth )
    : mHBV(hbv), mAdjacencyDepth(adjacencyDepth)
    { hbv.ToLeft(mFromVtx); hbv.ToRight(mToVtx); }

    void add( int edgeId )
    { mEdgeMap[edgeId];
      mIncompleteVertexSet.insert(mFromVtx[edgeId]);
      mIncompleteVertexSet.insert(mToVtx[edgeId]); }


private:

    struct EdgeStatus
    {
        EdgeStatus()
        : mUsed(false), mIsAdjacent(false) {}

        void setAdjacent( bool isAdjacent = true ) { mIsAdjacent = isAdjacent; }
        bool isAdjacent() const { return mIsAdjacent; }

        void setUsed( bool used = true ) { mUsed = used; }
        bool isUsed() const { return mUsed; }

        bool mUsed;
        bool mIsAdjacent;
    };


    typedef std::pair<int const,EdgeStatus> MapEntry;



    HyperBasevector const& mHBV;
    vec<int> mFromVtx;
    vec<int> mToVtx;
    unsigned mAdjacencyDepth;
    std::map<int,EdgeStatus> mEdgeMap;
    std::set<int> mIncompleteVertexSet;
};


}; // end of anonymous namespace




void BasesToGraph( vecbasevector& bpathsx, const int K, HyperBasevector& hb )
{    HyperKmerPath h;
     vecKmerPath paths, paths_rc, unipaths;
     vec<big_tagged_rpint> pathsdb, unipathsdb;
     ReadsToPathsCoreY( bpathsx, K, paths );
     CreateDatabase( paths, paths_rc, pathsdb );
     Unipath( paths, paths_rc, pathsdb, unipaths, unipathsdb );
     digraph A; 
     BuildUnipathAdjacencyGraph( paths, paths_rc, pathsdb, unipaths,
          unipathsdb, A );
     BuildUnipathAdjacencyHyperKmerPath( K, A, unipaths, h );
     KmerBaseBrokerBig kbb( K, paths, paths_rc, pathsdb, bpathsx );
     hb = HyperBasevector( h, kbb );    }

void MakeLocalAssembly2(VecEFasta &corrected,
                        const vec<int> &lefts, const vec<int> &rights,
                        SupportedHyperBasevector &shb, const int K2_FLOOR,
                        vecbasevector &creads, vec<int> &cid,
                        vec<pairing_info> &cpartner) {
    long_logging logc("", "");
    logc.STATUS_LOGGING = False;
    logc.MIN_LOGGING = False;
    ref_data ref;
    vec<ref_loc> readlocs;
    long_logging_control log_control(ref, &readlocs);
    long_heuristics heur("");
    heur.K2_FLOOR = K2_FLOOR;
    int count = 0;
    for (int l = 0; l < (int) corrected.size(); l++)
        if (corrected[l].size() > 0) count++;
    if (count == 0) {
        //mout << "No reads were corrected." << std::endl;
    } else {
        if (!LongHyper(corrected, cpartner, shb, heur, log_control, logc, False)) {
            //mout << "No paths were found." << std::endl;
            SupportedHyperBasevector shb0;
            shb = shb0;
        } else {
            // heur.LC_CAREFUL = True;
            shb.DeleteLowCoverage(heur, log_control, logc);
            if (shb.NPaths() == 0) {
                //mout << "No paths were found." << std::endl;
                SupportedHyperBasevector shb0;
                shb = shb0;
            } //TODO bj, check this is really not needed!
            // else shb.TestValid(logc);
        }
    }
    /*mout << "using K2 = " << shb.K( ) << "\n";
    mout << "local assembly has " << shb.EdgeObjectCount( ) << " edges" << "\n";
    mout << "assembly time 2 = " << TimeSince(clock) << std::endl;*/
}

void LogTime( const double clock, const String& what, const String& work_dir )
{    static String dir;
     if ( work_dir != "" ) dir = work_dir;
     else if ( dir != "" )
     {    Echo( TimeSince(clock) + " used " + what, dir + "/clock.log" );    }    }

void CleanupCore( HyperBasevector& hb, vec<int>& inv, ReadPathVec& paths )
{
     vec<Bool> used;
     hb.Used(used);
     vec<int> to_new_id( used.size( ), -1 );
     {    int count = 0;
          for ( int i = 0; i < used.isize( ); i++ )
               if ( used[i] ) to_new_id[i] = count++;
     }

     vec<int> inv2;
     for ( int i = 0; i < hb.EdgeObjectCount( ); i++ )
     {    if ( !used[i] ) continue;
          if ( inv[i] < 0 ) inv2.push_back(-1);
          else inv2.push_back( to_new_id[ inv[i] ] );
     }
     inv = inv2;

     vec<Bool> to_delete( paths.size( ), False );

     #pragma omp parallel for
     for ( int64_t i = 0; i < (int64_t) paths.size( ); i++ )
     {
          std::vector<int>& p = paths[i];
          for ( int j = 0; j < (int) p.size( ); j++ )
          {    int n = to_new_id[ p[j] ];
               if ( n < 0 ) to_delete[i] = True;
               else p[j] = n;
          }
     }

     hb.RemoveDeadEdgeObjects( );

     hb.RemoveEdgelessVertices( );

     
     }

void Cleanup( HyperBasevector& hb, vec<int>& inv, ReadPathVec& paths )
{    
     //XXX: truncates paths, which should be already done by the graph-modifying functions
    {
        vec<Bool> used;
        hb.Used(used);
        for (int64_t i = 0; i < (int64_t) paths.size(); i++) {
            for (int64_t j = 0; j < (int64_t) paths[i].size(); j++) {
                if (paths[i][j] < 0 || paths[i][j] >= hb.EdgeObjectCount() || !used[paths[i][j]]) {
                    paths[i].resize(j);
                    break;
                }
            }
        }
    }
    RemoveUnneededVertices2( hb, inv, paths );
    CleanupCore( hb, inv, paths );
}

void CleanupLoops( HyperBasevector& hb, vec<int>& inv, ReadPathVec& paths )
{    RemoveUnneededVerticesLoopsOnly( hb, inv, paths );
     CleanupCore( hb, inv, paths );    }
