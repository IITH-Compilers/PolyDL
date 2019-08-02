#include <pet.h>
#include <iostream>
#include <isl/union_set.h>
#include <isl/flow.h>
#include <stdlib.h>
#include <barvinok/isl.h>
#include <string.h>
#include <vector>
#include <isl/space.h>
#include <unordered_map>
#include <bits/stdc++.h>
#include <fstream>
#include <ConfigProcessor.hpp>
#include <OptionsProcessor.hpp>
#include <Utility.hpp>
using namespace std;


#define IGNORE_WS_SIZE_ONE 1
#define DEBUG 0

/* Function header declarations begin */
struct WorkingSetSize {
	isl_basic_map* dependence;
	isl_set* source;
	isl_set* target;
	isl_set *minTarget;
	isl_set *maxTarget;
	isl_union_pw_qpolynomial* minSize;
	isl_union_pw_qpolynomial* maxSize;
};

typedef struct WorkingSetSize WorkingSetSize;

struct ProgramCharacteristics {
	int L1Fit; // #working sets that fit in L1 cache
	int L2Fit; // #working sets that fit in L2 cache
	int L3Fit; // #working sets that fit in L3 cache
	int MemFit;
	int datatypeSize; // size of datatype of arrays
	long L1DataSetSize;
	long L2DataSetSize;
	long L3DataSetSize;
	long MemDataSetSize;
};

typedef struct ProgramCharacteristics ProgramCharacteristics;

struct MinMaxTuple {
	long min;
	long max;
};

typedef struct MinMaxTuple MinMaxTuple;

void GetSystemAndProgramCharacteristics(SystemConfig* systemConfig,
	ProgramCharacteristics* programChar);
void InitializeProgramCharacteristics(ProgramCharacteristics* programChar);
void UpdateProgramCharacteristics(long size, SystemConfig* systemConfig,
	ProgramCharacteristics* programChar);

struct ArgComputeWorkingSetSizesForDependence {
	pet_scop *scop;
	vector<WorkingSetSize*>* workingSetSizes;
};

typedef struct ArgComputeWorkingSetSizesForDependence  ArgComputeWorkingSetSizesForDependence;

void ComputeDataReuseWorkingSets(UserInput *userInput, Config *config);
pet_scop* ParseScop(isl_ctx* ctx, const char *fileName);
isl_union_map* ComputeDataDependences(isl_ctx* ctx, pet_scop* scop,
	Config *config);
isl_stat ComputeWorkingSetSizesForDependence(isl_map* dep, void *user);
vector<WorkingSetSize*>* ComputeWorkingSetSizesForDependences(
	isl_union_map *dependences,
	pet_scop *scop);
isl_stat ComputeWorkingSetSizesForDependenceBasicMap(isl_basic_map* dep,
	void *user);
isl_union_pw_qpolynomial* ComputeDataSetSize(isl_basic_set* sourceDomain,
	isl_set* source, isl_set* target, pet_scop* scop);
isl_union_pw_qpolynomial* ComputeDataSetSize(isl_union_set* WS,
	isl_union_map *may_reads, isl_union_map *may_writes);
void FreeWorkingSetSizes(vector<WorkingSetSize*>* workingSetSizes);
void PrintWorkingSetSizes(vector<WorkingSetSize*>* workingSetSizes);
void SimplifyWorkingSetSizesInteractively(vector<WorkingSetSize*>* workingSetSizes,
	UserInput *userInput, Config *config);
void SimplifyWorkingSetSizes(vector<WorkingSetSize*>* workingSetSizes,
	UserInput *userInput, Config *config);
string SimplifyUnionPwQpolynomial(isl_union_pw_qpolynomial* size,
	unordered_map<string, int>* paramValues);
unordered_map<string, int>* GetParameterValues(vector<WorkingSetSize*>* workingSetSizes);
int findInParamsMap(unordered_map<string, int>* map, string key);
long ExtractIntegerFromUnionPwQpolynomial(isl_union_pw_qpolynomial* poly);
string GetParameterValuesString(unordered_map<string, int>* paramValues);
isl_union_map* ComputeDataDependences(isl_union_map *source,
	isl_union_map *target, isl_schedule* schedule);
