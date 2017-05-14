#include <G3Pipeline.h>

#include <G3Data.h>

#include <sys/time.h>
#include <sys/resource.h>
#include <cxxabi.h>
#include <iostream>

G3Pipeline::G3Pipeline()
{
	log_trace("Initializing Pipeline");
}

static std::string
cxx_demangle(const char *name)
{
	int err;
	char *demangled;

	demangled = abi::__cxa_demangle(name, NULL, NULL, &err);

	std::string demangled_name((err == 0) ? demangled : name);
	free(demangled);

	return demangled_name;
}

void
G3Pipeline::Add(G3ModulePtr module, std::string name)
{
	if (name == "")
		name = cxx_demangle(typeid(*module).name());

	log_trace("Adding module \"%s\"", name.c_str());

	modules_.push_back(
	    std::pair<std::string, G3ModulePtr>(name, module));
}

namespace {
struct G3Pipeline_mod_data {
	G3Pipeline_mod_data(std::pair<std::string, G3ModulePtr> &module) {
		frames = 0;
		mod = module.second;
		name = module.first;
		utime.tv_sec = 0; utime.tv_usec = 0; stime = utime;
	}

	std::string name;
	G3ModulePtr mod;
	int frames;
	struct timeval utime;
	struct timeval stime;
	int graph_id;
};

struct G3Pipeline_proc_data{
	G3Pipeline_proc_data(int mod_id, int frame_id, 
			     G3Frame::FrameType frame_type) :
		mod_id(mod_id),
		frame_id(frame_id),
		frame_type(frame_type) {}
	int mod_id;
	int frame_id;
	G3Frame::FrameType frame_type;
};

// Recursive inner loop of G3Pipeline::Run()
static size_t
PushFrameThroughQueue(G3FramePtr frame, bool profile, bool graph,
    struct rusage &last_rusage, std::vector<G3Pipeline_mod_data> &mods,
    std::vector<G3Pipeline_mod_data>::iterator next_mod,
    int &graph_frame_id_vals, std::deque<G3Pipeline_proc_data> &graph_proc_data)
{
	std::deque<G3FramePtr> outqueue;

	// Records what processing is happening for graphing
	if (graph && !!frame) {
		int local_frame_id;

		if (frame->Has("_G3GraphingFrameId")) {
			local_frame_id = frame->
			    Get<G3Int>("_G3GraphingFrameId")->value;
		} else {
			frame->Put("_G3GraphingFrameId",
			    G3IntConstPtr(new G3Int(graph_frame_id_vals)));
			local_frame_id = graph_frame_id_vals;
			graph_frame_id_vals++;
		}

		graph_proc_data.push_back(G3Pipeline_proc_data(
		    next_mod->graph_id, local_frame_id, frame->type));
	}

	// Actually perform the processing
	try {
		log_trace("Pushing frame through module \"%s\"",
		    next_mod->name.c_str());
		next_mod->mod->Process(frame, outqueue);
	} catch (const std::exception &e) {
		log_warn("Exception in module \"%s\" (%s): %s",
		    next_mod->name.c_str(), typeid(e).name(), e.what());
		throw;
	} catch (...) {
		log_warn("Exception in module \"%s\"", next_mod->name.c_str());
		throw;
	}

	// Make sure end processing frames are not eaten, since that can
	// cause strange behavior (unclosed output files and the like).
	if (frame && frame->type == G3Frame::EndProcessing) {
		if (outqueue.size() == 0)
			log_fatal("No output on EndProcessing frame in module "
			    "\"%s\"", next_mod->name.c_str());
		if (outqueue.back()->type != G3Frame::EndProcessing)
			log_fatal("Last queued output frame from module "
			    "\"%s\" on EndProcessing not an EndProcessing "
			    "frame.", next_mod->name.c_str());
	}

	if (profile) {
		struct rusage rusage;
		struct timeval deltat;

#ifdef __APPLE__
		getrusage(RUSAGE_SELF, &rusage);
#else
		getrusage(RUSAGE_THREAD, &rusage);
#endif

		timersub(&rusage.ru_utime, &last_rusage.ru_utime, &deltat);
		timeradd(&next_mod->utime, &deltat, &next_mod->utime);

		timersub(&rusage.ru_stime, &last_rusage.ru_stime, &deltat);
		timeradd(&next_mod->stime, &deltat, &next_mod->stime);

		next_mod->frames++;
		last_rusage = rusage;
	}

	// Bail if this is the last module
	if ((next_mod + 1) == mods.end())
		return outqueue.size();

	// Send each frame on recursively
	for (auto i = outqueue.begin(); i != outqueue.end(); i++)
		PushFrameThroughQueue(*i, profile, graph, last_rusage, mods,
		    next_mod + 1, graph_frame_id_vals, graph_proc_data);

	return outqueue.size();
}
}

