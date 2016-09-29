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

const int MaxIter = 10000;

bool EndsWith( vec<int> const& whole, vec<int> const& e )
{
     if ( whole.size() < e.size() ) return false;
     return std::equal( e.begin(), e.end(), whole.end() - e.size() );
}

bool BeginsWith( vec<int> const& whole, vec<int> const& b )
{
     if ( whole.size() < b.size() ) return false;
     return std::equal( b.begin(), b.end(), whole.begin() );
}

void TailHeadOverlap( vec<int> const& x1, vec<int> const& x2, vec<int>& overlap )
{
     overlap.clear();
     if ( x1.size() == 0 || x2.size() == 0 ) return;

     vec<int> s1(x1), s2(x2);
     UniqueSort(s1);
     UniqueSort(s2);
     if (!Meet(s1,s2)) return;

     int i = x1.size()-1;
     int j = 0;

     // find head of x2 in x1
     for ( ; i >=0; --i )
          if ( x1[i] == x2[j] ) break;

     // now walk forward in x1 and x2 pushing back matches
     for ( ; i >= 0 && i < x1.isize() && j < x2.isize(); ++i, ++j )
          if ( x1[i] == x2[j] ) overlap.push_back( x1[i] );
          else break;

     if ( i < x1.isize() )
          overlap.clear();      // we didn't make it back to the end

     if ( overlap.size() > 1 ) {
          overlap.clear();
     }
}

typedef enum { AC_NOCYCLE, AC_CYCLE, AC_MAXITER } AcReason;

bool AcyclicSubgraphWithLimits( HyperBasevector const& hbv,
          vec<int> const& edge_path,
          vec<int> const& to_left, vec<int> const& to_right,
          const int max_bases, int max_iter = MaxIter,
          const bool verbose = false, AcReason *reasonp = 0 )
{
     ForceAssertGt(edge_path.size(), 0u);
     int K = hbv.K();

     if ( verbose ) std::cout << "edge_path = " << printSeq(edge_path) << std::endl;

     vec<bool> seen(hbv.N(), false);
     vec<bool> special(hbv.N(), false);



     // first validate overlap path
     for ( size_t i = 1; i < edge_path.size(); ++i )
          if ( to_left[edge_path[i]] != to_right[edge_path[i-1]] )
               FatalErr("edge_path is not a path");

     // now mark overlap path as seen
     int len = hbv.K()-1;
     for ( auto e : edge_path ) {
          len += hbv.EdgeLengthKmers(e);
          seen[to_left[e]] = special[to_left[e]] = true;
     }

     // now explore outward from the end
     vec<triple<int,int,int>> stack;
     stack.push( to_right[edge_path.back()], len, edge_path.size() );
     while ( stack.size() ) {
          // we return false if it takes more than max_iter iterations to
          // figure this out
          if ( max_iter-- <= 0 ) {
              if ( reasonp ) *reasonp = AC_MAXITER;
              return false;
          }

          auto rec = stack.back();
          stack.pop_back();
          int rec_v = rec.first;
          int rec_len = rec.second;
          int rec_depth = rec.third;

          if ( verbose ) PRINT3_TO(std::cout, rec_v, rec_len, rec_depth );

          // we *ignore* parts of the graph that are more than max_bases away
          if ( rec_len > max_bases ) continue;

/*
          // We return false if we are able to drift more than rec_depth from
          // our starting point *without* violating the previous rule about length.
          // This is a threshold set for computational reasons -- we return false
          // because we simply can't determine if it's true without exceeding
          // our traversal limit
          if ( rec_depth > max_depth ) return false;
*/

          if ( verbose ) PRINT2_TO(std::cout, seen[rec_v], special[rec_v] );

          // now we now check for a cycle
          if ( seen[ rec_v ] ) {
               // if the cycle hits a vertex in the path, that's bad
               if ( special[ rec_v ] ) {
                   if ( reasonp ) *reasonp = AC_CYCLE;
                   return false;
               }
               // if not, who cares
               else continue;
          }

          seen[rec_v] = true;

          for ( size_t i = 0; i < hbv.From(rec_v).size(); ++i ) {
               int v = hbv.From(rec_v)[i];
               int e = hbv.FromEdgeObj(rec_v)[i];
               stack.push( v, rec_len+hbv.EdgeLengthKmers(e), rec_depth+1 );
          }
     }

     if ( reasonp ) *reasonp = AC_NOCYCLE;
     return true;
}

template <unsigned K>
class IndirectKmer
{
public:
    struct Hasher
    {
        typedef IndirectKmer const& argument_type;
        size_t operator()( IndirectKmer const& kmer ) const
        { return kmer.getHash(); }
    };
    typedef HashSet<IndirectKmer,IndirectKmer::Hasher> Dictionary;

    IndirectKmer() = default;
    IndirectKmer( bvec const* pBV, unsigned offset, size_t hash )
    : mpBV(pBV), mOffset(offset), mHash(hash) {}

    bvec::const_iterator getBases() const { return mpBV->begin(mOffset); }
    size_t getId( bvec const* pBV ) const { return mpBV-pBV; }
    size_t getHash() const { return mHash; }