void OrchestrateDataReuseComputation(int argc, char **argv);
string ExtractFileName(string fileName);
bool IsUniqueDependence(vector<MinMaxTuple*> *minMaxTupleVector,
	long min, long max);
void FreeMinMaxTupleVector(vector<MinMaxTuple*> *minMaxTupleVector);
long ConvertStringToLong(string sizeStr);
isl_set* ConstructContextEquatingParametersToConstants(
	isl_space* space, unordered_map<string, int>* paramValues);
isl_union_map* SimplifyUnionMap(isl_union_map* map,
	unordered_map<string, int>* paramValues);
/* Function header declarations end */

int main(int argc, char **argv) {
	OrchestrateDataReuseComputation(argc, argv);
	return 0;
}

void OrchestrateDataReuseComputation(int argc, char **argv) {
	string fileName = "../apps/padded_conv_fp_stride_1_libxsmm_core2.c";

	UserInput *userInput = new UserInput;
	ReadUserInput(argc, argv, userInput);

	Config *config = NULL;

	if (!userInput->configFile.empty()) {
		config = new Config;
		ReadConfig(userInput->configFile, config);
		if (DEBUG) {
			PrintConfig(config);
		}
	}


	ComputeDataReuseWorkingSets(userInput, config);

	if (!userInput->interactive) {
		FreeConfig(config);
	}

	delete userInput;
}


void ComputeDataReuseWorkingSets(UserInput *userInput, Config *config) {
	isl_ctx* ctx = isl_ctx_alloc_with_pet_options();
	pet_scop *scop = ParseScop(ctx, userInput->inputFile.c_str());
	isl_union_map* dependences = ComputeDataDependences(ctx, scop, config);

	vector<WorkingSetSize*>* workingSetSizes =
		ComputeWorkingSetSizesForDependences(dependences, scop);

	if (DEBUG) {
		PrintWorkingSetSizes(workingSetSizes);
	}

	/*TODO: Deduplicate results*/
	if (userInput->interactive) {
		SimplifyWorkingSetSizesInteractively(workingSetSizes,
			userInput, config);
	}
	else {
		SimplifyWorkingSetSizes(workingSetSizes, userInput, config);
	}

	FreeWorkingSetSizes(workingSetSizes);
	isl_union_map_free(dependences);
	pet_scop_free(scop);
	isl_ctx_free(ctx);
}


vector<WorkingSetSize*>* ComputeWorkingSetSizesForDependences(
	isl_union_map *dependences,
	pet_scop *scop) {
	/*TODO: The following code works for perfectly nested loops
	only. It needs to be extended to cover data dependences that span
	across loops*/

	/* Here we assume that only may_dependences will be present because
	ComputeDataDependences() function is specifying only may_read,
	and may_write references */

	if (DEBUG) {
		PrintUnionMap(dependences);
	}

	vector<WorkingSetSize*>* workingSetSizes =
		new vector<WorkingSetSize*>();
	ArgComputeWorkingSetSizesForDependence* arg =
		(ArgComputeWorkingSetSizesForDependence*)malloc(
			sizeof(ArgComputeWorkingSetSizesForDependence));
	arg->scop = scop;
	arg->workingSetSizes = workingSetSizes;
	isl_union_map_foreach_map(dependences,
		&ComputeWorkingSetSizesForDependence, arg);

	return workingSetSizes;
}

isl_stat ComputeWorkingSetSizesForDependence(isl_map* dep, void *user) {
	isl_map_foreach_basic_map(dep,
		&ComputeWorkingSetSizesForDependenceBasicMap,
		user);
	return isl_stat_ok;
}

