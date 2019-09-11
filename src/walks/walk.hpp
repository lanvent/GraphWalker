#ifndef DEF_GRAPHWALKER_WALK
#define DEF_GRAPHWALKER_WALK

#include <iostream>
#include <cstdio>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <queue>

#include "metrics/metrics.hpp"
#include "api/filename.hpp"
#include "api/io.hpp"
#include "walks/walkbuffer.hpp"

class WalkManager
{
protected:
	std::string base_filename;
	bid_t nblocks;
	tid_t nthreads;
	metrics &m;
public:
	wid_t* walknum; //number of tptal walks of each block
	wid_t* dwalknum; //number of disk walks of each block
	hid_t* minstep;
	WalkBuffer **pwalks;

	bid_t curp; //current block id
	WalkDataType *curwalks; // all walks of current block
	wid_t walksum;

	bool* ismodified;

public:
	WalkManager(metrics &_m,bid_t _nblocks, tid_t _nthreads, std::string _base_filename):base_filename(_base_filename), nblocks(_nblocks), nthreads(_nthreads), m(_m){
		pwalks = new WalkBuffer*[nthreads];
		for(tid_t i = 0; i < nthreads; i++)
			pwalks[i] = new WalkBuffer[nblocks];

		walknum = (wid_t*)malloc(nblocks*sizeof(wid_t));
		dwalknum = (wid_t*)malloc(nblocks*sizeof(wid_t));
		minstep = (hid_t*)malloc(nblocks*sizeof(hid_t));
		memset(walknum, 0, nblocks*sizeof(wid_t));
		memset(dwalknum, 0, nblocks*sizeof(wid_t));
		memset(minstep, 0xffff, nblocks*sizeof(hid_t));
		walksum = 0;

		rm_dir((base_filename+"_GraphWalker/walks/").c_str());
		mkdir((base_filename+"_GraphWalker/walks/").c_str(), 0777);	

		ismodified = (bool*)malloc(nblocks*sizeof(bool));
		memset(ismodified, false, nblocks*sizeof(bool));
	}

	~WalkManager(){
		for(bid_t p = 0; p < nthreads; p++){
			if(pwalks[p] != NULL){
				delete [] pwalks[p];
			}
		}
		if(pwalks != NULL) delete [] pwalks;
		if(walknum != NULL) free(walknum);
		if(dwalknum != NULL) free(dwalknum);
		if(minstep != NULL) free(minstep);
	}

	WalkDataType encode( vid_t sourceId, vid_t currentId, hid_t hop ){
		assert( hop < 16384 );
		return (( (WalkDataType)sourceId & 0xffffff ) << 40 ) |(( (WalkDataType)currentId & 0x3ffffff ) << 14 ) | ( (WalkDataType)hop & 0x3fff ) ;
	}

	vid_t getSourceId( WalkDataType walk ){
		return (vid_t)( walk >> 40 ) & 0xffffff;
	}

	vid_t getCurrentId( WalkDataType walk ){
		return (vid_t)( walk >> 14 ) & 0x3ffffff;
	}

	hid_t getHop( WalkDataType walk ){
		return (hid_t)(walk & 0x3fff) ;
	}

	WalkDataType reencode( WalkDataType walk, vid_t toVertex ){
		hid_t hop = getHop(walk);
		vid_t source = getSourceId(walk);
		walk = encode(source,toVertex,hop);
		return walk;
	}

	void moveWalk( WalkDataType walk, bid_t p, tid_t t, vid_t toVertex ){
		if(pwalks[t][p].size_w == WALK_BUFFER_SIZE){
            // logstream(LOG_DEBUG) << "Walk buffer : pwalks["<< (int)t <<"]["<< p <<"] is ful with size_w = " << pwalks[t][p].size_w << " , WALK_BUFFER_SIZE = " << WALK_BUFFER_SIZE << std::endl;
			writeWalks2Disk(t,p);
        }
        assert(pwalks[t][p].size_w < WALK_BUFFER_SIZE);
		walk = reencode( walk, toVertex );
		pwalks[t][p].push_back( walk );
	}