    static void kmerize( bvec const& gvec, Dictionary* pDict )
    { if ( gvec.size() < K ) return;
      auto end = gvec.end()-K+1;
      auto itr = gvec.begin();
      BuzHasher<K> h;
      uint64_t hVal = h.hash(itr);
      // note: using insertUniqueValue may result in two equal keys being
      // present, but we're actually exploiting that as a feature.
      // (see BizarreComparator, below)
      pDict->insertUniqueValue(IndirectKmer(&gvec,itr.pos(),hVal));
      while ( ++itr != end )
      { hVal = h.step(itr,hVal);
        pDict->insertUniqueValue(IndirectKmer(&gvec,itr.pos(),hVal)); } }

    static bool matches( Dictionary const& dict, bvec const& edge )
    { if ( edge.size() < K ) return false;
      auto end = edge.end()-K+1;
      auto itr = edge.begin();
      BuzHasher<K> h;
      uint64_t hVal = h.hash(itr);
      if ( dict.lookup(IndirectKmer(&edge,itr.pos(),hVal)) ) return true;
      while ( ++itr != end )
      { hVal = h.step(itr,hVal);
        if ( dict.lookup(IndirectKmer(&edge,itr.pos(),hVal)) ) return true; }
      return false; }

    friend bool operator==( IndirectKmer const& k1, IndirectKmer const& k2 )
    { if ( k1.getHash() != k2.getHash() ) return false;
      auto itr = k1.getBases();
      return std::equal(itr,itr+K,k2.getBases()); }

    // Always fails, but accumulates a set of ids associated with the
    // IndirectKmers for some hash value, that are, in fact, equivalent.
    // In other words, it's useful only for an odd side-effect.
    class BizarreComparator
    {
    public:
        BizarreComparator( bvec const* pBV0 ) : mpBV0(pBV0) {}

        bool operator()( IndirectKmer const& k1, IndirectKmer const& k2 ) const
        { if ( k1.getHash() == k2.getHash() )
          { auto itr = k1.getBases();
            if ( std::equal(itr,itr+K,k2.getBases()) )
              mIds.insert(k2.getId(mpBV0)); }
          return false; }

        std::set<size_t> const& getIds() const { return mIds; }

    private:
        bvec const* mpBV0;
        unsigned mK;
        mutable std::set<size_t> mIds;
    };

    static void findMatches( Dictionary const& dict, bvec const* pG0,
                                bvec const& edge, ULongVec& invKeys )
    { if ( edge.size() < K ) return;
      BizarreComparator comp(pG0);
      auto end = edge.end()-K+1;
      auto itr = edge.begin();
      BuzHasher<K> h;
      uint64_t hVal = h.hash(itr);
      dict.lookup(hVal,IndirectKmer(&edge,itr.pos(),hVal),comp);
      while ( ++itr != end )
      { hVal = h.step(itr,hVal);
        dict.lookup(hVal,IndirectKmer(&edge,itr.pos(),hVal),comp); }
      std::set<size_t> const& keys = comp.getIds();
      invKeys.assign(keys.begin(),keys.end()); }

private:
    bvec const* mpBV;
    unsigned mOffset;
    size_t mHash;
};

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

    void write( std::ostream& os, String const& title )
    { expandAdjacent();
      cleanComplete();
      writeHeader(os);
      writeEdges(os);
      writeFooter(os,title);
      mEdgeMap.clear(); }

