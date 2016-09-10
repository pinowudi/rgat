#include "stdafx.h"
#include "trace_handler.h"
#include "traceMisc.h"
#include "GUIConstants.h"
#include "traceStructs.h"

bool thread_trace_handler::find_internal_at_address(long address) {
	int attempts = 2;
	while (!piddata->disassembly.count(address))
	{
		Sleep(5);
		if (!attempts--) return false;
		printf("Sleeping until EXTERN %lx...", address);
	}
	return true;
}

bool thread_trace_handler::get_extern_at_address(long address, BB_DATA **BB) {
	int attempts = 2;
	while (!piddata->externdict.count(address))
	{
		Sleep(5);
		if (!attempts--) return false;
		printf("Sleeping until bbdict contains EXTERN %lx\n", address);
	}

	obtainMutex(piddata->externDictMutex, 0, 1000);
	*BB = piddata->externdict.at(address);
	dropMutex(piddata->externDictMutex, 0);
	return true;
}

void thread_trace_handler::insert_edge(edge_data e, NODEPAIR edgePair)
{
	thisgraph->add_edge(e, edgePair);
	if (e.weight > thisgraph->maxWeight)
		thisgraph->maxWeight = e.weight;
}

//checks if current thread has executed this instruction
bool thread_trace_handler::is_old_instruction(INS_DATA *instruction, unsigned int *vertIdx)
{
	obtainMutex(piddata->disassemblyMutex, 0, 100);
	map<int,int>::iterator vertIdIt = instruction->threadvertIdx.find(TID);
	dropMutex(piddata->disassemblyMutex, 0);
	if (vertIdIt != instruction->threadvertIdx.end())
	{
		*vertIdx = vertIdIt->second;
		return true;
	}
	else 
		return false;
}

void thread_trace_handler::update_conditional_state(unsigned long nextAddress)
{
	if (lastVertID)
	{
		node_data *lastNode = thisgraph->get_node(lastVertID);
		int lastNodeCondStatus = lastNode->conditional;
		if (lastNodeCondStatus & CONDPENDING)
		{
			bool alreadyTaken = lastNodeCondStatus & CONDTAKEN;
			bool alreadyFailed = lastNodeCondStatus & CONDFELLTHROUGH;

			if (!alreadyFailed && (nextAddress == lastNode->ins->condDropAddress))
			{
				lastNodeCondStatus |= CONDFELLTHROUGH;
				alreadyFailed = true;
			}

			if (!alreadyTaken && (nextAddress == lastNode->ins->condTakenAddress))
			{
				lastNodeCondStatus |= CONDTAKEN;
				alreadyTaken = true;
			}

			if (alreadyTaken && alreadyFailed)
				lastNodeCondStatus = CONDCOMPLETE;
			lastNode->conditional = lastNodeCondStatus;
		}
	}

}

void thread_trace_handler::handle_new_instruction(INS_DATA *instruction, int mutation, int bb_inslist_index)
{

	node_data thisnode;
	thisnode.ins = instruction;
	if (instruction->conditional) thisnode.conditional = CONDPENDING;

	targVertID = thisgraph->get_num_nodes();
	int a = 0, b = 0;
	int bMod = 0;

	//first instruction in bb,
	if (bb_inslist_index == 0 && lastRIPType == FIRST_IN_THREAD)
	{
			a = 0;
			b = 0;
	}

	if (lastRIPType != FIRST_IN_THREAD)
	{
		node_data *lastNode = thisgraph->get_node(lastVertID);
		VCOORD lastnodec = lastNode->vcoord;
		a = lastnodec.a;
		b = lastnodec.b;
		bMod = lastnodec.bMod;

		if (afterReturn)
		{
			lastRIPType = AFTERRETURN;
			afterReturn = false;
		}

		//place vert on sphere based on how we got here
		positionVert(&a, &b, &bMod, thisnode.ins->address);

	}
	thisnode.vcoord.a = a;
	thisnode.vcoord.b = b;
	thisnode.vcoord.bMod = bMod;
	thisnode.index = targVertID;
	thisnode.ins = instruction;
	thisnode.address = instruction->address;
	thisnode.mutation = mutation;

	updateStats(a, b, bMod);
	usedCoords[a][b] = true;

	thisgraph->insert_node(targVertID, thisnode);

	obtainMutex(piddata->disassemblyMutex, 0, 100);
	instruction->threadvertIdx[TID] = targVertID;
	dropMutex(piddata->disassemblyMutex, 0);
}