volatile bool G3Pipeline::halt_processing = false;

void
G3Pipeline::sigint_catcher(int)
{
	log_warn("SIGINT received: halting data processing after "
	    "current frame. Send SIGINT again to abort processing "
	    "immediately, which may result in corrupt output files.");
	G3Pipeline::halt_processing = true;
}

void
G3Pipeline::Run(bool profile, bool graph)
{
	struct rusage last_rusage;
	std::vector<G3Pipeline_mod_data> mods;
	struct sigaction sigint_catcher, oldsigint;
	
	// Variables needed for graphing processing chain
	int graph_frame_id_vals = 0;
	std::deque<G3Pipeline_proc_data> graph_proc_data;
	if (modules_.size() == 0)
		log_fatal("No mods present when running pipeline");

	if (profile)
#ifdef __APPLE__
		getrusage(RUSAGE_SELF, &last_rusage);
#else
		getrusage(RUSAGE_THREAD, &last_rusage);
#endif

	// Catch SIGINT
	sigint_catcher.sa_handler = &G3Pipeline::sigint_catcher;
	sigint_catcher.sa_flags = SA_RESTART | SA_RESETHAND;
	sigemptyset(&sigint_catcher.sa_mask);
	sigaddset(&sigint_catcher.sa_mask, SIGINT);
	sigaction(SIGINT, &sigint_catcher, &oldsigint);

	for (auto i = modules_.begin(); i != modules_.end(); i++)
		mods.push_back(G3Pipeline_mod_data(*i));

	if (graph) {
		for (size_t i = 0; i < mods.size(); i++)
			mods[i].graph_id = i;
	}

	// Start with a NULL frame on the first module, stop when it yields no
	// frames
	while (!G3Pipeline::halt_processing && PushFrameThroughQueue(
	    G3FramePtr(), profile, graph, last_rusage, mods, mods.begin(),
	    graph_frame_id_vals, graph_proc_data) != 0)
	{}

	// One last EndProcessing frame, starting at the second module, so
	// later code can do any necessary cleanup.
	if (mods.size() != 1) {
		PushFrameThroughQueue(
		    G3FramePtr(new G3Frame(G3Frame::EndProcessing)), profile,
		    graph, last_rusage, mods, mods.begin() + 1,
		    graph_frame_id_vals, graph_proc_data);
	}

	// Restore old handler
	G3Pipeline::halt_processing = false;
	sigaction(SIGINT, &oldsigint, &sigint_catcher);

	if (profile) {
		printf("Pipeline profiling results:\n");
		for (auto i = mods.begin(); i != mods.end(); i++) {
			printf("%s: %ld.%06ld user, %ld.%06ld system, %d frames"
			    " (%.6f s per input frame)\n", i->name.c_str(),
			    long(i->utime.tv_sec), long(i->utime.tv_usec),
			    long(i->stime.tv_sec), long(i->stime.tv_usec),
			    i->frames,
			    (double(i->utime.tv_sec + i->stime.tv_sec) +
			     double(i->utime.tv_usec + i->stime.tv_usec)/1e6) /
			     i->frames);
		}
	}

	if (graph) {
		std::ostringstream os;

		// Formatted to be json parsable: I am a sucker for json
		os << std::endl << "{ \"Module List\": [" << std::endl;
		for (size_t i = 0; i < mods.size(); i++) {
			os << "["<< mods[i].graph_id << ", " 
				  << "\"" << mods[i].name << "\"]";
			if (i < mods.size()-1) os << "," << std::endl;
			else os << std::endl;
		}
		os << "]," <<std::endl << " \"Processing List\": [" << std::endl;
		for (size_t i = 0; i < graph_proc_data.size(); i++) {
			os <<"[" << i << ", " << graph_proc_data[i].mod_id 
				  << ", "<< graph_proc_data[i].frame_id<<", "
				  << "\""<<graph_proc_data[i].frame_type <<"\"]";
			if (i < graph_proc_data.size() -1 ) os<< "," <<std::endl;
			else os<< std::endl;
		}
		os << "] "<< std::endl <<" }" << std::endl;
		graph_info_ = os.str();
	}
}