private:
    class EdgeAttrs
    {
    public:
        EdgeAttrs( int edgeId, unsigned size, bool isAdjacent )
        : mEdgeId(edgeId), mSize(size), mIsAdjacent(isAdjacent) {}

        friend std::ostream& operator<<( std::ostream& os, EdgeAttrs const& edgeAttrs )
        { os << "label=\"" << ToString(edgeAttrs.mEdgeId);
          if ( edgeAttrs.mSize < 250u )
            os << "\",color=gray,minlen=1";
          else if ( edgeAttrs.mSize < 1000u )
            os << "\",color=black,minlen=2";
          else if ( edgeAttrs.mSize < 10000u )
            os << ' ' << edgeAttrs.sizeString() << "\",color=red,minlen=4";
          else
            os << ' ' << edgeAttrs.sizeString() << "\",color=magenta,minlen=8";
          if ( edgeAttrs.mIsAdjacent )
            os << ",style=dotted";
          return os; }

    private:
        String sizeString() const
        { return ToString(mSize/1000.,1)+"k"; }

        int mEdgeId;
        unsigned mSize;
        bool mIsAdjacent;
    };

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

    void expandAdjacent()
    {
        unsigned depth = mAdjacencyDepth;
        std::set<int> exploredVertexSet;
        while ( depth-- && !mIncompleteVertexSet.empty() )
        {
            exploredVertexSet.insert(mIncompleteVertexSet.begin(),
                                        mIncompleteVertexSet.end());
            std::set<int> newVertexSet;
            for ( int vtxId : mIncompleteVertexSet )
            {
                for ( int edgeId : mHBV.FromEdgeObj(vtxId) )
                {
                    if ( !mEdgeMap.count(edgeId) )
                    {
                        mEdgeMap[edgeId].setAdjacent(true);
                        if ( !exploredVertexSet.count(mFromVtx[edgeId]) )
                            newVertexSet.insert(mFromVtx[edgeId]);
                        if ( !exploredVertexSet.count(mToVtx[edgeId]) )
                            newVertexSet.insert(mToVtx[edgeId]);
                    }
                }
                for ( int edgeId : mHBV.ToEdgeObj(vtxId) )
                {
                    if ( !mEdgeMap.count(edgeId) )
                    {
                        mEdgeMap[edgeId].setAdjacent(true);
                        if ( !exploredVertexSet.count(mFromVtx[edgeId]) )
                            newVertexSet.insert(mFromVtx[edgeId]);
                        if ( !exploredVertexSet.count(mToVtx[edgeId]) )
                            newVertexSet.insert(mToVtx[edgeId]);
                    }
                }
            }
            using std::swap;
            swap(mIncompleteVertexSet,newVertexSet);
        }
    }

    void cleanComplete()
    {
        std::set<int> newVertexSet;
        for ( int vtxId : mIncompleteVertexSet )
        {
            bool incomplete = false;
            for ( int edgeId : mHBV.FromEdgeObj(vtxId) )
            {
                if ( !mEdgeMap.count(edgeId) )
                {
                    incomplete = true;
                    break;
                }
            }
            if ( !incomplete )
            {
                for ( int edgeId : mHBV.ToEdgeObj(vtxId) )
                {
                    if ( !mEdgeMap.count(edgeId) )
                    {
                        incomplete = true;
                        break;
                    }
                }
            }
            if ( incomplete )
                newVertexSet.insert(vtxId);
        }
        using std::swap;
        swap(mIncompleteVertexSet,newVertexSet);
    }

    void writeHeader( std::ostream& dot )
    { dot << "digraph G {\n";
      dot << "node [width=0.1,height=0.1,shape=point];\n";
      dot << "margin=1.0;\n";
      dot << "rankdir=LR;\n";
      dot << "labeljust=l;\n";
      dot << "color=white;\n"; }

    typedef std::pair<int const,EdgeStatus> MapEntry;

    void writeEdges( std::ostream& dot )
    { int componentId = 0;
      for ( MapEntry& entry : mEdgeMap )
      { if ( entry.second.isUsed() ) continue;
        dot << "subgraph cluster_" << componentId++ << " {\n";
        writeComponentEdges(dot,entry);
        dot << "}\n"; } }

    void writeFooter( std::ostream& dot, String const& title )
    { dot << "label=\"" << title << "\";\n";
      dot << "labelloc=top;\n";
      dot << "}\n"; }

    void writeComponentEdges( std::ostream& dot, MapEntry& entry )
    { entry.second.setUsed();

      int edgeId = entry.first;
      int v1 = mFromVtx[edgeId];
      int v2 = mToVtx[edgeId];

      if ( mIncompleteVertexSet.count(v1) )
      { dot << v1 << " [color=green]\n";
        mIncompleteVertexSet.erase(v1); }
      if ( mIncompleteVertexSet.count(v2) )
      { dot << v2 << " [color=green]\n";
        mIncompleteVertexSet.erase(v2); }

      int sz = mHBV.EdgeObject(edgeId).size();
      bool isAdjacent = entry.second.isAdjacent();
      dot << ToString(v1) << "->" << ToString(v2)
            << '[' << EdgeAttrs(edgeId,sz,isAdjacent) << "];\n";

      auto end = mEdgeMap.end();
      for ( int edgeId : mHBV.FromEdgeObj(v1) )
      { auto itr = mEdgeMap.find(edgeId);
        if ( itr != end && !itr->second.isUsed() )
          writeComponentEdges(dot,*itr); }
      for ( int edgeId : mHBV.ToEdgeObj(v1) )
      { auto itr = mEdgeMap.find(edgeId);
        if ( itr != end && !itr->second.isUsed() )
          writeComponentEdges(dot,*itr); }

      for ( int edgeId : mHBV.FromEdgeObj(v2) )
      { auto itr = mEdgeMap.find(edgeId);
        if ( itr != end && !itr->second.isUsed() )
          writeComponentEdges(dot,*itr); }
      for ( int edgeId : mHBV.ToEdgeObj(v2) )
      { auto itr = mEdgeMap.find(edgeId);
        if ( itr != end && !itr->second.isUsed() )
          writeComponentEdges(dot,*itr); } }

    HyperBasevector const& mHBV;
    vec<int> mFromVtx;
    vec<int> mToVtx;
    unsigned mAdjacencyDepth;
    std::map<int,EdgeStatus> mEdgeMap;
    std::set<int> mIncompleteVertexSet;
};