void thread_trace_handler::increaseWeight(edge_data *edge, long executions)
{
	edge->weight += executions;
	if (edge->weight > thisgraph->maxWeight)
		thisgraph->maxWeight = edge->weight;
}

void thread_trace_handler::handle_existing_instruction(INS_DATA *instruction)
{
	obtainMutex(piddata->disassemblyMutex, 0, 100);
	targVertID = instruction->threadvertIdx.at(TID);
	dropMutex(piddata->disassemblyMutex, 0);
}

int thread_trace_handler::runBB(unsigned long startAddress, int startIndex,int numInstructions, int repeats = 1)
{
	unsigned int bb_inslist_index = 0;
	bool newVert;
	int firstMutation = -1;
	int mutation = -1;
	unsigned long targetAddress = startAddress;
	for (int instructionIndex = 0; instructionIndex < numInstructions; instructionIndex++)
	{
		//conspicuous lack of mutation handling here
		//we could check this by looking at the mutation state of all members of the block
		INS_DATA *instruction = getLastDisassembly(targetAddress, piddata->disassemblyMutex, &piddata->disassembly, &mutation);
		if (firstMutation == -1) firstMutation = mutation;

		//todo: ditch this?
		if (lastRIPType != FIRST_IN_THREAD)
		{
			if (!thisgraph->node_exists(lastVertID))
			{
				printf("\t\tFatal error last vert not found\n");
				assert(0);
			}
		}

		unsigned int existingVertID;
		//target vert already on this threads graph?
		bool alreadyExecuted = is_old_instruction(instruction, &existingVertID);
		if (alreadyExecuted)
			targVertID = existingVertID; 
		else 
			handle_new_instruction(instruction, mutation, bb_inslist_index);

		if (bb_inslist_index == startIndex && loopState == LOOP_START)
		{
			firstLoopVert = targVertID;
			loopState = LOOP_PROGRESS;
		}

		long nextAddress = instruction->address + instruction->numbytes;
		//again, 2 lookups here
		NODEPAIR edgeIDPair = make_pair(lastVertID, targVertID);
		edge_data *edged;
		if (thisgraph->edge_exists(edgeIDPair, &edged))
			increaseWeight(edged, repeats);

		else if (lastRIPType != FIRST_IN_THREAD)
		{
			edge_data newEdge;
			newEdge.weight = repeats; //todo: skip on first+last edge?
			
			if (lastRIPType == RETURN)
				newEdge.edgeClass = IRET;
			else if (!alreadyExecuted)
			{
				if (lastRIPType == CALL)
					newEdge.edgeClass = ICALL;
				else
					newEdge.edgeClass = INEW;
			}
			else
				newEdge.edgeClass = IOLD;

			insert_edge(newEdge, edgeIDPair);
		}

		//setup conditions for next instruction
		switch (instruction->itype)
		{
			case OPCALL: 
				{
					lastRIPType = CALL;

					//let returns find their caller if and only if they have one
					callStack.push_back(make_pair(nextAddress, lastVertID));
					break;
				}
				
			case OPJMP:
				lastRIPType = JUMP;
				break;

			case OPRET:
				lastRIPType = RETURN;
				break;

			default:
				lastRIPType = NONFLOW;
				break;
		}
		lastVertID = targVertID;
		targetAddress = nextAddress;
	}

	return firstMutation;
}

void thread_trace_handler::updateStats(int a, int b, int bMod) {
	if (abs(a) > thisgraph->maxA) thisgraph->maxA = abs(a);
	if (abs(b) > thisgraph->maxB) thisgraph->maxB = abs(b);
	if (bMod > thisgraph->bigBMod) thisgraph->bigBMod = bMod;
}