isl_stat ComputeWorkingSetSizesForDependenceBasicMap(isl_basic_map* dep,
	void *user) {
	ArgComputeWorkingSetSizesForDependence* arg =
		(ArgComputeWorkingSetSizesForDependence*)user;
	pet_scop *scop = arg->scop;
	vector<WorkingSetSize*>* workingSetSizes = arg->workingSetSizes;

	isl_basic_set* sourceDomain = isl_basic_map_domain(
		isl_basic_map_copy(dep));

	isl_set* source = isl_basic_set_lexmin(
		isl_basic_set_copy(sourceDomain));

	isl_set* target = isl_set_apply(isl_set_copy(source),
		isl_map_from_basic_map(dep));

	isl_set* minTarget = isl_set_lexmin(isl_set_copy(target));
	isl_set* maxTarget = isl_set_lexmax(isl_set_copy(target));

	isl_union_pw_qpolynomial* minWSSize =
		ComputeDataSetSize(sourceDomain, source, minTarget, scop);

	isl_union_pw_qpolynomial* maxWSSize =
		ComputeDataSetSize(sourceDomain, source, maxTarget, scop);

	WorkingSetSize* workingSetSize =
		(WorkingSetSize*)malloc(sizeof(WorkingSetSize));
	workingSetSize->dependence = dep;
	workingSetSize->source = source;
	workingSetSize->target = target;
	workingSetSize->minTarget = minTarget;
	workingSetSize->maxTarget = maxTarget;
	workingSetSize->minSize = minWSSize;
	workingSetSize->maxSize = maxWSSize;
	workingSetSizes->push_back(workingSetSize);

	isl_basic_set_free(sourceDomain);
	return isl_stat_ok;
}

isl_union_pw_qpolynomial* ComputeDataSetSize(isl_basic_set* sourceDomain,
	isl_set* source, isl_set* target, pet_scop* scop) {

	/* itersUptoSourceExcludingSource := sourceDomain << source */
	isl_union_set* itersUptoSourceExcludingSource =
		isl_union_map_domain(
			isl_union_set_lex_lt_union_set(
				isl_union_set_from_basic_set(
					isl_basic_set_copy(sourceDomain)),
				isl_union_set_from_set(isl_set_copy(source))));

	/* itersUptoTargetIncludingTarget := sourceDomain <<= target */
	isl_union_set* itersUptoTargetIncludingTarget =
		isl_union_map_domain(
			isl_union_set_lex_le_union_set(
				isl_union_set_from_basic_set(
					isl_basic_set_copy(sourceDomain)),
				isl_union_set_from_set(isl_set_copy(target))));

	/* WS :=  itersUptoTargetIncludingTarget - itersUptoSourceExcludingSource */

	isl_union_set* WS =
		isl_union_set_subtract(
			itersUptoTargetIncludingTarget,
			itersUptoSourceExcludingSource);

	isl_union_map *may_reads = pet_scop_get_may_reads(scop);
	isl_union_map *may_writes = pet_scop_get_may_writes(scop);

	isl_union_pw_qpolynomial* WSSize = ComputeDataSetSize(
		WS, may_reads, may_writes);
	isl_union_set_free(WS);
	isl_union_map_free(may_reads);
	isl_union_map_free(may_writes);
	return WSSize;
}

isl_union_pw_qpolynomial* ComputeDataSetSize(isl_union_set* WS,
	isl_union_map *may_reads, isl_union_map *may_writes) {
	isl_union_set* readSet =
		isl_union_set_apply(isl_union_set_copy(WS),
			isl_union_map_copy(may_reads));
	isl_union_set* writeSet =
		isl_union_set_apply(isl_union_set_copy(WS),
			isl_union_map_copy(may_writes));
	isl_union_set* dataSet = isl_union_set_union(readSet, writeSet);
	return isl_union_set_card(dataSet);
}