void writeDot( bvec const& gv, String const& gvName, vec<int> const& keys,
                HyperBasevector const& hbv, String const& file )
{
#if 0
    // this way is slower
    unsigned const K = 200;
    if ( gv.size() < K ) return;
    vec<bvec> const& edges = hbv.Edges();
    if ( edges.empty() ) return;

    bvec const* pEV0 = &edges.front();
    IndirectKmer<K>::Dictionary dict(gv.size()-K+1);
    IndirectKmer<K>::kmerize(gv,&dict);

    unsigned const ADJACENCY_EXPLORATION_DEPTH = 1;
    Dotter dotter(hbv,ADJACENCY_EXPLORATION_DEPTH);
    for ( bvec const& ev : edges )
    {
        if ( IndirectKmer<K>::matches(dict,ev) )
        {
            int edgeId = &ev-pEV0;
            dotter.add(edgeId);
        }
    }
#else
    // this is faster
    if ( keys.empty() ) return;

    unsigned const ADJACENCY_EXPLORATION_DEPTH = 1;
    Dotter dotter(hbv,ADJACENCY_EXPLORATION_DEPTH);
    for ( int edgeId : keys )
        dotter.add(edgeId);
#endif
    std::ofstream dot(file);
    dotter.write(dot,gvName);
    dot.close();
}

}; // end of anonymous namespace

/*
void JoinPathsAndHackReads(vec<int>& inv, String const& in_head, ReadPathVec& paths, HyperBasevector& hbv,
          bool test )
{
     const int max_pair_sep = 1000;

     vecbvec reads( in_head + ".fastb" );
     vecqvec quals( in_head + ".qualb" );


     for ( int64_t i = 0; i < (int64_t) paths.size( ); i += 2 ) {
          ReadPath &p1 = paths[i], &p2 = paths[i+1];
          vec<int> x1, x2;
          for ( int j = 0; j < (int) p1.size( ); j++ )
               x1.push_back( p1[j] );
          for ( int j = 0; j < (int) p2.size( ); j++ )
               x2.push_back( inv[ p2[j] ] );
          x2.ReverseMe( );
          if ( x1 != x2 && !x1.Contains(x2) && !x2.Contains(x1) ) {
               vec<int> overlap;
               TailHeadOverlap( x1, x2, overlap );
               bool neil_test = false;
               if ( overlap.size() == 1 &&
                         AcyclicSubgraphWithLimits( hbv, overlap, max_pair_sep, MaxDepth )  ) {
                    std::cout << "[" << i << "] " << printSeq(x1) << ".." <<
                              printSeq(x2) << "  (" << printSeq(overlap) << ")" << std::endl;
                    ForceAssertEq(overlap.size(), 1u);
                    int overlap_bases = hbv.EdgeLengthBases( overlap.front() ) - p2.getLastSkip() - p1.getLastSkip();
                    basevector center;
                    qualvector qcenter;
                    if ( overlap_bases < 0  ) {
                         std::cout << "negative overlap" << std::endl;
                         center.SetToSubOf(hbv.EdgeObject(overlap.front()), hbv.EdgeObject(overlap.front()).size() - p1.getLastSkip(), -overlap_bases);
                         qcenter.resize( -overlap_bases, 0 );
                    }
                    basevector con1 = reads[i];
                    con1.append( center.begin(), center.end() );
                    con1.append( reads[i+1].rcbegin()+std::max(overlap_bases,0), reads[i+1].rcend() );
                    reads[i] = con1;
                    reads[i+1] = con1;
                    reads[i+1].ReverseComplement();
                    qualvector qcon1 = quals[i];
                    qcon1.append( qcenter.begin(), qcenter.end() );
                    qcon1.append( quals[i+1].rbegin() + std::max(overlap_bases,0), quals[i+1].rend() );
                    quals[i] = qcon1;
                    quals[i+1].ReverseMe();
                    int offset = p1.getOffset();
                    for ( int j = overlap.size(); j < (int) x2.size( ); j++ )
                         p1.push_back( x2[j] );
                    p1.setLastSkip(p2.getFirstSkip());
                    p2.resize(0);
                    for ( size_t i = p1.size(); i > 0; --i )
                         p2.push_back( inv[p1[i-1]] );
                    p2.setLastSkip( p1.getFirstSkip() );
                    neil_test = true;
               }
               if ( test ) {
                    vec<int> s1(x1), s2(x2);
                    UniqueSort(s1), UniqueSort(s2);
                    bool dj_test = Meet(s1,s2) && x1.back() == x2.front();
                    if ( neil_test != dj_test ) {
                         std::cout << "==================== DEBUGGING ====================" << std::endl;
                         std::cout << "neil_test = " << (neil_test ? "T":"F") << std::endl;
                         std::cout << "dj_test = " << (dj_test ? "T":"F") << std::endl;
                         std::cout << "[" << i << "] " << printSeq(x1) << ".." << printSeq(x2) << std::endl;
                         std::cout << "overlap = " << printSeq(overlap) << std::endl;
                    }
               }
          }
     }

     SystemSucceed( "/bin/mv " + in_head + ".fastb " + in_head + ".orig.fastb" );
     SystemSucceed( "/bin/mv " + in_head + ".qualb " + in_head + ".orig.qualb" );
     reads.WriteAll( in_head + ".fastb" );
     quals.WriteAll( in_head + ".qualb" );
}
*/