//takes position of a node as pointers
//performs an action (call,jump,etc), places new position in pointers
void thread_trace_handler::positionVert(int *pa, int *pb, int *pbMod, long address)
{
	int a = *pa;
	int b = *pb;
	int bMod = *pbMod;
	int clash = 0;

	switch (lastRIPType)
	{
	case AFTERRETURN:
		a = min(a - 20, -(thisgraph->maxA + 2));
		b += 7 * BMULT;
		break;

	case NONFLOW:
		{
			//is it a conditional jump being taken? -> fall through to jump
			//TODO: this tends to be a source of messy edge overlap
			node_data *lastNode = thisgraph->get_node(lastVertID);
			if (!lastNode->conditional || address != lastNode->ins->condTakenAddress)
			{
				bMod += 1 * BMULT;
				break;
			}
		}
	case JUMP:
		{
			a += JUMPA;
			b += JUMPB * BMULT;

			while (usedCoords[a][b] == true)
			{
				a += JUMPA_CLASH;
				if (clash++ > 15)
					printf("\tWARNING: JUMP MAXED\n");
			}
			break;
		}
	case CALL:
		{
			b += CALLB * BMULT;

			while (usedCoords[a][b] == true)
			{
				a += CALLA_CLASH;
				b += CALLB_CLASH * BMULT;

				if (clash++ > 15)
					printf("\tWARNING: CALL MAXED\n");
			}

			if (clash) a += CALLA_CLASH;
			break;
		}
	case RETURN:
		afterReturn = true;
	case EXTERNAL:
		{
			int result = -1;
			vector<pair<long, int>>::iterator it;
			for (it = callStack.begin(); it != callStack.end(); ++it)
				if (it->first == address)
				{
					result = it->second;
					break;
				}

			if (result != -1)
			{
				VCOORD *caller = &thisgraph->get_node(result)->vcoord;
				a = caller->a + RETURNA_OFFSET;
				b = caller->b + RETURNB_OFFSET;
				bMod = caller->bMod;
				
				//may not have returned to the last item in the callstack
				//delete everything inbetween
				callStack.resize(it-callStack.begin());
			}
			else
			{
				a += EXTERNA;
				b += EXTERNB * BMULT;
			}
		
			break;
		}
	default:
		if (lastRIPType != FIRST_IN_THREAD)
			printf("ERROR: Unknown Last RIP Type\n");
		break;
	}
	*pa = a;
	*pb = b;
	*pbMod = bMod;
	return;
}

void __stdcall thread_trace_handler::ThreadEntry(void* pUserData) {
	return ((thread_trace_handler*)pUserData)->TID_thread();
}

