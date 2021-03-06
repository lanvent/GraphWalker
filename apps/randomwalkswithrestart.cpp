#define DYNAMICEDATA 1
#include <string>
#include <fstream>
#include <vector>
#include <cmath>

#include "api/graphwalker_basic_includes.hpp"
#include "walks/randomwalkwithrestart.hpp"

bool semi_external;

class RandomWalks : public RandomWalkwithRestart{
    private:
        vid_t s;
        unsigned N, R, L;

    public:
        void initializeApp(vid_t _s, unsigned _N, unsigned _R, unsigned _L, float tail){
            s = _s;
            N = _N;
            R = _R;
            L = _L;
            initializeRW(s, R, L, tail);
        }

        void startWalksbyApp(WalkManager &walk_manager){
            std::cout << "Random walks:\tStart " << R << " walks randomly ..." << std::endl;
            int p = getInterval(s);
            walk_manager.minstep[p] = 0;
            vid_t cur = s - intervals[p].first;
            //std::cout << "startWalksbyApp:\t" << "source" << i <<"=" << s << "\tp=" << p << "\tcur=" << cur << std::endl;
            WalkDataType walk = walk_manager.encode(s,cur,0);
            srand((unsigned)time(NULL));
            unsigned nthreads = get_option_int("execthreads", omp_get_max_threads());
            omp_set_num_threads(nthreads);
            #pragma omp parallel for schedule(static)
                for (unsigned i = 0; i < R; i++){
                    walk_manager.pwalks[omp_get_thread_num()][p].push_back(walk);
                }
            for (unsigned t = 0; t < nthreads; t++)
                walk_manager.walknum[p] +=  walk_manager.pwalks[t][p].size();
            if(!semi_external){
                walk_manager.freshIntervalWalks(p);
            }
        }

        void updateInfo(WalkManager &walk_manager, WalkDataType walk, vid_t dstId){
        }

        /**
         * Called before an execution interval is started.
         */
        void before_exec_interval(unsigned exec_interval, vid_t window_st, vid_t window_en, WalkManager &walk_manager) {
            if(!semi_external){
                /*load walks*/
                walk_manager.readIntervalWalks(exec_interval);
            }
        }
        
        /**
         * Called after an execution interval has finished.
         */
        void after_exec_interval(unsigned exec_interval, vid_t window_st, vid_t window_en, WalkManager &walk_manager) {
            walk_manager.walknum[exec_interval] = 0;
            walk_manager.minstep[exec_interval] = 0xfffffff;
            unsigned nthreads = get_option_int("execthreads", omp_get_max_threads());;
            for(unsigned t = 0; t < nthreads; t++)
                walk_manager.pwalks[t][exec_interval].clear();
            for( unsigned p = 0; p < nshards; p++){
                if(p == exec_interval ) continue;
                if(semi_external) walk_manager.walknum[p] = 0;
                for(unsigned t=0;t<nthreads;t++){
                    walk_manager.walknum[p] += walk_manager.pwalks[t][p].size();
                }
            }

            if(!semi_external){
                /*write back walks*/
                walk_manager.writeIntervalWalks(exec_interval);
            }
        }
};

int main(int argc, const char ** argv){
    set_argc(argc,argv);
    metrics m("randomwalks");
    
    std::string filename = get_option_string("file", "../DataSet/LiveJournal/soc-LiveJournal1.txt");  // Base filename
    unsigned nvertices = get_option_int("nvertices", 4847571); // Number of vertices
    long long nedges = get_option_long("nedges", 68993773); // Number of edges
    unsigned nshards = get_option_int("nshards", 0); // Number of intervals
    unsigned s = get_option_int("s", 0); // source vertex
    unsigned R = get_option_int("R", 10000); // Number of steps
    unsigned L = get_option_int("L", 4); // Number of steps per walk
    float tail = get_option_float("tail", 0); // Ratio of stop long tail
    float prob = get_option_float("prob", 0.2); // prob of chose min step
    semi_external = get_option_int("semi_external", 0);
    
    /* Detect the number of shards or preprocess an input to create them */
    nshards = convert_if_notexists(filename, get_option_string("nshards", "auto"), nvertices, nedges, R, nshards);

    // run
    RandomWalks program;
    program.initializeApp(s,nvertices,R,L,tail);
    graphwalker_engine engine(filename, nshards, m);
    engine.run(program, prob);

    metrics_report(m);
    return 0;
}