string ExtractFileName(string fileName) {
	string returnFileName;
	size_t found = fileName.find_last_of("/\\");
	if (found == string::npos) {
		returnFileName = fileName;
	}
	else {
		returnFileName = fileName.substr(found + 1);
	}

	return returnFileName;
}
void SimplifyWorkingSetSizes(vector<WorkingSetSize*>* workingSetSizes,
	UserInput *userInput, Config *config) {
	string suffix = "_ws_stats.csv";
	ofstream file;
	string configFileName = ExtractFileName(userInput->configFile);
	string fullFileName = userInput->inputFile + configFileName
		+ suffix;
	file.open(fullFileName);

	if (file.is_open()) {
		if (DEBUG) {
			cout << "Writing to file " << fullFileName << endl;
		}
	}
	else {
		cout << "Could not open the file: " << fullFileName << endl;
		exit(1);
	}

	ProgramCharacteristics* programChar = new ProgramCharacteristics;
	vector<MinMaxTuple*> *minMaxTupleVector = new vector<MinMaxTuple*>();

	programChar->datatypeSize = config->datatypeSize;
	file << "params,L1,L2,L3,Mem,L1DataSetSize,L2DataSetSize,L3DataSetSize,MemDataSetSize" << endl;
	for (int i = 0; i < config->programParameterVector->size(); i++) {
		InitializeProgramCharacteristics(programChar);
		unordered_map<string, int>* paramValues =
			config->programParameterVector->at(i);
		file << GetParameterValuesString(paramValues) << ",";

		for (int i = 0; i < workingSetSizes->size(); i++) {
			isl_union_pw_qpolynomial* minSizePoly =
				workingSetSizes->at(i)->minSize;
			string minSize = SimplifyUnionPwQpolynomial(
				minSizePoly,
				paramValues);

			isl_union_pw_qpolynomial* maxSizePoly =
				workingSetSizes->at(i)->maxSize;
			string maxSize = SimplifyUnionPwQpolynomial(
				maxSizePoly,
				paramValues);

			if (!minSize.empty() && !maxSize.empty()) {
				long min = ConvertStringToLong(minSize);
				long max = ConvertStringToLong(maxSize);

				if (IsUniqueDependence(minMaxTupleVector, min, max)) {
					UpdateProgramCharacteristics(min, config->systemConfig, programChar);
					UpdateProgramCharacteristics(max, config->systemConfig, programChar);
				}
			}

		}

		FreeMinMaxTupleVector(minMaxTupleVector);

		file << programChar->L1Fit << "," << programChar->L2Fit << ","
			<< programChar->L3Fit << "," << programChar->MemFit << ","
			<< programChar->L1DataSetSize << ","
			<< programChar->L2DataSetSize << ","
			<< programChar->L3DataSetSize << ","
			<< programChar->MemDataSetSize
			<< endl;
	}

	file.close();

	delete minMaxTupleVector;
	delete programChar;
}

bool IsUniqueDependence(vector<MinMaxTuple*> *minMaxTupleVector,
	long min, long max) {
	if (min != -1 && max != -1) {
		for (int i = 0; i < minMaxTupleVector->size(); i++) {
			if (minMaxTupleVector->at(i)->min == min &&
				minMaxTupleVector->at(i)->max == max) {
				return false;
			}
		}

		MinMaxTuple* minMaxTuple = new MinMaxTuple;
		minMaxTuple->min = min;
		minMaxTuple->max = max;
		minMaxTupleVector->push_back(minMaxTuple);
		return true;
	}
	else {
		return false;
	}
}

void FreeMinMaxTupleVector(vector<MinMaxTuple*> *minMaxTupleVector) {
	for (int i = 0; i < minMaxTupleVector->size(); i++) {
		delete minMaxTupleVector->at(i);
	}

	minMaxTupleVector->clear();
}