void thread_trace_handler::handle_arg(char * entry, size_t entrySize) {
	unsigned long funcpc, returnpc;
	string argidx_s = string(strtok_s(entry + 4, ",", &entry));
	int argpos;
	if (!caught_stoi(argidx_s, &argpos, 10)) {
		printf("handle_arg 3 STOL ERROR: %s\n", argidx_s.c_str());
		return;
	}

	string funcpc_s = string(strtok_s(entry, ",", &entry));
	if (!caught_stol(funcpc_s, &funcpc, 16)) {
		printf("handle_arg 4 STOL ERROR: %s\n", funcpc_s.c_str());
		return;
	}

	string retaddr_s = string(strtok_s(entry, ",", &entry));
	if (!caught_stol(retaddr_s, &returnpc, 16)) {
		printf("handle_arg 5 STOL ERROR: %s\n", retaddr_s.c_str());
		return;
	}

	if (!pendingFunc) {
		pendingFunc = funcpc;
		pendingRet = returnpc;
	}

	string moreargs_s = string(strtok_s(entry, ",", &entry));
	bool callDone = moreargs_s.at(0) == 'E' ? true : false;

	//todo: b64 decode
	string contents;
	if (entry < entry+entrySize)
		contents = string(entry).substr(0, entrySize - (size_t)entry);
	else
		contents = string("NULL");

	BB_DATA* targbbptr;
	get_extern_at_address(funcpc, &targbbptr);
	printf("Handling arg %s of function %s [addr %lx] module %s retting to %lx\n",
		contents.c_str(),
		piddata->modsyms[targbbptr->modnum][funcpc].c_str(),
		funcpc,
		piddata->modpaths[targbbptr->modnum].c_str(),
		returnpc);

	pendingArgs.push_back(make_pair(argpos, contents));
	if (!callDone) return;

	//func been called in thread already? if not, have to place args in holding buffer
	if (thisgraph->pendingcallargs.count(pendingFunc) == 0)
	{
		map <unsigned long, vector<ARGLIST>> *newmap = new map <unsigned long, vector<ARGLIST>>;
		thisgraph->pendingcallargs.emplace(pendingFunc, *newmap);
	}
	if (thisgraph->pendingcallargs.at(pendingFunc).count(pendingRet) == 0)
	{
		vector<ARGLIST> *newvec = new vector<ARGLIST>;
		thisgraph->pendingcallargs.at(pendingFunc).emplace(pendingRet, *newvec);
	}
		
	ARGLIST::iterator pendcaIt = pendingArgs.begin();
	ARGLIST thisCallArgs;
	for (; pendcaIt != pendingArgs.end(); pendcaIt++)
		thisCallArgs.push_back(*pendcaIt);

	//thisgraph->pendingcallargs.at(funcpc).at(returnpc).push_back(thisCallArgs);
	thisgraph->pendingcallargs.at(pendingFunc).at(pendingRet).push_back(thisCallArgs);

	pendingArgs.clear();
	pendingFunc = 0;
	pendingRet = 0;

	process_new_args();
}

int thread_trace_handler::run_external(unsigned long targaddr, unsigned long repeats, NODEPAIR *resultPair)
{
	//if parent calls multiple children, spread them out around caller
	//todo: can crash here if lastvid not in vd - only happned while pause debugging tho
	node_data *lastNode = thisgraph->get_node(lastVertID);
	assert(lastNode->ins->numbytes);

	//start by examining our caller
	
	int callerModule = lastNode->nodeMod;
	//if caller is external, not interested in this //todo: no longer happens
	if (piddata->activeMods[callerModule] == MOD_UNINSTRUMENTED) return -1;
	BB_DATA *thisbb = 0;
	do {
		get_extern_at_address(targaddr, &thisbb);
	} while (!thisbb);

	//see if caller already called this
	//if so, get the destination so we can just increase edge weight
	auto x = thisbb->thread_callers.find(TID);
	if (x != thisbb->thread_callers.end())
	{
		EDGELIST::iterator vecit = x->second.begin();
		for (; vecit != x->second.end(); vecit++)
		{
			if (vecit->first != lastVertID) continue;

			//this instruction in this thread has already called it
			targVertID = vecit->second;
			node_data *targNode = thisgraph->get_node(targVertID);

			*resultPair = std::make_pair(vecit->first, vecit->second);
			increaseWeight(thisgraph->get_edge(*resultPair), repeats);
			targNode->calls += repeats;

			return 1;
		}
		//else: thread has already called it, but from a different place
		
	}
	//else: thread hasnt called this function before

	lastNode->childexterns += 1;
	targVertID = thisgraph->get_num_nodes();
	//todo: check thread safety. crashes
	if (!thisbb->thread_callers.count(TID))
	{
		EDGELIST callervec;
		callervec.push_back(make_pair(lastVertID, targVertID));
		thisbb->thread_callers.emplace(TID, callervec);
	}
	else
		thisbb->thread_callers.at(TID).push_back(make_pair(lastVertID, targVertID));
	
	int module = thisbb->modnum;

	//make new external/library call node
	node_data newTargNode;
	newTargNode.nodeMod = module;

	int parentExterns = thisgraph->get_node(lastVertID)->childexterns;
	VCOORD lastnodec = thisgraph->get_node(lastVertID)->vcoord;

	newTargNode.vcoord.a = lastnodec.a + 2 * parentExterns + 5;
	newTargNode.vcoord.b = lastnodec.b + parentExterns + 5;
	newTargNode.vcoord.bMod = lastnodec.bMod;
	newTargNode.external = true;
	newTargNode.address = targaddr;
	newTargNode.index = targVertID;
	newTargNode.parentIdx = lastVertID;

	BB_DATA *thisnode_bbdata = 0;
	get_extern_at_address(targaddr, &thisnode_bbdata);
	unsigned long returnAddress = lastNode->ins->address + lastNode->ins->numbytes;

	thisgraph->insert_node(targVertID, newTargNode); //this invalidates lastnode
	lastNode = &newTargNode;

	obtainMutex(thisgraph->funcQueueMutex, "Push Externlist", 1200);
	thisgraph->externList.push_back(targVertID);
	dropMutex(thisgraph->funcQueueMutex, "Push Externlist");
	*resultPair = std::make_pair(lastVertID, targVertID);

	edge_data newEdge;
	newEdge.weight = repeats;
	newEdge.edgeClass = ILIB;
	insert_edge(newEdge, *resultPair);
	lastRIPType = EXTERNAL;
	return 1;
}

