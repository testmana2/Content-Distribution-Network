#include "MetaServer.h"
#include "MetaCDNReceiver.h"
#include "MetaCDNSender.h"
#include "../Origin/OriginServer.h"
#include "../Shared.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <vector>
#include <math.h>
#include <sys/stat.h>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <ios>

using namespace std;
using namespace web;
using namespace utility;
using namespace http;
using namespace web::http::experimental::listener;
using namespace json;

MetaServer::MetaServer(string file, OriginServer* origin) {
	for(int i=0; i<file.size(); i++)
		if(file[i]==' ') {
			cout<<"MetaServer::MetaServer - file name can't contain a space"<<endl;
			exit(0);
		}
	string dirName = "./MetaData";
	mkdir(dirName.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	m_file = "./MetaData/"+file;
	m_file_timestamp = "./MetaData/"+file+"_timestamp";
	m_version = 0;
	m_version_timestamp = 0;
	m_origin = origin;
	m_nextCDNId = 0;
}

MetaServer::~MetaServer() {
	//clean up
}

void MetaServer::startListening() {
	utility::string_t port = U("5000");
	utility::string_t address = U("http://localhost:");
	address.append(port);
	MetaCDNReceiver::initialize(address, this);
}

void MetaServer::endListening() {
	MetaCDNReceiver::shutDown();
}

int MetaServer::setFssAddr(Address fss) {
	m_FssAddr = fss;
	return 0;
}
int MetaServer::addCdnAddr(Address cdn) { //returns the id # to be associated with the cdn
	m_cdnAddrList.push_back(cdn);
	return m_nextCDNId++;
}

/*------------------*/
/* helper functions */
/*------------------*/

void MetaServer::updateVersion() {
	m_version = (m_version+1)%10;
}

bool compare(const pair<string, string>& a, const pair<string, string>& b) {
		return a.first < b.first;
}

void MetaServer::sortFileList(vector< pair<string, string> >& fileListFromOrigin) {
		sort(fileListFromOrigin.begin(), fileListFromOrigin.end(), compare);
}

double rad2deg(double rad) {
	return (rad * 180 / 3.14159265358979323846);
}

double deg2rad(double deg) {
	return (deg * 3.14159265358979323846 / 180);
}

double MetaServer::calculateDistance(Address addr1, Address addr2) { //in miles
	double theta, dist;
	pair<double, double> loc1 =  addr1.latLng;
	pair<double, double> loc2 = addr2.latLng;
	theta = loc1.second - loc2.second;
	dist = sin(deg2rad(loc1.first)) * sin(deg2rad(loc2.first)) + cos(deg2rad(loc1.first)) * cos(deg2rad(loc2.first)) * cos(deg2rad(theta));
	dist = acos(dist);
	dist = rad2deg(dist);
	dist = dist * 60 * 1.1515;
	return dist;
}


/*-------------------------------*/
/* for communication with Origin */
/*-------------------------------*/

Address MetaServer::parseAddress(const string& line) { // "23,45,1.1.1.1" -> { latLng: pair<23, 45>, ipAddr:"1.1.1.1" }
	string latStr = "", lngStr = "", ipAddrStr = "";
	double lat, lng;
	int idx = 0;

	while(idx<line.size() && line[idx]!=',') {
		latStr += line[idx];
		idx++;
	}
	lat = strtod(latStr.c_str(), NULL);

	idx++;
	while(idx<line.size() && line[idx]!=',') {
		lngStr += line[idx];
		idx++;
	}
	lng = strtod(lngStr.c_str(), NULL);

	idx++;
	while(idx<line.size()) {
		ipAddrStr += line[idx];
		idx++;
	}

	Address result(make_pair(lat, lng), ipAddrStr);
	return result;
}

void parseLine(const string& line, string& fileName, string& fileHash, vector<Address>& cdnList) {
	string currWord = "";
	int wordCount = 0;
	for(int i=0; i<line.size(); i++) {
		if(line[i] != ' ') {
			currWord += line[i];
		} else {
			if(wordCount==0)
				fileName = currWord;
			else if(wordCount==1)
				fileHash = currWord;
			else {
				Address addr = MetaServer::parseAddress(currWord);
				cdnList.push_back(addr);
			}
			wordCount++;
			currWord = "";
		}
	}
	if(wordCount==1) //when the cdnList is empty
		fileHash = currWord;
	else if(wordCount>1) {
		Address addr = MetaServer::parseAddress(currWord);
		cdnList.push_back(addr);
	}
}

vector<Address> MetaServer::getCdnsThatContainFile(string fileName) {
	ifstream file;
	string currFileName = m_file+"_v" + char('0'+m_version);
	file.open(currFileName.c_str());
	if(!file.is_open()) {
		cout<<"MetaServer::getCdnsThatContainFile - file not opened"<<endl;
		exit(0);
	}

	string currLine;
	while(getline(file, currLine)) {
		if(currLine.substr(0, fileName.size())==fileName && currLine[fileName.size()]==' ') {
			string fileName="", fileHash="";
			vector<Address> cdnsThatContainFile;
			parseLine(currLine, fileName, fileHash, cdnsThatContainFile);
			return cdnsThatContainFile;
		}
	}
	vector<Address> emptyVec;
	return emptyVec;
}

bool MetaServer::CDN_load_OK(Address cdnAddr) {
	return true; //always true for now
}

Address MetaServer::getClosestCDN(const vector<Address>& cdnAddrList, Address clientAddr) { //also checks load inside
	double minDist = -1;
	int minIdx = -1;

	for(int i=0; i<cdnAddrList.size(); i++) {
		if(!CDN_load_OK(cdnAddrList[i]))
			continue;
		double currDist = calculateDistance(cdnAddrList[i], clientAddr);
		if(minDist < 0 || currDist<minDist) {
			minDist = currDist;
			minIdx = i;
		}
	}

	if(minIdx==-1) { //all the CDNs are heavily loaded or cdn list is empty
		Address errResult(make_pair(0.0, 0.0), "");
		return errResult;
	}

	return cdnAddrList[minIdx];
}


bool MetaServer::isCDN_closerThanFSS(Address cdnAddr, Address clientAddr) {
	double distToCDN = calculateDistance(cdnAddr, clientAddr);
	double distToFSS = calculateDistance(m_FssAddr, clientAddr);
	return distToCDN <= distToFSS;
}

vector< pair<string, Address> > MetaServer::processListFromOriginDownload(const vector< pair<string, string> >& listFromClientApp, Address clientAddr) {
	vector< pair<string, Address> > result;
	ifstream file;
	string fileName = m_file+"_v" + char('0'+m_version);
	file.open(fileName.c_str());
	if(!file.is_open()) {
		cout<<"MetaServer::processListFromOriginDownload - file not opened"<<endl;
		exit(0);
	}
	unordered_map<string, string> clientNameToHashMap;
	for(int i=0; i<listFromClientApp.size(); i++) {
		clientNameToHashMap[listFromClientApp[i].first] = listFromClientApp[i].second;
	}

	string currLine;
	while(getline(file, currLine)) {
		string fileName="", fileHash="";
		vector<Address> cdnsThatContainFile;
		parseLine(currLine, fileName, fileHash, cdnsThatContainFile);
		if(clientNameToHashMap.count(fileName)>0 && clientNameToHashMap[fileName]==fileHash) //no need to download since name and hash match
			continue;
		Address candidateCdnAddr = getClosestCDN(cdnsThatContainFile, clientAddr);
		if(!(candidateCdnAddr.latLng.first==0.0 && candidateCdnAddr.latLng.second==0.0) && isCDN_closerThanFSS(candidateCdnAddr, clientAddr)) { //cdn is closer than fss
			result.push_back(make_pair(fileName, candidateCdnAddr));
		} else {
			Address closestCDN = getClosestCDN(m_cdnAddrList, clientAddr);
			result.push_back(make_pair(fileName, closestCDN));
		}
	}

	return result;
}

vector< pair<string, Address> > MetaServer::processListFromOriginUpload(const vector< pair<string, string> >& listFromClientApp, Address clientAddr) {
	vector< pair<string, Address> > result;
	ifstream file;
	string fileName = m_file+"_v" + char('0'+m_version);
	file.open(fileName.c_str());
	if(!file.is_open()) {
		cout<<"MetaServer::processListFromOriginUpload - file not opened"<<endl;
		exit(0);
	}
	unordered_map<string, string> clientNameToHashMap;
	for(int i=0; i<listFromClientApp.size(); i++) {
		clientNameToHashMap[listFromClientApp[i].first] = listFromClientApp[i].second;
	}

	Address closestCdnAddr = getClosestCDN(m_cdnAddrList, clientAddr);

	//add the files that exist in both FSS and client local and need to be updated
	string currLine;
	while(getline(file, currLine)) {
		string fileName="", fileHash="";
		vector<Address> cdnsThatContainFile;
		parseLine(currLine, fileName, fileHash, cdnsThatContainFile);
		if(clientNameToHashMap.count(fileName)==0) {
			continue;
		} else if(clientNameToHashMap[fileName]==fileHash) { //no need to download since name and hash match
			clientNameToHashMap.erase(fileName);
			continue;
		}
		result.push_back(make_pair(fileName, closestCdnAddr));
		clientNameToHashMap.erase(fileName); //erase to find out which client's files are new to FSS
	}

	//add the files that client has but FSS doesn't
	unordered_map<string, string>::iterator itr = clientNameToHashMap.begin();
	while(itr != clientNameToHashMap.end()) {
		result.push_back(make_pair(itr->first, closestCdnAddr));
		itr++;
	}

	return result;
}


/*----------------------------*/
/* for communication with CDN */
/*----------------------------*/

int MetaServer::deleteMetaEntry(string fileName) {
	string currFileName = m_file+"_v" + char('0'+m_version);
	updateVersion();
	string newFileName = m_file+"_v" + char('0'+m_version);
	ifstream fileIn(currFileName.c_str());
	if(!fileIn) {
		cout<<"MetaServer::deleteMetaEntry - file not opened"<<endl;
		return -1;
	}
	ofstream fileOut(newFileName.c_str());
	if(!fileOut) {
		cout<<"MetaServer::deleteMetaEntry - file not opened"<<endl;
		fileIn.close();
		return -1;
	}
	bool found = false;
	string currLine;
	while(getline(fileIn, currLine)) {
		if(currLine.substr(0, fileName.size())==fileName && currLine[fileName.size()]==' ') {
			found = true;
			continue;
		}
		fileOut << currLine+"\n";
	}

	fileIn.close();
	fileOut.close();

	if(found==false) {
		cout<<"MetaServer::deleteMetaEntry - "+fileName+" is not found for delete operation"<<endl;
		return -1;
	}
	return 0;
}

string constructLine(string fileName, const string& fileHash, const vector<Address>& CdnAddrList) {
	string result = "";
	result += fileName + " " + fileHash;
	for(int i=0; i<CdnAddrList.size(); i++)
		result += " " + to_string(CdnAddrList[i].latLng.first) + "," + to_string(CdnAddrList[i].latLng.second) + "," + CdnAddrList[i].ipAddr;
	return result;
}

bool MetaServer::doesExist(string fileName) {
	string currFileName = m_file+"_v" + char('0'+m_version);
	ifstream fileIn(currFileName.c_str());
	if(!fileIn.is_open()) {
		cout<<"MetaServer::doesExist - file not opened"<<endl;
		return false;
	}
	bool found = false;
	string currLine;
	while(getline(fileIn, currLine)) {
		if(currLine.substr(0, fileName.size())==fileName && currLine[fileName.size()]==' ') {
			found = true;
			break;
		}
	}

	fileIn.close();
	return found;
}

int MetaServer::addNewMetaEntry(string fileName, const string& fileHash, const vector<Address>& CdnAddrList) {
	if(doesExist(fileName)) {
		cout<<"MetaServer::addNewMetaEntry - " + fileName + " already exists"<<endl;
		return -1;
	}
	string currFileName = m_file+"_v" + char('0'+m_version);
	ofstream file(currFileName.c_str(), ios_base::app | ios_base::out);
	if(!file.is_open()) {
		cout<<"MetaServer::addNewMetaEntry - file not opened"<<endl;
		return -1;
	}
	string lineToAdd = constructLine(fileName, fileHash, CdnAddrList);
	file << lineToAdd + "\n";
	file.close();
	return 0;
}

int MetaServer::updateMetaEntry(string fileName, const string& fileHash, const vector<Address>& CdnAddrList) {
	if(deleteMetaEntry(fileName) != 0)
		return -1;
	if(addNewMetaEntry(fileName, fileHash, CdnAddrList) != 0)
		return -1;
	return 0;
}

int MetaServer::addCdnToMetaEntry(string fileName, Address cdnAddr) {
	string currFileName = m_file+"_v" + char('0'+m_version);
	updateVersion();
	string newFileName = m_file+"_v" + char('0'+m_version);
	ifstream fileIn(currFileName.c_str());
	if(!fileIn) {
		cout<<"MetaServer::addCdnToMetaEntry - file not opened"<<endl;
		return -1;
	}
	ofstream fileOut(newFileName.c_str());
	if(!fileOut) {
		cout<<"MetaServer::addCdnToMetaEntry - file not opened"<<endl;
		fileIn.close();
		return -1;
	}
	bool found = false;
	string currLine;
	while(getline(fileIn, currLine)) {
		if(currLine.substr(0, fileName.size())==fileName && currLine[fileName.size()]==' ') {
			found = true;
			string tempName, tempHash;
			vector<Address> newCdnList;
			parseLine(currLine, tempName, tempHash, newCdnList);
			bool cdnFound = false;
			for(int i=0; i<newCdnList.size(); i++) { //check if cdn already exists in the list
				if(newCdnList[i].ipAddr==cdnAddr.ipAddr) { //check only ip address for now !! TODO: CDN ID
					cout<<"MetaServer::addCdnToMetaEntry - "+cdnAddr.ipAddr+" already exists in the list"<<endl;
					cdnFound = true;
				}
			}
			if(cdnFound==false) //add new cdn only when it doesn't exist yet
				currLine += " " + to_string(cdnAddr.latLng.first) + "," + to_string(cdnAddr.latLng.second) + "," + cdnAddr.ipAddr;
		}
		fileOut << currLine+"\n";
	}

	fileIn.close();
	fileOut.close();

	if(found==false) {
		cout<<"MetaServer::addCdnToMetaEntry - "+fileName+" is not found for add CDN operation"<<endl;
		return -1;
	}
	return 0;
}

int MetaServer::deleteCdnFromMetaEntry(string fileName, Address cdnAddr) {
	string currFileName = m_file+"_v" + char('0'+m_version);
	updateVersion();
	string newFileName = m_file+"_v" + char('0'+m_version);
	ifstream fileIn(currFileName.c_str());
	if(!fileIn) {
		cout<<"MetaServer::deleteCdnFromMetaEntry - file not opened"<<endl;
		return -1;
	}
	ofstream fileOut(newFileName.c_str());
	if(!fileOut) {
		cout<<"MetaServer::deleteCdnFromMetaEntry - file not opened"<<endl;
		fileIn.close();
		return -1;
	}
	//void parseLine(const string& line, string& fileName, string& fileHash, vector<Address>& cdnList)
	bool fileFound = false;
	bool cdnFound = false;
	string currLine;
	while(getline(fileIn, currLine)) {
		if(currLine.substr(0, fileName.size())==fileName && currLine[fileName.size()]==' ') {
			fileFound = true;
			string tempName, tempHash;
			vector<Address> newCdnList;
			parseLine(currLine, tempName, tempHash, newCdnList);
			for(int i=0; i<newCdnList.size(); i++) { //assuming there is only one entry for each cdn in the list
				if(newCdnList[i].ipAddr==cdnAddr.ipAddr) { //check only ip address for now !! TODO: CDN ID
					newCdnList.erase(newCdnList.begin()+i);
					cdnFound = true;
					break;
				}
			}
			currLine = constructLine(tempName, tempHash, newCdnList);
		}
		fileOut << currLine+"\n";
	}

	fileIn.close();
	fileOut.close();
	if(cdnFound==false)
		cout<<"MetaServer::deleteCdnFromMetaEntry - "+cdnAddr.ipAddr+" does not contain "+fileName<<endl;
	if(fileFound==false)
		cout<<"MetaServer::deleteCdnFromMetaEntry - "+fileName+" is not found for delete CDN operation"<<endl;
	if(cdnFound==false || fileFound==false)
		return -1;
	return 0;
}


/*--------------------------*/
/* for timestamp management */
/*--------------------------*/

void MetaServer::updateVersion_timestamp() {
	m_version_timestamp = (m_version_timestamp+1)%10;
}

void parseLine_timestamp(string s, string& fileName, long long& timeStamp) {
	fileName = "";
	timeStamp = -1;
	string curr = "";
	int idx=0;
	while(idx<s.size() && s[idx]!=' ') {
		curr += s[idx];
		++idx;
	}
	++idx;
	fileName = curr;
	curr = "";
	while(idx<s.size()) {
		curr = s[idx];
		++idx;
	}
	timeStamp = stoll(curr);
}

//only considers files that exist in both client and fss
int MetaServer::processSyncWithTimeStamp(const vector< pair<string, long long> >& clientFileList, //<file name, file timestamp>
										 vector<string>& uploadList, vector<string>& downloadList, vector<string>& deleteList) {
	uploadList.clear();
	downloadList.clear();
	deleteList.clear();
	string currFileName = m_file_timestamp+"_v" + char('0'+m_version_timestamp);
	ifstream file(currFileName.c_str());
	if(!file.is_open()) {
		cout<<"MetaServer::processSyncWithTimeStamp - file not opened"<<endl;
		return -1;
	}
	unordered_map<string, long long> clientNameToTimestampMap;
	for(int i=0; i<clientFileList.size(); i++)
		clientNameToTimestampMap[clientFileList[i].first] = clientFileList[i].second;
	string currLine;
	while(getline(file, currLine)) {
		string fileName;
		long long timeStampFSS;
		parseLine_timestamp(currLine, fileName, timeStampFSS);
		if(clientNameToTimestampMap.count(fileName) > 0) {
			if(clientNameToTimestampMap[fileName] > timeStampFSS) { //client has more recent version; client needs to upload it to fss
				uploadList.push_back(fileName);
			} else { //fss has more recent version; client needs to download it from fss
				downloadList.push_back(fileName);
			}
		}
	}
	return 0;
}

int MetaServer::addNewTimeStamp(string fileName, long long timeStamp) {
	string currFileName = m_file_timestamp+"_v" + char('0'+m_version_timestamp);
	ofstream file(currFileName.c_str(), ios_base::app | ios_base::out);
	if(!file.is_open()) {
		cout<<"MetaServer::addNewTimeStamp - file not opened"<<endl;
		return -1;
	}
	string lineToAdd = fileName + " " + to_string(timeStamp) + "\n";
	file << lineToAdd;
	file.close();
	return 0;
}

int MetaServer::updateTimeStamp(string fileName, long long timeStamp) {
	if(deleteTimeStamp(fileName)==-1)
		return -1;
	if(addNewTimeStamp(fileName, timeStamp)==-1)
		return -1;
	return 0;
}

int MetaServer::deleteTimeStamp(string fileName) {
	string currFileName = m_file_timestamp+"_v" + char('0'+m_version_timestamp);
	updateVersion_timestamp();
	string newFileName = m_file_timestamp+"_v" + char('0'+m_version_timestamp);
	ifstream fileIn(currFileName.c_str());
	if(!fileIn) {
		cout<<"MetaServer::updateTimeStamp - file not opened"<<endl;
		return -1;
	}
	ofstream fileOut(newFileName.c_str());
	if(!fileOut) {
		cout<<"MetaServer::updateTimeStamp - file not opened"<<endl;
		fileIn.close();
		return -1;
	}
	bool fileFound = false;
	string currLine;
	while(getline(fileIn, currLine)) {
		if(currLine.substr(0, fileName.size())==fileName && currLine[fileName.size()]==' ') {
			fileFound = true;
			continue;
		}
		fileOut << currLine+"\n";
	}

	fileIn.close();
	fileOut.close();
	if(fileFound==false) {
		cout<<"MetaServer::deleteTimeStamp - "+fileName+" is not found"<<endl;
		return -1;
	}
	return 0;
}