void SimplifyWorkingSetSizesInteractively(vector<WorkingSetSize*>* workingSetSizes,
	UserInput *userInput, Config *config) {
	string fileName = userInput->inputFile;

	if (DEBUG) {
		cout << "Number of working set sizes: " << workingSetSizes->size()
			<< endl;
	}

	string DIR = "stats";
	string suffix;

	if (config == NULL) {
		suffix = "_ws_stats.tsv";
	}
	else {
		suffix = ExtractFileName(userInput->configFile) + "_ws_stats.tsv";
	}

	ofstream file;
	string fullFileName = fileName + suffix;
	file.open(fullFileName);

	if (file.is_open()) {
		if (DEBUG) {
			cout << "Writing to file " << fullFileName << endl;
		}
	}
	else {
		cout << "Could not open the file: " << fullFileName << endl;
		exit(1);
	}

	SystemConfig* systemConfig;

	if (config == NULL) {
		systemConfig = (SystemConfig*)malloc(sizeof(SystemConfig));
	}
	else {
		systemConfig = config->systemConfig;
	}

	ProgramCharacteristics* programChar =
		(ProgramCharacteristics*)malloc(sizeof(ProgramCharacteristics));
	vector<MinMaxTuple*> *minMaxTupleVector = new vector<MinMaxTuple*>();

	if (config == NULL) {
		GetSystemAndProgramCharacteristics(systemConfig, programChar);
	}
	else {
		programChar->datatypeSize = config->datatypeSize;
	}

	char answer = 'Y';
	int count = 0;
	int size = 0;
	if (config) {
		size = config->programParameterVector->size();
	}

	while ((config == NULL && answer == 'Y') || (config && count < size)) {
		InitializeProgramCharacteristics(programChar);
		unordered_map<string, int>* paramValues;

		if (config == NULL) {
			paramValues = GetParameterValues(workingSetSizes);
		}
		else {
			paramValues = config->programParameterVector->at(count);
		}

		file << "Parameters: " << GetParameterValuesString(paramValues)
			<< endl;
		file << "dependence \t source \t min_target \t max_target \t min_WS_size \t max_WS_size\n";
		for (int i = 0; i < workingSetSizes->size(); i++) {
			isl_union_pw_qpolynomial* minSizePoly =
				workingSetSizes->at(i)->minSize;
			string minSize = SimplifyUnionPwQpolynomial(
				minSizePoly,
				paramValues);

			isl_union_pw_qpolynomial* maxSizePoly =
				workingSetSizes->at(i)->maxSize;
			string maxSize = SimplifyUnionPwQpolynomial(
				maxSizePoly,
				paramValues);

			if (!minSize.empty() && !maxSize.empty()) {
				long min = ConvertStringToLong(minSize);
				long max = ConvertStringToLong(maxSize);

				if (IsUniqueDependence(minMaxTupleVector, min, max)) {
					file << isl_basic_map_to_str(
						workingSetSizes->at(i)->dependence)
						<< "\t";
					file << isl_set_to_str(workingSetSizes->at(i)->source)
						<< "\t";
					file << isl_set_to_str(workingSetSizes->at(i)->minTarget)
						<< "\t";
					file << isl_set_to_str(workingSetSizes->at(i)->maxTarget)
						<< "\t";
					file << minSize << "\t";
					file << maxSize << endl;

					UpdateProgramCharacteristics(min, systemConfig, programChar);
					UpdateProgramCharacteristics(max, systemConfig, programChar);
				}
			}
		}

		file << "#reuses in L1, L2, L3:"
			<< "\t" << programChar->L1Fit
			<< "\t" << programChar->L2Fit
			<< "\t" << programChar->L3Fit
			<< "\t" << programChar->MemFit
			<< "\t" << programChar->L1DataSetSize
			<< "\t" << programChar->L2DataSetSize
			<< "\t" << programChar->L3DataSetSize
			<< "\t" << programChar->MemDataSetSize
			<< endl;

		FreeMinMaxTupleVector(minMaxTupleVector);

		if (config == NULL) {
			paramValues->clear();
			delete paramValues;
			cout << "Would like to enter a new set of parameters? [Y/N]"
				<< endl;
			cin >> answer;
		}
		else {
			count++;
		}
	}
	file.close();

	if (config == NULL) {
		free(systemConfig);
	}

	delete minMaxTupleVector;
	free(programChar);
}