void thread_trace_handler::process_new_args()
{

	map<unsigned long, map <unsigned long, vector<ARGLIST>>>::iterator pcaIt = thisgraph->pendingcallargs.begin();
	while (pcaIt != thisgraph->pendingcallargs.end())
	{
		unsigned long funcad = pcaIt->first;
		obtainMutex(piddata->externDictMutex, 0, 1000);
		if (!piddata->externdict.at(funcad)->thread_callers.count(TID)) { 
			dropMutex(piddata->externDictMutex, 0);
			//TODO: keep track of this. printf("Failed to find call for %lx in externdict\n", funcad);
			pcaIt++; continue; 
		}

		
		EDGELIST callvs = piddata->externdict.at(funcad)->thread_callers.at(TID);
		dropMutex(piddata->externDictMutex, 0);

		EDGELIST::iterator callvsIt = callvs.begin();
		while (callvsIt != callvs.end()) //run through each function with a new arg
		{
			node_data *parentn = thisgraph->get_node(callvsIt->first);
			unsigned long returnAddress = parentn->ins->address + parentn->ins->numbytes;
			node_data *targn = thisgraph->get_node(callvsIt->second);

			map <unsigned long, vector<ARGLIST>>::iterator retIt = pcaIt->second.begin();
			while (retIt != pcaIt->second.end())//run through each caller to this function
			{
				if (retIt->first != returnAddress) {retIt++; continue;}

				vector<ARGLIST> callsvector = retIt->second;
				vector<ARGLIST>::iterator callsIt = callsvector.begin();

				obtainMutex(thisgraph->funcQueueMutex, "FuncQueue Push Live", INFINITE);
				while (callsIt != callsvector.end())//run through each call made by caller
				{

					EXTERNCALLDATA ex;
					ex.edgeIdx = make_pair(parentn->index, targn->index);
					ex.nodeIdx = targn->index;
					ex.callerAddr = parentn->ins->address;
					ex.externPath = piddata->modpaths[piddata->externdict.at(funcad)->modnum];
					ex.fdata = *callsIt;

					assert(parentn->index != targn->index);
					thisgraph->funcQueue.push(ex);
					
					if (targn->funcargs.size() < MAX_ARG_STORAGE)
						targn->funcargs.push_back(*callsIt);
					callsIt = callsvector.erase(callsIt);
				}
				dropMutex(thisgraph->funcQueueMutex, "FuncQueue Push Live");
				retIt->second.clear();

				if (retIt->second.empty())
					retIt = pcaIt->second.erase(retIt);
				else
					retIt++;
			}

			callvsIt++;
		}
		if (pcaIt->second.empty())
			pcaIt = thisgraph->pendingcallargs.erase(pcaIt);
		else
			pcaIt++;
	}
}