void JoinPaths0( vec<int> const& inv, ReadPathVec& paths )
{
     for ( int64_t i = 0; i < (int64_t) paths.size( ); i += 2 )
     {    ReadPath &p1 = paths[i], &p2 = paths[i+1];
          vec<int> x1, x2;
          for ( int j = 0; j < (int) p1.size( ); j++ )
               x1.push_back( p1[j] );
          for ( int j = 0; j < (int) p2.size( ); j++ )
               x2.push_back( inv[ p2[j] ] );
          x2.ReverseMe( );
          vec<int> s1(x1), s2(x2);
          UniqueSort(s1), UniqueSort(s2);
          if ( Meet( s1, s2 ) && x1 != x2 && !x1.Contains(x2) && !x2.Contains(x1) )
          {    std::cout << "[" << i << "] " << printSeq(x1) << ".." << printSeq(x2)
                    << std::endl;
               if ( x1.back( ) == x2.front( ) )
               {    for ( int j = 1; j < (int) x2.size( ); j++ )
                         p1.push_back( x2[j] );
                    p2.resize(0);    }    }    }
}


void JoinPaths(vec<int> const& inv, ReadPathVec& paths, HyperBasevector const& hbv,
          bool test )
{
     const int max_pair_sep = 1000;

     vec<int> to_left, to_right;
     hbv.ToLeft(to_left);
     hbv.ToRight(to_right);

     int percent = 0;
     size_t cycles = 0, totals = 0, maxiter = 0;
#pragma omp parallel for schedule(dynamic,1) reduction(+:cycles,totals,maxiter)
     for ( int64_t i = 0; i < (int64_t) paths.size( ); i += 2 ) {
          if ( !test && omp_get_thread_num() == 0 ) {
               if ( i * 100 / ((int64_t) paths.size()-1) > percent ) {
                    ++percent;
                    if ( percent % 10 == 0 ) std::cout << ". ";
                    else std::cout << ".";
                    if ( percent % 50 == 0 ) std::cout << std::endl;
               }
          }
          ReadPath &p1 = paths[i], &p2 = paths[i+1];
          vec<int> x1, x2;
          for ( int j = 0; j < (int) p1.size( ); j++ )
               x1.push_back( p1[j] );
          for ( int j = 0; j < (int) p2.size( ); j++ )
               x2.push_back( inv[ p2[j] ] );
          x2.ReverseMe( );
          if ( x1 != x2 && !x1.Contains(x2) && !x2.Contains(x1) ) {
               vec<int> overlap;
               TailHeadOverlap( x1, x2, overlap );
               bool neil_test = false;
               AcReason type;
               if ( overlap.size() ) {
                   totals++;
                   if ( AcyclicSubgraphWithLimits( hbv, overlap, to_left, to_right, max_pair_sep, MaxIter, false, &type )  ) {
                       if ( test ) {
#pragma omp critical
                           std::cout << "[" << i << "] " << printSeq(x1) << ".." <<
                                   printSeq(x2) << "  (" << printSeq(overlap) << ")" << std::endl;
                       }
                       for ( int j = overlap.size(); j < (int) x2.size( ); j++ )
                           p1.push_back( x2[j] );
                       p2.resize(0);
                       neil_test = true;
                   }
                   else if ( type == AC_MAXITER )
                       maxiter++;
                   else if ( type == AC_CYCLE )
                       cycles++;
               }
               if ( test ) {
                    vec<int> s1(x1), s2(x2);
                    UniqueSort(s1), UniqueSort(s2);
                    bool dj_test = Meet(s1,s2) && x1.back() == x2.front();
                    bool cycle_test = false;
                    if ( overlap.size() ) cycle_test = AcyclicSubgraphWithLimits( hbv, overlap, to_left, to_right, max_pair_sep, MaxIter );
#pragma omp critical
                    if ( neil_test != dj_test ) {
                         std::cout << "==================== DEBUGGING ====================" << std::endl;
                         std::cout << "neil_test = " << (neil_test ? "T":"F") << std::endl;
                         std::cout << "dj_test = " << (dj_test ? "T":"F") << std::endl;
                         std::cout << "cycle_test = " << (cycle_test ? "T":"F") << std::endl;
                         std::cout << "[" << i << "] " << printSeq(x1) << ".." << printSeq(x2) << std::endl;
                         std::cout << "overlap = " << printSeq(overlap) << std::endl;
                    }
               }
          }
     }
     if ( !test ) {
          // kludgy catch-up -- we may not get to 100 depending on
          // how threads are balanced
          while ( percent++ < 100 ) { std::cout << "."; }
          std::cout << std::endl;
     }
     std::cout << Date() << ": JoinPaths summary totals=" << totals << ", cycles=" << cycles << ", maxiter=" <<  maxiter << std::endl;
}