void GetSystemAndProgramCharacteristics(SystemConfig* systemConfig,
	ProgramCharacteristics* programChar) {
	cout << "Enter L1, L2, L3 cache sizes (in bytes): ";
	cin >> systemConfig->L1;
	cin >> systemConfig->L2;
	cin >> systemConfig->L3;

	cout << "Enter the datatype size (in bytes): ";
	cin >> programChar->datatypeSize;
}

void InitializeProgramCharacteristics(ProgramCharacteristics* programChar) {
	programChar->L1Fit = 0;
	programChar->L2Fit = 0;
	programChar->L3Fit = 0;
	programChar->MemFit = 0;
	programChar->L1DataSetSize = 0;
	programChar->L2DataSetSize = 0;
	programChar->L3DataSetSize = 0;
	programChar->MemDataSetSize = 0;
}

long ConvertStringToLong(string sizeStr) {
	try {
		return stol(sizeStr, nullptr, 10);
	}
	catch (const invalid_argument) {
		if (DEBUG) {
			cerr << "Invalid argument while updating" << endl;
		}

		return -1;
	}
}

void UpdateProgramCharacteristics(long size,
	SystemConfig* systemConfig,
	ProgramCharacteristics* programChar) {
	size = size * programChar->datatypeSize;
	if (size != -1) {
		if (size <= systemConfig->L1) {
			programChar->L1Fit += 1;
			programChar->L1DataSetSize += size;
		}
		else if (size <= systemConfig->L2) {
			programChar->L2Fit += 1;
			programChar->L2DataSetSize += size;
		}
		else if (size <= systemConfig->L3) {
			programChar->L3Fit += 1;
			programChar->L3DataSetSize += size;
		}
		else {
			programChar->MemFit += 1;
			programChar->MemDataSetSize += size;
		}
	}
}

unordered_map<string, int>* GetParameterValues(vector<WorkingSetSize*>* workingSetSizes) {
	unordered_map<string, int>* paramValues =
		new unordered_map<string, int>();

	if (workingSetSizes->size() > 0) {
		isl_union_pw_qpolynomial* repPoly =
			workingSetSizes->at(0)->minSize;
		isl_space* space = isl_union_pw_qpolynomial_get_space(repPoly);
		isl_size numParams =
			isl_space_dim(space, isl_dim_param);

		cout << "Enter values for the following parameters:" << endl;
		for (int j = 0; j < numParams; j++) {
			string name(isl_space_get_dim_name(
				space, isl_dim_param, (unsigned)j));
			cout << name << " ";
		}

		cout << endl;

		for (int j = 0; j < numParams; j++) {
			string name(isl_space_get_dim_name(
				space, isl_dim_param, (unsigned)j));
			int val;
			cin >> val;
			paramValues->insert({ {name, val} });
		}

		isl_space_free(space);
	}

	return paramValues;
}

string GetParameterValuesString(unordered_map<string, int>* paramValues) {
	string params = "";
	for (auto i : *paramValues) {
		params += i.first + " = " + to_string(i.second) + " ";
	}

	return params;
}