void thread_trace_handler::handle_tag(TAG thistag, unsigned long repeats = 1)
{
	
	/*printf("handling tag %lx, jmpmod:%d", thistag.targaddr, thistag.jumpModifier);
	if (thistag.jumpModifier == 2)
		printf(" - sym: %s\n", piddata->modsyms[piddata->externdict[thistag.targaddr]->modnum][thistag.targaddr].c_str());
	else printf("\n");*/
	
	update_conditional_state(thistag.blockaddr);

	if (thistag.jumpModifier == INTERNAL_CODE)
	{

		int mutation = runBB(thistag.blockaddr, 0, thistag.insCount, repeats);

		obtainMutex(thisgraph->animationListsMutex);
		thisgraph->bbsequence.push_back(make_pair(thistag.blockaddr, thistag.insCount));

		//could probably break this by mutating code in a running loop
		thisgraph->mutationSequence.push_back(mutation);
		dropMutex(thisgraph->animationListsMutex);

		if (repeats == 1)
		{
			thisgraph->totalInstructions += thistag.insCount;
			thisgraph->loopStateList.push_back(make_pair(0, 0xbad));
		}
		else
		{
			thisgraph->totalInstructions += thistag.insCount*loopCount;
			thisgraph->loopStateList.push_back(make_pair(thisgraph->loopCounter, loopCount));
		}
		thisgraph->set_active_node(lastVertID);
	}

	else if (thistag.jumpModifier == EXTERNAL_CODE) //call to (uninstrumented) external library
	{
		if (!lastVertID) return;

		//find caller,external vertids if old + add node to graph if new
		NODEPAIR resultPair;
		int result = run_external(thistag.blockaddr, repeats, &resultPair);
		
		if (result)
		{
			obtainMutex(thisgraph->animationListsMutex, "Extern run", 1000);
			thisgraph->externCallSequence[resultPair.first].push_back(resultPair);
			dropMutex(thisgraph->animationListsMutex, "Extern run");
		}

		process_new_args();
		thisgraph->set_active_node(resultPair.second);
	}
	else
	{
		printf("ERROR: BAD JUMP MODIFIER 0x%x: CORRUPT TRACE?\n", thistag.jumpModifier);
		assert(0);
	}
}

int thread_trace_handler::find_containing_module(unsigned long address)
{
	const int numModules = piddata->modBounds.size();
	for (int modNo = 0; modNo < numModules; modNo++)
	{
		pair<unsigned long, unsigned long> *bounds = &piddata->modBounds.at(modNo);
		if (address >= bounds->first &&	address <= bounds->second)
		{
			if (piddata->activeMods.at(modNo) == MOD_ACTIVE) return MOD_ACTIVE;
			else return MOD_UNINSTRUMENTED;
		}
	}
	return 0;

}

