#pragma once

#include <string>

#define	FEATURELISTSIZE	100
#define LEN_SN 7
#define LEN_DATE 6

class CHVHASPManager
{
public:
    int level_basic()                    { return 0; }
    int level_standard()                 { return 1; }
    int level_upright()                  { return 3; }
    int level_measure()                  { return 4; }
    int level_advanced()                 { return 5; }
    int level_automation()               { return 6; }
	int level_hiis()					 { return 8; }
    int level_professional()             { return 2; }
    int level_basic_noDevice()           { return 50; }
    int level_standard_noDevice()        { return 51; }
    int level_advanced_noDevice()        { return 55; }
    int level_automation_noDevice()      { return 56; }
    int level_professional_noDevice()    { return 52; }
    int level_super()                    { return 99; }
	int level_hdrmate()                  { return 77; }

public:
	CHVHASPManager(void);
	~CHVHASPManager(void);

private:
	int DongleID;
	int DongleVersion;
	char* strVersion;

public:
	bool Init();
	int  HASPLogin();
	bool CheckConnectDongle();
	bool IsValidFeature(int* module);
	bool GetDongleFeatureIDList();
	bool GetCompanyName(void* buffer, /*unsigned int buffer_size, */size_t& len);
	bool GetDongleVersion(void* buffer, size_t& len);
	bool DongleVersionToString();
	int  GetDongleID();
	bool RealTimeCheck(int* module);
	bool SetCompanyName(void* buffer, size_t len);
	bool SetProductInfo(char sn[LEN_SN], char date[LEN_DATE]);
	bool GetProductInfo(std::string& out_sn, std::string& out_date);
};