	void writeWalks2Disk(tid_t t, bid_t p){
		m.start_time("4_writeWalks2Disk");
		std::string walksfile = walksname( base_filename, p );
		int f = open(walksfile.c_str(), O_WRONLY | O_CREAT | O_APPEND, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
		pwritea( f, &pwalks[t][p][0], pwalks[t][p].size_w*sizeof(WalkDataType) );
		dwalknum[p] += pwalks[t][p].size_w;
		pwalks[t][p].size_w = 0;
		close(f);
		m.stop_time("4_writeWalks2Disk");
	}

	wid_t getCurrentWalks(bid_t p, bid_t bb){
		// logstream(LOG_DEBUG) << "getCurrentWalks, p = " << p << ", bb = " << bb << std::endl;
		m.start_time("3_getCurrentWalks");
		wid_t walkcount = 0;
		for(bid_t b = 0; b < bb; b++){
			walkcount += walknum[p+b];
		}
		curwalks = (WalkDataType*)malloc(walkcount*sizeof(WalkDataType));

		wid_t cumsum = 0;
		for(bid_t b = p; b < p+bb; b++){
			if(dwalknum[b] > 0){
				readWalksfromDisk(b, cumsum);
				cumsum += dwalknum[b];
				dwalknum[b] = 0;
				// logstream(LOG_INFO) << "read walks count = " << cumsum << ", disk walknum[b] = " << dwalknum[b] << std::endl;
			}
			for(tid_t t = 0; t < nthreads; t++){
				if(pwalks[t][b].size_w > 0){
					for(wid_t w = 0; w < pwalks[t][b].size_w; w++){
						curwalks[cumsum+w] = pwalks[t][b][w];
					}
					cumsum += pwalks[t][b].size_w;
					// logstream(LOG_INFO) << "t" << (int)t << ", read walks count = " << cumsum << ", pwalks["<<(int)t<<"]["<<b<<"].size_w = " << pwalks[t][b].size_w << std::endl;
					pwalks[t][b].size_w = 0;
				}
			}
		}
		if (cumsum != walkcount) {
			logstream(LOG_DEBUG) << "read walks count = " << cumsum << ", recorded walknum[p] = " << walkcount << std::endl;
			assert(false);
		}
		m.stop_time("3_getCurrentWalks");
		return cumsum;
	}

	void readWalksfromDisk(bid_t p, wid_t offset){
		m.start_time("z_w_readWalksfromDisk");

		std::string walksfile = walksname( base_filename, p );
		int f = open(walksfile.c_str(),O_RDWR, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
		if (f < 0) {
			logstream(LOG_FATAL) << "Could not load :" << walksfile << " error: " << strerror(errno) << std::endl;
		}
		assert(f > 0);
		/* read from file*/
		preada(f, &curwalks[offset], dwalknum[p]*sizeof(WalkDataType), 0);
		/* 清空文件 */
    	ftruncate(f,0);
		close(f);
		/* remove the walk file*/
		unlink(walksfile.c_str()); 

		m.stop_time("z_w_readWalksfromDisk");
	}

	void updateWalkNum(bid_t p, bid_t bb){

		m.start_time("6_updateWalkNum");
		wid_t forwardWalks = 0;
		for(bid_t b = 0; b < nblocks; b++){
			if(ismodified[b]){
				ismodified[b] = false;
				wid_t newwalknum = 0;
				newwalknum = dwalknum[b];
				for(tid_t t = 0; t < nthreads; t++){
					newwalknum += pwalks[t][b].size_w;
				}
				if(newwalknum <= walknum[b]){
					logstream(LOG_DEBUG) << "newwalknum = " << newwalknum << ", walknum[b] = " << walknum[b] << std::endl;
					assert(false);
				}
				forwardWalks += newwalknum - walknum[b];
				walknum[b] = newwalknum;
			}
		}

		// logstream(LOG_DEBUG) <<"Total " << walksum << " walks, Updated " << walknum[p] << " walks, and forward " << forwardWalks << " walks. " << std::endl;
		
		m.start_time("z_w_clear_curwalks");
		walksum += forwardWalks;
		wid_t walkcount = 0;
		for(bid_t b = 0; b < bb; b++){
			walkcount += walknum[p+b];
			walknum[p+b] = 0;
			minstep[p+b] = 0xffff;
		}
		walksum -= walkcount;
		// logstream(LOG_DEBUG) <<"Remain " << walksum << " walks, reduced " << walkcount << " walks. " << std::endl;
		free(curwalks);
		curwalks = NULL;
		m.stop_time("z_w_clear_curwalks");

		m.stop_time("6_updateWalkNum");
	}

     void setMinStep(bid_t p, hid_t hop ){
		if(minstep[p] > hop)
		{
			#pragma omp critical
			{
				minstep[p] = hop;
			}
		}
     }

     bid_t blockWithMaxWalks(){
		wid_t maxw = 0, maxp = 0;
		for(bid_t p = 0; p < nblocks; p++) {
			if( maxw < walknum[p] ){
				maxw = walknum[p];
				maxp = p;
			}
	   	}
		return maxp;
     }

     bid_t blockWithMinStep(){
		hid_t mins = 0xffff, minp = 0;
		for(bid_t p = 0; p < nblocks; p++) {
			if( mins > minstep[p] ){
				mins = minstep[p];
				minp = p;
			}
	   	}
		if(walknum[minp] > 0)
			return minp;
		return blockWithMaxWalks();
     }

     bid_t blockWithMaxWeight(){
		float maxwt = 0;
		bid_t maxp = 0;
		for(bid_t p = 0; p < nblocks; p++) {
			if(  maxwt < (float)walknum[p]/minstep[p] ){
				maxwt = (float)walknum[p]/minstep[p];
				maxp = p;
			}
	   	}
		return maxp;
     }

     bid_t blockWithRandom(){
		bid_t ranp = rand() % nblocks;
		return ranp;
     }

	bid_t chooseBlock(float prob){
		// return blockWithMaxWeight();//////////////
		float cc = ((float)rand())/RAND_MAX;
		if( cc < prob ){
			return blockWithMinStep();
		}
		return blockWithMaxWalks();
	}

     void printWalksDistribution(bid_t exec_block ){
		//print walk number decrease trend
		std::string walk_filename = base_filename + ".walks";
		std::ofstream ofs;
	    ofs.open(walk_filename.c_str(), std::ofstream::out | std::ofstream::app );
	   	wid_t sum = 0;
	  	for(bid_t p = 0; p < nblocks; p++) {
	      		sum += walknum[p];
	   	}
	  	ofs << exec_block << " \t " << walknum[exec_block] << " \t " << sum << std::endl;
	 	ofs.close();
     }    

};

#endif