void FixPaths(HyperBasevector const& hbv, ReadPathVec& paths)
{
     vec<int> to_right, to_left;
     hbv.ToRight(to_right);
     hbv.ToLeft(to_left);
     #pragma omp parallel for
     for ( int64_t m = 0; m < (int64_t) paths.size( ); m++ )
     {    ReadPath& p = paths[m];
          for ( int i = 0; i < ( (int) p.size( ) ) - 1; i++ )
          {    int e1 = p[i], e2 = p[i+1];
               if ( to_right[e1] != to_left[e2] )
               {    p.resize(i+1);
                    break;    }    }    }
}


void DumpBPaths( vec<basevector> const& bp, int lroot,
          int rroot, String const& head )
{
     std::ostringstream s;
     s << head << "." << lroot << "." << rroot << ".fasta";
     Ofstream( bpaths_out, s.str() );
     size_t serial = 0;
     for ( auto const& bv : bp ) {
          bpaths_out << ">" << serial++ << std::endl;
          bv.PrintCol(bpaths_out,80);
     }
}

void Dot( const int nblobs, int& nprocessed, int& dots_printed, 
     const Bool ANNOUNCE, const int bl )
{
     #pragma omp critical
     {    nprocessed++;
          double done_percent = 100.0 * double(nprocessed) / double(nblobs);
          while ( done_percent >= dots_printed+1)
          {    if ( dots_printed % 10 == 0 && dots_printed > 0 
                    && dots_printed != 50 )
               {    std::cout << " ";    }
                    std::cout << ".";
               dots_printed++;
               if ( dots_printed % 50 == 0 ) std::cout << std::endl;    
               flush(std::cout);    }    }
     if (ANNOUNCE)
     {
          #pragma omp critical
          {    std::cout << "\n" << Date( ) << ": STOP " << bl << std::endl;    }    }    }

void FixInversion( const HyperBasevector& hb, vec<int>& inv2 )
{    double clock1 = WallClockTime( );
     inv2.resize_and_set( hb.EdgeObjectCount( ), -1 );
     const int K = 200;
     ForceAssertEq( K, hb.K( ) );
     vec<Bool> used;
     hb.Used(used);
     vec<int> ids;
     for ( int e = 0; e < hb.EdgeObjectCount( ); e++ )
          if ( used[e] ) ids.push_back(e);
     vec< triple<kmer<K>,int,int> > uni2( ids.size( ) * 2 );
     #pragma omp parallel for
     for ( int i = 0; i < ids.isize( ); i++ )
     {    int e = ids[i];
          kmer<K> x;
          x.SetToSubOf( hb.EdgeObject(e), 0 );
          uni2[2*i] = make_triple( x, 0, e );
          basevector b = hb.EdgeObject(e);
          b.ReverseComplement( );
          x.SetToSubOf( b, 0 );
          uni2[2*i+1] = make_triple( x, 1, e );    }
     std::cout << TimeSince(clock1) << " used fixing inversion 1" << std::endl;
     double clock2 = WallClockTime( );
     ParallelSort(uni2);
     std::cout << TimeSince(clock2) << " used fixing inversion 2" << std::endl;
     double clock3 = WallClockTime( );
     const int blocks = 100;
     vec<int> starts( blocks + 1 );
     for ( int j = 0; j <= blocks; j++ )
          starts[j] = ( j * uni2.jsize( ) ) / blocks;
     for ( int j = blocks - 1; j >= 1; j-- )
     {    while( starts[j] > 0 && uni2[ starts[j] ].first
               == uni2[ starts[j] - 1 ].first )
          {    starts[j]--;    }    }
     #pragma omp parallel for
     for ( int b = 0; b < blocks; b++ )
     {    for ( int i = starts[b]; i < starts[b+1]; i++ )
          {    int j;
               for ( j = i + 1; j < starts[b+1]; j++ )
                    if ( uni2[j].first != uni2[i].first ) break;
               if ( j - i == 2 && uni2[i].second != uni2[i+1].second )
               {    int e1 = uni2[i].third, e2 = uni2[i+1].third;
                    basevector b1 = hb.EdgeObject(e1);
                    b1.ReverseComplement( );
                    if ( b1 == hb.EdgeObject(e2) )
                    {    inv2[ uni2[i].third ] = uni2[i+1].third;
                         inv2[ uni2[i+1].third ] = uni2[i].third;    }    }
                    else if ( j - i > 2 )
               {    vec< triple<basevector,int,int> > U(j-i);
                    for ( int k = i; k < j; k++ )
                    {    int e = uni2[k].third;
                         U[k-i].first = hb.EdgeObject(e);
                         if ( uni2[k].second == 1 ) 
                              U[k-i].first.ReverseComplement( );
                         U[k-i].second = uni2[k].second, U[k-i].third = e;    }
                    Sort(U);
                    for ( int l = 0; l < U.isize( ); l++ )
                    {    int m;
                         for ( m = l + 1; m < U.isize( ); m++ )
                              if ( U[m].first != U[l].first ) break;
                         if ( m - l == 2 && U[l].second != U[l+1].second )
                         {    inv2[ U[l].third ] = U[l+1].third;
                              inv2[ U[l+1].third ] = U[l].third;    }
                         l = m - 1;    }    }
               i = j - 1;    }    }
     std::cout << TimeSince(clock3) << " used fixing inversion 3" << std::endl;    }