isl_set* ConstructContextEquatingParametersToConstants(
	isl_space* space, unordered_map<string, int>* paramValues) {
	isl_set* context = isl_set_universe(space);

	/*Add constraints now*/
	isl_constraint *c;
	isl_local_space *ls;
	space = isl_set_get_space(context);
	ls = isl_local_space_from_space(isl_space_copy(space));

	isl_size numParams =
		isl_set_dim(context, isl_dim_param);

	for (int j = 0; j < numParams; j++) {
		string name(isl_space_get_dim_name(
			space, isl_dim_param, (unsigned)j));
		int val = findInParamsMap(paramValues, name);

		c = isl_constraint_alloc_equality(
			isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(
			c, isl_dim_param, j, -1);
		c = isl_constraint_set_constant_si(c, val);
		context = isl_set_add_constraint(context, c);
	}

	isl_space_free(space);
	isl_local_space_free(ls);
	return context;
}

isl_union_map* SimplifyUnionMap(isl_union_map* map,
	unordered_map<string, int>* paramValues) {
	isl_set* context = ConstructContextEquatingParametersToConstants(
		isl_union_map_get_space(map), paramValues);
	return isl_union_map_gist_params(isl_union_map_copy(map), context);
}

string SimplifyUnionPwQpolynomial(isl_union_pw_qpolynomial* size,
	unordered_map<string, int>* paramValues) {
	isl_set* context = ConstructContextEquatingParametersToConstants(
		isl_union_pw_qpolynomial_get_space(size), paramValues);
	isl_union_pw_qpolynomial* gistSize =
		isl_union_pw_qpolynomial_gist_params(
			isl_union_pw_qpolynomial_copy(size),
			context);

	long sizeInteger = ExtractIntegerFromUnionPwQpolynomial(gistSize);
	if (DEBUG) {
		cout << "gistSize: " << sizeInteger << endl;
	}

	string sizeString;
	if (sizeInteger != -1) {
		sizeString = to_string(sizeInteger);
	}

	return sizeString;
}

long ExtractIntegerFromUnionPwQpolynomial(
	isl_union_pw_qpolynomial* polynomial)
{
	/*The string representation typically will be of the following kind:
	[pad_w, ifwp, pad_h, ifhp, nIfm, ofwp, ofhp, nOfm, kw, kh, nImg, ofh, ofw] -> { 290 }

	This function extracts the integer -- in this case 290 from the string
	representation such as the above.*/

	long val = -1;
	if (DEBUG) {
		cout << "Converting: " << endl;
		PrintUnionPwQpolynomial(polynomial);
	}
	string poly(isl_union_pw_qpolynomial_to_str(polynomial));

	string openingBrace = "{";
	string closingBrace = "}";

	size_t begin = -1, end = -1;
	size_t found = poly.find(openingBrace);
	if (found != string::npos) {
		begin = found + 1;
	}

	found = poly.find(closingBrace);
	if (found != string::npos) {
		end = found - 1;
	}

	if (begin != -1 && end != -1 && begin < end) {
		string valStr = poly.substr(begin, end - begin + 1);
		try {
			val = stol(valStr, nullptr, 10);
			if (DEBUG) {
				cout << "Converted string " << valStr << " to integer "
					<< val << endl;
			}

			if (IGNORE_WS_SIZE_ONE && val == 1) {
				val = -1;
			}

			return val;
		}
		catch (const invalid_argument) {
			if (DEBUG) {
				cerr << "Invalid argument" << endl;
			}
		}
	}

	return val;
}

int findInParamsMap(unordered_map<string, int>* map, string key) {
	unordered_map<string, int>::const_iterator find =
		map->find(key);

	if (find == map->end()) {
		cout << "Parameter value not found for " << key << endl;
		exit(1);
		return -1;
	}
	else {
		return find->second;
	}
}

void PrintWorkingSetSizes(vector<WorkingSetSize*>* workingSetSizes) {
	cout << "Number of working set sizes: " << workingSetSizes->size()
		<< endl;
	for (int i = 0; i < workingSetSizes->size(); i++) {
		cout << "*********************************************" << endl;
		cout << "*********************************************" << endl;
		cout << "dependence: " << endl;
		PrintBasicMap(workingSetSizes->at(i)->dependence);

		cout << "source: " << endl;
		PrintSet(workingSetSizes->at(i)->source);

		cout << "target: " << endl;
		PrintSet(workingSetSizes->at(i)->target);

		cout << "MinSize: " << endl;
		PrintUnionPwQpolynomial(workingSetSizes->at(i)->minSize);

		cout << "MaxSize: " << endl;
		PrintUnionPwQpolynomial(workingSetSizes->at(i)->maxSize);
		cout << "*********************************************" << endl;
		cout << "*********************************************" << endl;
		cout << endl;
	}
}


void FreeWorkingSetSizes(vector<WorkingSetSize*>* workingSetSizes) {
	for (int i = 0; i < workingSetSizes->size(); i++) {
		if (workingSetSizes->at(i) == NULL) {
			continue;
		}

		if (workingSetSizes->at(i)->dependence) {
			isl_basic_map_free(workingSetSizes->at(i)->dependence);
		}

		if (workingSetSizes->at(i)->source) {
			isl_set_free(workingSetSizes->at(i)->source);
		}

		if (workingSetSizes->at(i)->target) {
			isl_set_free(workingSetSizes->at(i)->target);
		}

		if (workingSetSizes->at(i)->minTarget) {
			isl_set_free(workingSetSizes->at(i)->minTarget);
		}

		if (workingSetSizes->at(i)->maxTarget) {
			isl_set_free(workingSetSizes->at(i)->maxTarget);
		}

		if (workingSetSizes->at(i)->minSize) {
			isl_union_pw_qpolynomial_free(
				workingSetSizes->at(i)->minSize);
		}

		if (workingSetSizes->at(i)->maxSize) {
			isl_union_pw_qpolynomial_free(
				workingSetSizes->at(i)->maxSize);
		}

		free(workingSetSizes->at(i));
	}

	delete workingSetSizes;
}

isl_union_map* ComputeDataDependences(isl_ctx* ctx, pet_scop* scop,
	Config *config) {
	/*TODO: Print the array because of which the dependence is formed -
	use "full" dependence structrues*/
	isl_schedule* schedule = pet_scop_get_schedule(scop);
	isl_union_map *may_reads = pet_scop_get_may_reads(scop);
	isl_union_map *may_writes = pet_scop_get_may_writes(scop);

	if (config && config->programParameterVector &&
		config->programParameterVector->size() == 1) {
		if (DEBUG) {
			cout << "SIMPLIFYING the read and write union maps" << endl;
		}

		isl_union_map *simple_may_reads = SimplifyUnionMap(may_reads,
			config->programParameterVector->at(0));
		isl_union_map_free(may_reads);
		may_reads = simple_may_reads;

		isl_union_map *simple_may_writes = SimplifyUnionMap(may_writes,
			config->programParameterVector->at(0));
		isl_union_map_free(may_writes);
		may_writes = simple_may_writes;
	}

	// RAR
	isl_union_map* RAR = ComputeDataDependences(may_reads,
		may_reads, schedule);

	// RAW
	isl_union_map* RAW = ComputeDataDependences(may_writes,
		may_reads, schedule);

	// WAR
	isl_union_map* WAR = ComputeDataDependences(may_reads,
		may_writes, schedule);

	// WAW
	isl_union_map* WAW = ComputeDataDependences(may_writes,
		may_writes, schedule);

	isl_union_map_free(may_writes);
	isl_union_map_free(may_reads);
	isl_schedule_free(schedule);

	return isl_union_map_union(RAR,
		isl_union_map_union(RAW, isl_union_map_union(WAR, WAW)));
}

isl_union_map* ComputeDataDependences(isl_union_map *source,
	isl_union_map *target, isl_schedule* schedule)
{
	isl_union_access_info* access_info =
		isl_union_access_info_from_sink(isl_union_map_copy(target));
	access_info = isl_union_access_info_set_may_source(access_info,
		isl_union_map_copy(source));
	isl_union_access_info_set_schedule(access_info,
		isl_schedule_copy(schedule));

	isl_union_flow *deps =
		isl_union_access_info_compute_flow(access_info);

	if (DEBUG) {
		cout << "Dependences are:" << endl;
		if (deps == NULL) {
			cout << "No RAR dependences found" << endl;
		}
		else {
			cout << "Calling PrintUnionAccessInfo" << endl;
			PrintUnionFlow(deps);
		}
	}

	isl_union_map *may_dependences =
		isl_union_flow_get_may_dependence(deps);
	isl_union_flow_free(deps);
	return may_dependences;
}


pet_scop* ParseScop(isl_ctx* ctx, const char *fileName) {
	pet_options_set_autodetect(ctx, 0);
	pet_scop *scop = pet_scop_extract_from_C_source(ctx, fileName, NULL);
	if (DEBUG) {
		PrintScop(ctx, scop);
	}

	return scop;
}



