///////////////////////////////////////////////////////////////////////////////
//                   SOFTWARE COPYRIGHT NOTICE AGREEMENT                     //
//       This software and its documentation are copyright (2013) by the     //
//   Broad Institute.  All rights are reserved.  This software is supplied   //
//   without any warranty or guaranteed support whatsoever. The Broad        //
//   Institute is not responsible for its use, misuse, or functionality.     //
///////////////////////////////////////////////////////////////////////////////
/*
 * BuildReadQGraph.h
 *
 *  Created on: Jan 22, 2014
 *      Author: tsharpe
 */

#ifndef PATHS_LONG_BUILDREADQGRAPH_H_
#define PATHS_LONG_BUILDREADQGRAPH_H_

#include "dvString.h"
#include "feudal/ObjectManager.h"
#include "feudal/PQVec.h"
#include "paths/HyperBasevector.h"
#include "paths/long/ReadPath.h"

typedef struct __attribute__((__packed__)) KMerNodeFreq_s {
    uint64_t kdata[2];
    uint8_t count;
    uint8_t kc;
    inline const operator==(KMerNodeFreq_s const & other) const {
        //return 0==memcmp(&kdata,&other.kdata,2*sizeof(uint64_t));
        return (kdata[0]==other.kdata[0] and kdata[1]==other.kdata[1]);
    }
    inline const operator<(KMerNodeFreq_s const & other) const{
        //return -1==memcmp(&kdata,&other.kdata,2*sizeof(uint64_t));
        if (kdata[0]<other.kdata[0]) return true;
        if (kdata[0]>other.kdata[0]) return false;
        return kdata[1]<other.kdata[1];
    }
    inline const operator>(KMerNodeFreq_s const & other) const{
        if (kdata[0]>other.kdata[0]) return true;
        if (kdata[0]<other.kdata[0]) return false;
        return kdata[1]>other.kdata[1];
    }
    inline void combine(KMerNodeFreq_s const & other){
        uint16_t newcount=count;
        newcount+=other.count;
        if ( newcount>255) count=255;
        else count=newcount;
        kc|=other.kc;
    }
    /*inline const operator=(KMerNodeFreq_s const & other) const {
        memcpy(&this,&other,sizeof(this));
    }*/
};

class KmerList{
public:
    ~KmerList();
    void clear();
    void merge(KmerList & other);
    void sort();
    void uniq();
    void dump(std::string filename);
    void load(std::string filename);
    void resize(size_t new_size);
    KMerNodeFreq_s * kmers = nullptr;
    size_t size=0;
};

void create_read_lengths(std::vector<uint16_t> & rlen, VecPQVec const& quals, unsigned minQual);
/*void buildReadQGraph( vecbvec const& reads, VecPQVec &quals, std::vector<uint16_t> & rlen,
                        bool doFillGaps, bool doJoinOverlaps, unsigned minFreq,
                        double minFreq2Fract, unsigned maxGapSize,
                        HyperBasevector* pHBV, ReadPathVec* pPaths, int _K, std::string workdir="",
                        std::string tmpdir="", unsigned char disk_batches=0, uint64_t count_batch_size=10000000);
*/
void buildReadQGraph( vecbvec const & reads, VecPQVec const &quals, std::shared_ptr<KmerList> kmerlist,
                      unsigned minFreq, HyperBasevector* pHBV, ReadPathVec* pPaths, int _K);
//void dumpkmers( std::shared_ptr<std::vector<KMerNodeFreq_s>> const kmercounts, std::string filename);
//std::shared_ptr<std::vector<KMerNodeFreq_s>> loadkmers( std::string filename);



std::shared_ptr<KmerList> buildKMerCount( vecbvec const& reads,
                                                             std::vector<uint16_t> & rlen, unsigned minCount,
                                                             std::string workdir, std::string tmpdir,
                                                             unsigned char disk_batches, uint64_t count_batch_size );


#endif /* PATHS_LONG_BUILDREADQGRAPH_H_ */