void InsertPatch( HyperBasevector& hb, vec<int>& to_left, 
     vec<int>& to_right, const HyperBasevector& hbp, 
     const int lroot, const int rroot, const int left, const int right,
     vec<Bool>& used )
{    hb.EdgeObjectMutable(lroot) = hbp.EdgeObject(left);
     hb.EdgeObjectMutable(rroot) = hbp.EdgeObject(right);
     int M = hb.N( ), N = hbp.N( );
     hb.AddVertices( N - 2 );
     vec<int> to_leftx, to_rightx;
     hbp.ToLeft(to_leftx), hbp.ToRight(to_rightx);
     vec<int> ends = { to_rightx[left], to_leftx[right] };
     for ( int l = 0; l < hbp.EdgeObjectCount( ); l++ )
     {    if ( l == left || l == right ) continue;
          int v, w;
          if ( to_leftx[l] == to_rightx[left] ) v = to_right[lroot];
          else 
          {    v = M + to_leftx[l] - ( to_leftx[l] > ends[0] ? 1 : 0 )
                    - ( to_leftx[l] > ends[1] ? 1 : 0 );    }
          if ( to_rightx[l] == to_leftx[right] ) w = to_left[rroot];
          else 
          {    w = M + to_rightx[l] - ( to_rightx[l] > ends[0] ? 1 : 0 )
                    - ( to_rightx[l] > ends[1] ? 1 : 0 );    }
          hb.AddEdge( v, w, hbp.EdgeObject(l) );    
          used.push_back(True);    }    }

void EdgesMatchingGenome( const HyperBasevector& hb, const vecbasevector& G,
     vec< vec<int> >& keys )
{
    //double startTime = WallClockTime();
    keys.clear();
    keys.resize(G.size());
    vec<bvec> const& edges = hb.Edges();
    if ( G.empty() || edges.empty() )
        return;

    // build a dictionary of the kmers that we seek
    unsigned const K = 200;
    IndirectKmer<K>::Dictionary dict(G.getKmerCount(K));
    parallelFor(0ul,G.size(),
        [&dict,&G]( size_t id )
        { IndirectKmer<K>::kmerize(G[id],&dict); });

    bvec const* pG0 = &G.front();
    size_t nEdges = edges.size();
    VecULongVec invKeyVec(nEdges);
    parallelForBatch(0ul,edges.size(),100,
        [&dict,pG0,&edges,&invKeyVec]( size_t edgeId )
        { IndirectKmer<K>::findMatches(dict,pG0,edges[edgeId],invKeyVec[edgeId]); });

    for ( size_t edgeId = 0; edgeId != nEdges; ++edgeId )
    {
        ULongVec const& invKeys = invKeyVec[edgeId];
        for ( size_t gId : invKeys )
            keys[gId].push_back(edgeId);
    }

    //std::cout << "EdgesMatchingGenome takes " << TimeSince(startTime) << std::std::endl;
}

void PrintDotMatchingGenome( const HyperBasevector& hbv, const vecbasevector& G,
     vecString const& Gnames, const String& work_dir )
{
    double clock = WallClockTime();
    vec< vec<int> > keys;
    EdgesMatchingGenome( hbv, G, keys );
    Mkdir777( work_dir + "/fos" );
    parallelFor(0ul,G.size(),
      [&hbv,&G,&Gnames,&keys,&work_dir]( size_t gId )
      { writeDot(G[gId],Gnames[gId],keys[gId],
                  hbv,work_dir+"/fos/"+Gnames[gId]+".dot"); });
    LogTime( clock, "in PrintDotMatchingGenome" );    }



void DefinePairs( const ReadPathVec& paths, const vec<int>& inv,
     vec< std::pair<vec<int>,vec<int>> >& pairs, vec<int64_t>& pairs_pid, 
     const String& dir )
{    std::cout << "in DefinePairs, memory in use = " 
          << MemUsageBytes( )  << std::endl;
     pairs.reserve( paths.size( ) );
     pairs_pid.reserve( paths.size( ) );
     for ( int pass = 1; pass <= 2; pass++ )
     {    for ( int64_t pid = 0; pid < (int64_t) paths.size( )/2; pid++ )
          {    vec<int> x, y;
               for ( int64_t j = 0; j < (int64_t) paths[2*pid].size( ); j++ )
                    x.push_back( paths[2*pid][j] );
               for ( int64_t j = 0; j < (int64_t) paths[2*pid+1].size( ); j++ )
                    y.push_back( paths[2*pid+1][j] );
               y.ReverseMe( );
               for ( int j = 0; j < y.isize( ); j++ )
                    y[j] = inv[ y[j] ];
               if ( pass == 2 )
               {    swap( x, y );
                    x.ReverseMe( ), y.ReverseMe( );
                    for ( int j = 0; j < x.isize( ); j++ )
                         x[j] = inv[ x[j] ];
                    for ( int j = 0; j < y.isize( ); j++ )
                         y[j] = inv[ y[j] ];    }
               pairs.push( x, y );    
               pairs_pid.push_back( pid /* pass == 1 ? pid : -pid-1 */ );    }    }
     std::cout << Date( ) << ": syncing" << std::endl;
     ParallelSortSync( pairs, pairs_pid );
     // File format: [pair count] left_unipaths..right_unipaths [pair ids]
     Ofstream( zout, dir + "/pairs" );
     {    for ( int64_t i = 0; i < pairs.jsize( ); i++ )
          {    int64_t j = pairs.NextDiff(i);
               if ( pairs[i].first.nonempty( ) || pairs[i].second.nonempty( ) )
               {    zout << "[" << j-i << "] " << printSeq( pairs[i].first ) << ".."
                         << printSeq( pairs[i].second );
                    vec<int64_t> pids;
                    for ( int k = i; k < j; k++ )
                         pids.push_back( pairs_pid[k] );
                    UniqueSort(pids);
                    zout << " [pid=" << printSeq(pids) << "]\n";    }
               i = j - 1;   }    }    }

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