//thread handler to build graph for a thread
void thread_trace_handler::TID_thread()
{
	thisgraph = (thread_graph_data *)piddata->graphs[TID];
	thisgraph->tid = TID;
	thisgraph->pid = PID;

	char* msgbuf;
	int result;
	unsigned long bytesRead;
	bool threadRunning = true;
	while (threadRunning)
	{
		thisgraph->traceBufferSize = reader->get_message(&msgbuf, &bytesRead);
		if (bytesRead == 0) {
			Sleep(1);
			continue;
		}
		if (bytesRead == -1)
		{
			printf("Thread handler got thread end message, terminating!\n");
			timelinebuilder->notify_tid_end(PID, TID);
			thisgraph->active = false;
			thisgraph->terminated = true;
			thisgraph->emptyArgQueue();
			return;
		}

		char *next_token = msgbuf;
		while (true)
		{
			//todo: check if buf is sensible - suspicious repeats?
			if (next_token >= msgbuf + bytesRead) break;
			char *entry = strtok_s(next_token, "@", &next_token);
			if (!entry) break;

			if (entry[0] == 'j')
			{
				TAG thistag;
				//each of these string conversions is 1% of trace handler time

				thistag.blockaddr = stol(strtok_s(entry + 1, ",", &entry), 0, 16);
				unsigned long nextBlock = stol(strtok_s(entry, ",", &entry), 0, 16);
				thistag.insCount = stol(strtok_s(entry, ",", &entry));

				thistag.jumpModifier = INTERNAL_CODE;
				if (loopState == LOOP_START)
					loopCache.push_back(thistag);
				else
					handle_tag(thistag);

				//fallen through conditional
				if (nextBlock == 0) continue;

				int modType = find_containing_module(nextBlock);
				if (modType == MOD_ACTIVE) continue;

				bool external;
				if (modType == MOD_UNINSTRUMENTED)
					external = true;
				else
					external = false;

				//see if next block is external
				//this is our alternative to instrumenting *everything*
				//rarely called
				while (true)
				{
					if (get_extern_at_address(nextBlock, &thistag.foundExtern))
					{
						external = true;
						break;
					}
					if (find_internal_at_address(nextBlock)) break;
				} 

				if (!external) continue;

				thistag.blockaddr = nextBlock;
				thistag.jumpModifier = EXTERNAL_CODE;
				thistag.insCount = 0;

				int modu = thistag.foundExtern->modnum;

				if (loopState == LOOP_START)
					loopCache.push_back(thistag);
				else
					handle_tag(thistag);

				continue;
			}

			//repeats/loop
			if (entry[0] == 'R')
			{	//loop start
				if (entry[1] == 'S')
				{
					
					loopState = LOOP_START;
					string repeats_s = string(strtok_s(entry+2, ",", &entry));
					if (!caught_stol(repeats_s, &loopCount, 10))
						printf("1 STOL ERROR: %s\n", repeats_s.c_str());
					continue;
				}

				//loop end
				else if (entry[1] == 'E') 
				{
					vector<TAG>::iterator tagIt;
					
					loopState = LOOP_START;

					if (loopCache.empty())
					{
						loopState = NO_LOOP;
						continue;
					}

					thisgraph->loopCounter++;
					//put the verts/edges on the graph
					printf("Processing %d iterations of %d block loop\n", loopCount, loopCache.size());
					for (tagIt = loopCache.begin(); tagIt != loopCache.end(); tagIt++)
						handle_tag(*tagIt, loopCount);

					loopCache.clear();
					loopState = NO_LOOP;
					continue;
				}
				printf("Fell through bad loop tag? [%s]\n",entry);
				assert(0);
			}

			string enter_s = string(entry);
			if (enter_s.substr(0, 3) == "ARG")
			{
				handle_arg(entry, bytesRead);
				continue;
			}

			if (enter_s.substr(0, 3) == "EXC")
			{
				unsigned long e_ip;
				string e_ip_s = string(strtok_s(entry + 4, ",", &entry));
				if (!caught_stol(e_ip_s, &e_ip, 16)) {
					printf("handle_arg 4 STOL ERROR: %s\n", e_ip_s.c_str());
					assert(0);
				}

				unsigned long e_code;
				string e_code_s = string(strtok_s(entry, ",", &entry));
				if (!caught_stol(e_code_s, &e_code, 16)) {
					printf("handle_arg 4 STOL ERROR: %s\n", e_code_s.c_str());
					assert(0);
				}

				printf("Target exception [code %lx] at address %lx\n", e_code, e_ip);
				continue;
			}

			if (enter_s.substr(0, 3) == "BLK")
			{
				unsigned long funcpc;
				string funcpc_s = string(strtok_s(entry+4, ",", &entry));
				if (!caught_stol(funcpc_s, &funcpc, 16)) {
					printf("handle_arg 4 STOL ERROR: %s\n", funcpc_s.c_str());
					assert(0);
				}

				unsigned long retpc;
				string retpc_s = string(strtok_s(entry, ",", &entry));
				if (!caught_stol(retpc_s, &retpc, 16)) {
					printf("handle_arg 4 STOL ERROR: %s\n", retpc_s.c_str());
					assert(0);
				}

				//TODO? BB_DATA* extfunc = piddata->externdict.at(funcpc);
				//thisgraph->set_active_node(extfunc->thread_callers[TID])
				continue;
			}

			printf("<TID THREAD %d> UNHANDLED LINE (%d b): %s\n", TID, bytesRead, msgbuf);
			if (next_token >= msgbuf + bytesRead) break;
		}
	}
	
}