void GetRoots( const HyperBasevector& hb, vec<int>& to_left, vec<int>& to_right,
     const vec<int>& lefts, const vec<int>& rights, int& lroot, int& rroot )
{    lroot = -1; 
     rroot = -1;
     vec<int> x;

     digraphE<basevector> L( digraphE<basevector>::COMPLETE_SUBGRAPH_EDGES, 
          hb, lefts, to_left, to_right );
     while(1)
     {    L.TerminalEdges(x);
          if ( x.empty( ) ) break;
          if ( x.solo( ) ) { lroot = lefts[ x[0] ]; break; }
          L.DeleteEdges(x);    }
     digraphE<basevector> R( digraphE<basevector>::COMPLETE_SUBGRAPH_EDGES, 
          hb, rights, to_left, to_right );
     while(1)
     {    R.InitialEdges(x);
          if ( x.empty( ) ) break;
          if ( x.solo( ) ) { rroot = rights[ x[0] ]; break; }
          R.DeleteEdges(x);    }
     /*
     if ( lroot < 0 || rroot < 0 || lroot == rroot ) return;
     const int max_prox2 = 300;
     const int max_iterations = 10000;
     if ( rroot >= 0 )
     {    vec<int> x;
          hb.GetPredecessors1( to_left[rroot], x );
          vec<vec<int>> paths;
          for ( int z = 0; z < x.isize( ); z++ )
          {    if ( hb.Source( x[z] ) )
               {    if ( x[z] == to_left[rroot] ) continue; // why needed?
                    vec<vec<int>> p;
                    hb.EdgePaths( to_left, to_right, x[z], to_left[rroot], 
                         p, -1, -1, max_iterations );
                    paths.append(p);    }    }
          for ( int z = 0; z < paths.isize( ); z++ )
          {    int len = 0;
               for ( int r = 0; r < paths[z].isize( ); r++ )
                    len += hb.EdgeLengthKmers( paths[z][r] );
               if ( len > max_prox2 ) rroot = -1;    }    }
     if ( lroot >= 0 )
     {    vec<int> x;
          hb.GetSuccessors1( to_right[lroot], x );
          vec<vec<int>> paths;
          for ( int z = 0; z < x.isize( ); z++ )
          {    if ( hb.Sink( x[z] ) )
               {    if ( x[z] == to_right[lroot] ) continue; // why needed?
                    vec<vec<int>> p;
                    hb.EdgePaths( to_left, to_right, to_right[lroot], x[z], 
                         p, -1, -1, max_iterations );
                    paths.append(p);    }    }
          for ( int z = 0; z < paths.isize( ); z++ )
          {    int len = 0;
               for ( int r = 0; r < paths[z].isize( ); r++ )
                    len += hb.EdgeLengthKmers( paths[z][r] );
               if ( len > max_prox2 ) lroot = -1;    }    }    
     */
     }

void MakeLocalAssembly2(VecEFasta &corrected,
                        const vec<int> &lefts, const vec<int> &rights,
                        SupportedHyperBasevector &shb, const int K2_FLOOR,
                        vecbasevector &creads/*, LongProtoTmpDirManager &tmp_mgr*/, vec<int> &cid,
                        vec<pairing_info> &cpartner) {
    long_logging logc("", "");
    logc.STATUS_LOGGING = False;
    logc.MIN_LOGGING = False;
    ref_data ref;
    vec<ref_loc> readlocs;
    long_logging_control log_control(ref, &readlocs, "", "");
    long_heuristics heur("");
    heur.K2_FLOOR = K2_FLOOR;
    int count = 0;
    for (int l = 0; l < (int) corrected.size(); l++)
        if (corrected[l].size() > 0) count++;
    if (count == 0) {
        //mout << "No reads were corrected." << std::endl;
    } else {
        if (!LongHyper("", corrected, cpartner, shb, heur, log_control, logc, /*tmp_mgr,*/ False)) {
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
         SerfVec<int>& p = paths[i];
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
